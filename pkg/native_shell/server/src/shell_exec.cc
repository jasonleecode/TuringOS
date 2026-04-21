#include "shell_exec.h"

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
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// l4re_aux is defined in main.cc and set from the program's AUX vector.
extern l4re_aux_t const *l4re_aux;

// ============================================================================
// File_descriptor base
// ============================================================================

File_descriptor::File_descriptor(const char* path) : _path(path)
{
  File_resolver::basename(path, _name, sizeof(_name));
}

// ============================================================================
// Rom_file
// ============================================================================

Rom_file::Rom_file(const char* path,
                   L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap cap)
  : File_descriptor(path), _ds(cap), _size(0)
{
  if (!_ds.is_valid()) return;

  l4_addr_t start = 0, end = 0;
  if (_ds->map_info(&start, &end) >= 0)
    _size = static_cast<size_t>(end - start);
}

L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap
Rom_file::load_as_dataspace()
{
  return _ds;
}

bool Rom_file::exists() const
{
  return _ds.is_valid();
}

// ============================================================================
// File_resolver
// ============================================================================

L4::Cap<L4Re::Namespace> File_resolver::g_programs_ns;

void File_resolver::set_programs_library(L4::Cap<L4Re::Namespace> ns)
{
  g_programs_ns = ns;
}

L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap
File_resolver::open_from_env(const char* name)
{
  L4Re::Util::Env_ns ens;

  // Try "rom/<name>" first
  char rom_path[280];
  if (strncmp(name, "rom/", 4) != 0) {
    snprintf(rom_path, sizeof(rom_path), "rom/%s", name);
  } else {
    strncpy(rom_path, name, sizeof(rom_path) - 1);
    rom_path[sizeof(rom_path) - 1] = '\0';
  }

  L4::Cap<L4Re::Dataspace> cap = ens.query<L4Re::Dataspace>(rom_path);
  if (cap.is_valid())
    return L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap(cap);

  // Also try programs_ns if configured
  if (g_programs_ns.is_valid()) {
    char base[256];
    File_resolver::basename(name, base, sizeof(base));
    L4::Cap<L4Re::Dataspace> ds = L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>();
    if (ds.is_valid()) {
      long r = g_programs_ns->query(base, ds);
      if (r >= 0)
        return L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap(ds);
      L4Re::Util::cap_alloc.free(ds);
    }
  }

  return L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap();
}

File_descriptor* File_resolver::resolve(const char* path)
{
  if (!path || !path[0]) return nullptr;

  auto ds = open_from_env(path);
  if (!ds.is_valid()) {
    fprintf(stderr, "run: program not found: %s\n", path);
    return nullptr;
  }

  return new Rom_file(path, ds);
}

bool File_resolver::file_exists(const char* path)
{
  auto* f = resolve(path);
  if (!f) return false;
  bool ok = f->exists();
  delete f;
  return ok;
}

char* File_resolver::basename(const char* path, char* buf, size_t sz)
{
  if (!path || !buf || sz == 0) return nullptr;
  const char* s = strrchr(path, '/');
  strncpy(buf, s ? s + 1 : path, sz - 1);
  buf[sz - 1] = '\0';
  return buf;
}

// ============================================================================
// Shell_stack
// ============================================================================

Shell_stack::~Shell_stack()
{
  if (_vma) {
    L4Re::Env::env()->rm()->detach(reinterpret_cast<l4_addr_t>(_vma), 0);
    _vma = nullptr;
  }
}

void Shell_stack::set_stack(L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap const &ds,
                             l4_size_t size)
{
  void *vaddr = nullptr;
  L4Re::chksys(
    L4Re::Env::env()->rm()->attach(
      &vaddr, size,
      L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
      L4::Ipc::make_cap_rw(ds.get()), 0),
    "Shell_stack: attach stack DS");

  _vma      = vaddr;
  _vma_size = size;
  set_local_top(static_cast<char*>(vaddr) + size);
}

// ============================================================================
// Shell_model_base
// ============================================================================

Shell_model_base::Shell_model_base()
  : Ldr::Base_app_model<Shell_stack>(),
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

void Shell_model_base::set_args(int argc, const char* const* argv,
                                 const char* const* envp)
{
  _argc = argc;
  _argv = argv;
  _envp = envp;
}

// ------ libloader interface ------

Shell_model_base::Const_dataspace
Shell_model_base::open_file(const char* name)
{
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

  fprintf(stderr, "[shell-exec] open_file: '%s' not found\n", name);
  return Const_dataspace();
}

