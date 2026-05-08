#include "app_model.h"

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/unique_cap>
#include <l4/re/util/env_ns>
#include <l4/re/error_helper>
#include <l4/re/mem_alloc>
#include <l4/sys/factory>
#include <l4/sys/scheduler>
#include <l4/sys/thread>
#include <l4/sys/task>
#include <l4/sys/ipc.h>
#include <l4/re/l4aux.h>
#include <ext4_file_proto.h>
#include <cstring>
#include <cstdio>

// l4re_aux is defined in main.cc and set from the program's AUX vector.
extern l4re_aux_t const *l4re_aux;

// ============================================================================
// Spawn_stack
// ============================================================================

Spawn_stack::~Spawn_stack()
{
  if (_vma) {
    L4Re::Env::env()->rm()->detach(reinterpret_cast<l4_addr_t>(_vma), 0);
    _vma = nullptr;
  }
}

void Spawn_stack::set_stack(L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap const &ds,
                             l4_size_t size)
{
  void *vaddr = nullptr;
  L4Re::chksys(
    L4Re::Env::env()->rm()->attach(
      &vaddr, size,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
      L4::Ipc::make_cap_rw(ds.get()), 0),
    "Spawn_stack: attach stack DS");

  _vma      = vaddr;
  _vma_size = size;
  set_local_top(static_cast<char*>(vaddr) + size);
}

// ============================================================================
// Spawn_model_base
// ============================================================================

Spawn_model_base::Spawn_model_base()
  : Ldr::Base_app_model<Spawn_stack>(),
    _argc(0), _argv(nullptr), _envp(nullptr)
{
  using L4Re::chkcap;
  using L4Re::chksys;
  auto env = L4Re::Env::env();

  prog_info()->utcbs_start    = Utcb_area_start;
  prog_info()->utcbs_log2size = L4_PAGESHIFT;
  prog_info()->kip   = reinterpret_cast<l4_addr_t>(l4re_kip());
  prog_info()->ldr_flags = 0;
  prog_info()->l4re_dbg  = 0;

  _child_task_cap   = chkcap(L4Re::Util::make_unique_del_cap<L4::Task>(),
                              "alloc child task cap");
  _child_thread_cap = chkcap(L4Re::Util::make_unique_del_cap<L4::Thread>(),
                              "alloc child thread cap");

  _child_rm = chkcap(L4Re::Util::make_unique_del_cap<L4Re::Rm>(),
                     "alloc child rm cap");
  chksys(env->user_factory()->create(_child_rm.get()), "create child RM");

  _parent_gate = chkcap(L4Re::Util::make_unique_del_cap<L4::Ipc_gate>(),
                        "alloc parent gate cap");
  chksys(env->factory()->create(_parent_gate.get()), "create parent gate");
}

void Spawn_model_base::set_args(int argc, const char* const* argv,
                                 const char* const* envp)
{
  _argc = argc;
  _argv = argv;
  _envp = envp;
}

// ------ libloader interface ------

Spawn_model_base::Const_dataspace
Spawn_model_base::open_file(const char* name)
{
  // Phase 3: ext4 absolute paths like "/ext4/bin/hello"
  if (strncmp(name, "/ext4/", 6) == 0)
    return open_from_ext4(name + 6);

  // Phase 1: ROM — try "rom/<name>" then bare "<name>" in Env_ns.
  L4Re::Util::Env_ns ens;
  char rom_path[280];
  if (strncmp(name, "rom/", 4) != 0)
    snprintf(rom_path, sizeof(rom_path), "rom/%s", name);
  else {
    strncpy(rom_path, name, sizeof(rom_path) - 1);
    rom_path[sizeof(rom_path) - 1] = '\0';
  }

  L4::Cap<L4Re::Dataspace> cap = ens.query<L4Re::Dataspace>(rom_path);
  if (cap.is_valid())
    return Const_dataspace(cap);

  cap = ens.query<L4Re::Dataspace>(name);
  if (cap.is_valid())
    return Const_dataspace(cap);

  fprintf(stderr, "[spawnd] open_file: '%s' not found\n", name);
  return Const_dataspace();
}

Spawn_model_base::Const_dataspace
Spawn_model_base::open_from_ext4(const char *relpath)
{
  // Build env namespace query path "ext4/<relpath>".
  // Env_ns handles multi-component Partly_resolved responses automatically,
  // traversing the Ext4_namespace tree until it reaches a file object.
  char qpath[520];
  snprintf(qpath, sizeof(qpath), "ext4/%s", relpath);

  L4Re::Util::Env_ns ens;
  L4::Cap<Ext4_file_ops> raw = ens.query<Ext4_file_ops>(qpath);
  if (!raw.is_valid()) {
    fprintf(stderr, "[spawnd] open_from_ext4: '%s' not found\n", relpath);
    return Const_dataspace();
  }
  // Take ownership so the cap slot is freed when this scope exits.
  L4Re::Util::Unique_del_cap<Ext4_file_ops> fops(raw);

  // Allocate a DS cap slot and receive the file dataspace from the server.
  Const_dataspace ds(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>());
  if (!ds.is_valid()) {
    fprintf(stderr, "[spawnd] open_from_ext4: DS cap alloc failed\n");
    return Const_dataspace();
  }

  l4_uint64_t fsize = 0;
  long r = fops.get()->get_ds(ds.get(), fsize);

  // Read-only load: tell the server to close without flushing.
  fops.get()->close(0);

  if (r < 0) {
    fprintf(stderr, "[spawnd] open_from_ext4: get_ds failed (%ld)\n", r);
    return Const_dataspace();
  }

  printf("[spawnd] open_from_ext4: '%s' %llu bytes\n", relpath, fsize);
  return ds;
}

Spawn_model_base::Dataspace
Spawn_model_base::alloc_ds(unsigned long size) const
{
  Dataspace mem(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>());
  L4Re::chkcap(mem, "alloc_ds: cap alloc");
  // Loader-side allocation: use our own mem_alloc (prog_info not set yet).
  L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(size, mem.get()),
               "alloc_ds: mem alloc");
  return mem;
}

Spawn_model_base::Dataspace
Spawn_model_base::alloc_ds_aligned(unsigned long size, unsigned align) const
{
  Dataspace mem(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>());
  L4Re::chkcap(mem, "alloc_ds_aligned: cap alloc");
  L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(size, mem.get(), 0, align),
               "alloc_ds_aligned");
  return mem;
}

Spawn_model_base::Dataspace
Spawn_model_base::alloc_app_stack()
{
  using L4Re::chkcap;
  using L4Re::chksys;

  Dataspace stack(chkcap(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>(),
                         "alloc stack cap"));
  chksys(L4Re::Env::env()->mem_alloc()->alloc(_stack.stack_size(), stack.get()),
         "alloc stack");
  _stack.set_stack(stack, _stack.stack_size());
  return stack;
}

void Spawn_model_base::init_prog()
{
  auto env = L4Re::Env::env();

  prog_info()->mem_alloc = env->mem_alloc().fpage(L4_CAP_FPAGE_RWS);
  prog_info()->log       = env->log().fpage(L4_CAP_FPAGE_RWS);
  prog_info()->factory   = env->factory().fpage(L4_CAP_FPAGE_RWS);
  prog_info()->scheduler = env->scheduler().fpage(L4_CAP_FPAGE_RWS);
  prog_info()->rm        = _child_rm.get().fpage(L4_CAP_FPAGE_RWS);
  prog_info()->parent    = _parent_gate.get().fpage(L4_CAP_FPAGE_RWS);

  push_argv_strings();
  push_env_strings();
}

l4_addr_t
Spawn_model_base::local_attach_ds(Const_dataspace ds,
                                   unsigned long size,
                                   unsigned long offset) const
{
  auto rm = L4Re::Env::env()->rm();
  l4_addr_t pg_off    = l4_trunc_page(offset);
  l4_addr_t in_pg_off = offset - pg_off;
  unsigned long pg_sz = l4_round_page(size + in_pg_off);
  l4_addr_t vaddr = 0;
  L4Re::chksys(rm->attach(&vaddr, pg_sz,
                           L4Re::Rm::F::Search_addr | L4Re::Rm::F::R,
                           ds.get(), pg_off),
               "local_attach_ds");
  return vaddr + in_pg_off;
}

void Spawn_model_base::local_detach_ds(l4_addr_t addr, unsigned long) const
{
  L4Re::chksys(L4Re::Env::env()->rm()->detach(l4_trunc_page(addr), 0),
               "local_detach_ds");
}