Shell_model_base::Dataspace
Shell_model_base::alloc_ds(unsigned long size) const
{
  Dataspace mem(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>());
  L4Re::chkcap(mem, "alloc_ds: cap alloc");
  // Loader-side allocation: use our own mem_alloc (prog_info not set yet).
  L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(size, mem.get()),
               "alloc_ds: mem alloc");
  return mem;
}

Shell_model_base::Dataspace
Shell_model_base::alloc_ds_aligned(unsigned long size, unsigned align) const
{
  Dataspace mem(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>());
  L4Re::chkcap(mem, "alloc_ds_aligned: cap alloc");
  L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(size, mem.get(), 0, align),
               "alloc_ds_aligned");
  return mem;
}

Shell_model_base::Dataspace
Shell_model_base::alloc_app_stack()
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

void Shell_model_base::init_prog()
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
Shell_model_base::local_attach_ds(Const_dataspace ds,
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

void Shell_model_base::local_detach_ds(l4_addr_t addr, unsigned long) const
{
  L4Re::chksys(L4Re::Env::env()->rm()->detach(l4_trunc_page(addr), 0),
               "local_detach_ds");
}

void Shell_model_base::prog_attach_ds(l4_addr_t addr, unsigned long size,
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

int Shell_model_base::prog_reserve_area(l4_addr_t *start, unsigned long size,
                                         L4Re::Rm::Flags flags,
                                         unsigned char align)
{
  return _child_rm.get()->reserve_area(start, size, flags, align);
}

void Shell_model_base::copy_ds(Dataspace dst, unsigned long dst_offs,
                                Const_dataspace src, unsigned long src_offs,
                                unsigned long size)
{
  L4Re::chksys(dst->copy_in(dst_offs, src.get(), src_offs, size),
               "copy_ds");
}

void Shell_model_base::ds_map_info(Const_dataspace ds, l4_addr_t *start)
{
  l4_addr_t unused_end;
  L4Re::chksys(ds->map_info(start, &unused_end), "ds_map_info");
}

Shell_model_base::Dataspace Shell_model_base::local_kip_ds()
{
  return Dataspace(L4::Cap<L4Re::Dataspace>(l4re_aux->kip_ds));
}

L4::Cap<void> Shell_model_base::local_kip_cap()
{
  return local_kip_ds().get();
}

void Shell_model_base::get_task_caps(L4::Cap<L4::Factory> *factory,
                                      L4::Cap<L4::Task>    *task,
                                      L4::Cap<L4::Thread>  *thread)
{
  *factory = L4Re::Env::env()->factory();
  *task    = _child_task_cap.get();
  *thread  = _child_thread_cap.get();
}

l4_cap_idx_t Shell_model_base::push_initial_caps(l4_cap_idx_t start)
{
  return start;
}

void Shell_model_base::map_initial_caps(L4::Cap<L4::Task>, l4_cap_idx_t)
{
}

l4_msgtag_t Shell_model_base::run_thread(L4::Cap<L4::Thread> thread,
                                          l4_sched_param_t const &sp)
{
  L4::Cap<L4::Scheduler> s(prog_info()->scheduler.raw & L4_FPAGE_ADDR_MASK);
  return s->run_thread(thread, sp);
}

void Shell_model_base::push_argv_strings()
{
  argv.a0 = nullptr;
  if (_argc <= 0 || !_argv) return;

  for (int i = 0; i < _argc && _argv[i]; i++) {
    const char *pushed = _stack.push_str(_argv[i], strlen(_argv[i]));
    argv.al = pushed;
    if (i == 0) argv.a0 = pushed;
  }
}

void Shell_model_base::push_env_strings()
{
  envp.a0 = nullptr;
  if (!_envp) return;

  for (int i = 0; _envp[i]; i++) {
    const char *pushed = _stack.push_str(_envp[i], strlen(_envp[i]));
    envp.al = pushed;
    if (i == 0) envp.a0 = pushed;
  }
}

void Shell_model_base::release_caps(l4_cap_idx_t *task,
                                     l4_cap_idx_t *thread,
                                     l4_cap_idx_t *rm,
                                     l4_cap_idx_t *gate)
{
  *task   = _child_task_cap.release().cap();
  *thread = _child_thread_cap.release().cap();
  *rm     = _child_rm.release().cap();
  *gate   = _parent_gate.release().cap();
}

// ============================================================================
// Simple_task_manager
// ============================================================================

Simple_task_manager::Simple_task_manager() : _task_count(0)
{
  memset(_tasks, 0, sizeof(_tasks));
}

Simple_task_manager::~Simple_task_manager()
{
  for (int i = 0; i < MAX_TASKS; i++) {
    if (_tasks[i].running)
      kill(&_tasks[i]);
  }
}

Simple_task_manager::Task_handle*
Simple_task_manager::alloc_task_handle()
{
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!_tasks[i].running) {
      _tasks[i].running  = true;
      _tasks[i].task_cap = L4_INVALID_CAP;
      _task_count = std::max(_task_count, i + 1);
      return &_tasks[i];
    }
  }
  return nullptr;
}