void Spawn_model_base::prog_attach_ds(l4_addr_t addr, unsigned long size,
                                       Const_dataspace ds, unsigned long offset,
                                       L4Re::Rm::Flags flags,
                                       char const* name,
                                       unsigned long file_offset,
                                       char const* what)
{
  l4_addr_t _addr = addr;
  L4Re::chksys(
    _child_rm.get()->attach(
      &_addr, size, flags,
      L4::Ipc::make_cap(ds.get(), flags.cap_rights()),
      offset, 0, L4::Cap<L4::Task>(_child_task_cap.cap()),
      name, file_offset),
    what);
}

int Spawn_model_base::prog_reserve_area(l4_addr_t *start, unsigned long size,
                                         L4Re::Rm::Flags flags,
                                         unsigned char align)
{
  return _child_rm.get()->reserve_area(start, size, flags, align);
}

void Spawn_model_base::copy_ds(Dataspace dst, unsigned long dst_offs,
                                Const_dataspace src, unsigned long src_offs,
                                unsigned long size)
{
  L4Re::chksys(dst->copy_in(dst_offs, src.get(), src_offs, size),
               "copy_ds");
}

void Spawn_model_base::ds_map_info(Const_dataspace ds, l4_addr_t *start)
{
  l4_addr_t unused_end;
  L4Re::chksys(ds->map_info(start, &unused_end), "ds_map_info");
}

Spawn_model_base::Dataspace Spawn_model_base::local_kip_ds()
{
  return Dataspace(L4::Cap<L4Re::Dataspace>(l4re_aux->kip_ds));
}

L4::Cap<void> Spawn_model_base::local_kip_cap()
{
  return local_kip_ds().get();
}

void Spawn_model_base::get_task_caps(L4::Cap<L4::Factory> *factory,
                                      L4::Cap<L4::Task>    *task,
                                      L4::Cap<L4::Thread>  *thread)
{
  *factory = L4Re::Env::env()->factory();
  *task    = _child_task_cap.get();
  *thread  = _child_thread_cap.get();
}

l4_cap_idx_t Spawn_model_base::push_initial_caps(l4_cap_idx_t start)
{
  l4re_env_cap_entry_t const *e = L4Re::Env::env()->initial_caps();
  for (; e && e->flags != ~0UL; ++e) {
    if (!l4_is_valid_cap(e->cap)) continue;
    l4re_env_cap_entry_t child_e;
    child_e.cap   = start;
    child_e.flags = 0;
    memcpy(child_e.name, e->name, sizeof(e->name));
    _stack.push(child_e);
    start += L4_CAP_OFFSET;
  }
  return start;
}

void Spawn_model_base::map_initial_caps(L4::Cap<L4::Task> task, l4_cap_idx_t start)
{
  l4re_env_cap_entry_t const *e = L4Re::Env::env()->initial_caps();
  for (; e && e->flags != ~0UL; ++e) {
    if (!l4_is_valid_cap(e->cap)) continue;
    task->map(L4Re::This_task,
              L4::Cap<void>(e->cap).fpage(L4_CAP_FPAGE_RWS),
              L4::Cap<void>(start).snd_base());
    start += L4_CAP_OFFSET;
  }
}

l4_msgtag_t Spawn_model_base::run_thread(L4::Cap<L4::Thread> thread,
                                          l4_sched_param_t const &sp)
{
  L4::Cap<L4::Scheduler> s(prog_info()->scheduler.raw & L4_FPAGE_ADDR_MASK);
  return s->run_thread(thread, sp);
}

void Spawn_model_base::push_argv_strings()
{
  argv.a0 = nullptr;
  if (_argc <= 0 || !_argv) return;

  for (int i = 0; i < _argc && _argv[i]; i++) {
    const char *pushed = _stack.push_str(_argv[i], strlen(_argv[i]));
    argv.al = pushed;
    if (i == 0) argv.a0 = pushed;
  }
}

void Spawn_model_base::push_env_strings()
{
  envp.a0 = nullptr;
  if (!_envp) return;

  for (int i = 0; _envp[i]; i++) {
    const char *pushed = _stack.push_str(_envp[i], strlen(_envp[i]));
    envp.al = pushed;
    if (i == 0) envp.a0 = pushed;
  }
}

void Spawn_model_base::release_caps(l4_cap_idx_t *task,
                                     l4_cap_idx_t *thread,
                                     l4_cap_idx_t *rm,
                                     l4_cap_idx_t *gate)
{
  *task   = _child_task_cap.release().cap();
  *thread = _child_thread_cap.release().cap();
  *rm     = _child_rm.release().cap();
  *gate   = _parent_gate.release().cap();
}