void Simple_task_manager::free_task_handle(Task_handle* task)
{
  task->running         = false;
  task->task_cap        = L4_INVALID_CAP;
  task->thread_cap      = L4_INVALID_CAP;
  task->rm_cap          = L4_INVALID_CAP;
  task->parent_gate_cap = L4_INVALID_CAP;
  task->name[0]         = '\0';
}

Simple_task_manager::Task_handle*
Simple_task_manager::spawn(const char* program,
                            char* const* argv,
                            char* const* envp)
{
  printf("[shell-exec] spawning: %s\n", program);

  Task_handle* handle = alloc_task_handle();
  if (!handle) {
    fprintf(stderr, "run: no free task slots\n");
    return nullptr;
  }

  try {
    Simple_app_model am;

    int argc = 0;
    if (argv) while (argv[argc]) argc++;

    am.set_args(argc,
                const_cast<const char* const*>(argv),
                const_cast<const char* const*>(envp));

    typedef Ldr::Elf_loader<Simple_app_model, Shell_dbg> My_loader;
    Shell_dbg dbg;
    My_loader loader;

    loader.launch(&am, program, dbg);

    am.release_caps(&handle->task_cap,
                    &handle->thread_cap,
                    &handle->rm_cap,
                    &handle->parent_gate_cap);

    snprintf(handle->name, sizeof(handle->name), "%s", program);

    printf("[shell-exec] spawned: %s (task=%lu)\n",
           program, handle->task_cap);
    return handle;

  } catch (L4::Runtime_error const &e) {
    fprintf(stderr, "run: launch failed: %s (%s)\n", e.str(), e.extra_str());
    free_task_handle(handle);
    return nullptr;
  } catch (...) {
    fprintf(stderr, "run: launch failed (unknown exception)\n");
    free_task_handle(handle);
    return nullptr;
  }
}

int Simple_task_manager::wait(Task_handle* task)
{
  if (!task || !task->running) return 0;
  if (!l4_is_valid_cap(task->parent_gate_cap)) {
    task->running = false;
    return 0;
  }

  l4_msg_regs_t *mr = l4_utcb_mr();
  l4_msgtag_t tag = l4_ipc_receive(task->parent_gate_cap, l4_utcb(),
                                    L4_IPC_NEVER);

  task->running = false;

  if (l4_msgtag_has_error(tag)) {
    fprintf(stderr, "[shell-exec] wait: IPC error %ld\n",
            l4_ipc_error(tag, l4_utcb()));
    return -1;
  }

  int nwords = l4_msgtag_words(tag);
  long exit_code = 0;
  if (nwords >= 2)
    exit_code = static_cast<long>(mr->mr[1]);
  else if (nwords >= 1)
    exit_code = static_cast<long>(mr->mr[0]);

  return static_cast<int>(exit_code);
}

void Simple_task_manager::kill(Task_handle* task)
{
  if (!task || !task->running) return;

  task->running = false;

  auto del = [](l4_cap_idx_t idx) {
    if (!l4_is_valid_cap(idx)) return;
    L4Re::Env::env()->task()->unmap(
      l4_obj_fpage(idx, 0, L4_FPAGE_RWX),
      L4_FP_DELETE_OBJ | L4_FP_ALL_SPACES);
    L4Re::Util::cap_alloc.free(L4::Cap<void>(idx));
  };

  del(task->task_cap);
  del(task->thread_cap);
  del(task->rm_cap);
  del(task->parent_gate_cap);

  task->task_cap        = L4_INVALID_CAP;
  task->thread_cap      = L4_INVALID_CAP;
  task->rm_cap          = L4_INVALID_CAP;
  task->parent_gate_cap = L4_INVALID_CAP;
}
