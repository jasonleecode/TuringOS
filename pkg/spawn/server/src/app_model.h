#pragma once

#include <l4/re/dataspace>
#include <l4/re/rm>
#include <l4/re/mem_alloc>
#include <l4/re/env>
#include <l4/re/log>
#include <l4/re/namespace>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/unique_cap>
#include <l4/re/util/env_ns>
#include <l4/re/error_helper>
#include <l4/re/l4aux.h>
#include <l4/sys/factory>
#include <l4/sys/scheduler>
#include <l4/sys/thread>
#include <l4/sys/task>
#include <l4/libloader/loader>
#include <l4/libloader/remote_app_model>
#include <l4/libloader/remote_mem>
#include <l4/libloader/elf>
#include <l4/sys/ipc_gate>
#include <cstring>
#include <cstdio>

// l4re_aux is defined in main.cc and set from the program's AUX vector.
extern l4re_aux_t const *l4re_aux;

// ============================================================================
// Silent debug sink for libloader
// ============================================================================
struct Spawn_dbg {
  void printf(const char *, ...) const __attribute__((format(printf, 2, 3))) {}
  void cprintf(const char *, ...) const __attribute__((format(printf, 2, 3))) {}
};

// ============================================================================
// Stack for the remote (child) application
// ============================================================================

class Spawn_stack : public Ldr::Remote_stack<>
{
public:
  Spawn_stack() : Ldr::Remote_stack<>(nullptr), _vma(nullptr), _vma_size(0) {}
  ~Spawn_stack();

  // Map the stack dataspace locally so we can build the startup stack.
  // Called from Spawn_model_base::alloc_app_stack().
  void set_stack(L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap const &ds,
                 l4_size_t size);

private:
  void      *_vma;
  l4_size_t  _vma_size;
};

// ============================================================================
// Application model base — implements ALL methods called by Remote_app_model.
// Follows ned's App_model pattern: extends Base_app_model<Stack>, defines
// Dataspace, and owns the child caps so prog_attach_ds/get_task_caps work.
// ============================================================================

struct Spawn_model_base : public Ldr::Base_app_model<Spawn_stack>
{
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Dataspace;
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Const_dataspace;

  enum { Utcb_area_start = 0xb3000000UL };

  // spawnd's own service cap, forwarded to every child as the named cap
  // "spawnd" so children can call back to spawnd (e.g. exec()).  Set once at
  // spawnd startup (main.cc); L4_INVALID_CAP disables forwarding.
  static l4_cap_idx_t s_spawnd_cap;

  // Optional per-spawn console cap: when valid, the child's "log" (stdin/stdout)
  // is this cap instead of spawnd's own serial console.  Used by telnetd to run
  // native_shell over a telnet connection (a vcon backed by the TCP socket).
  l4_cap_idx_t _console_cap = L4_INVALID_CAP;
  void set_console_cap(l4_cap_idx_t c) { _console_cap = c; }

  Spawn_model_base();

  // Set the argv/envp that will be pushed onto the child's startup stack.
  void set_args(int argc, const char* const* argv,
                const char* const* envp);

  // ---- libloader interface: file / memory ----

  Const_dataspace open_file(const char* name);

  Dataspace alloc_ds(unsigned long size) const;
  Dataspace alloc_ds_aligned(unsigned long size, unsigned align) const;
  Dataspace alloc_app_stack();

  // ---- libloader interface: local (loader-side) attach / detach ----

  l4_addr_t local_attach_ds(Const_dataspace ds,
                             unsigned long size,
                             unsigned long offset) const;
  void local_detach_ds(l4_addr_t addr, unsigned long size) const;

  // ---- libloader interface: remote (child) VM operations ----

  void prog_attach_ds(l4_addr_t addr, unsigned long size,
                      Const_dataspace ds, unsigned long offset,
                      L4Re::Rm::Flags flags,
                      char const* name, unsigned long file_offset,
                      char const* what);

  int prog_reserve_area(l4_addr_t *start, unsigned long size,
                        L4Re::Rm::Flags flags, unsigned char align);

  static void copy_ds(Dataspace dst, unsigned long dst_offs,
                      Const_dataspace src, unsigned long src_offs,
                      unsigned long size);

  static void ds_map_info(Const_dataspace ds, l4_addr_t *start);

  static bool        all_segs_cow()  { return false; }
  static Const_dataspace reserved_area() { return Const_dataspace(); }

  static Dataspace       local_kip_ds();
  static L4::Cap<void>   local_kip_cap();

  // ---- libloader interface: caps / thread start ----

  // Called by Remote_app_model::alloc_prog() to get pre-allocated cap slots.
  void get_task_caps(L4::Cap<L4::Factory> *factory,
                     L4::Cap<L4::Task>    *task,
                     L4::Cap<L4::Thread>  *thread);

  l4_cap_idx_t push_initial_caps(l4_cap_idx_t start);
  void         map_initial_caps(L4::Cap<L4::Task> task, l4_cap_idx_t start);

  l4_msgtag_t run_thread(L4::Cap<L4::Thread> thread,
                         l4_sched_param_t const &sp);

  // Called by Remote_app_model::launch() just before pushing argv/envp.
  void init_prog();

  // ---- Accessors used by Spawnd after launch ----

  l4_cap_idx_t child_task_idx()   const { return _child_task_cap.cap(); }
  l4_cap_idx_t child_thread_idx() const { return _child_thread_cap.cap(); }
  l4_cap_idx_t child_rm_idx()     const { return _child_rm.get().cap(); }
  l4_cap_idx_t parent_gate_idx()  const { return _parent_gate.get().cap(); }

  // Release ownership of caps so Task_table can take over.
  void release_caps(l4_cap_idx_t *task, l4_cap_idx_t *thread,
                    l4_cap_idx_t *rm,   l4_cap_idx_t *gate);

private:
  void push_argv_strings();
  void push_env_strings();

  // Open a file from the ext4 filesystem and return its dataspace.
  // relpath: path relative to ext4 root, e.g. "bin/hello"
  Const_dataspace open_from_ext4(const char *relpath);

  // Child cap slots — pre-allocated in constructor.
  L4Re::Util::Unique_del_cap<L4::Task>     _child_task_cap;
  L4Re::Util::Unique_del_cap<L4::Thread>   _child_thread_cap;
  L4Re::Util::Unique_del_cap<L4Re::Rm>     _child_rm;
  L4Re::Util::Unique_del_cap<L4::Ipc_gate> _parent_gate;

  // Source argv/envp (not owned)
  int               _argc;
  const char* const* _argv;
  const char* const* _envp;
};

// Spawn_app_model = Base model + Remote_app_model infrastructure (alloc_prog,
// start_prog, add_env, prog_attach_stack, prog_reserve_utcb_area, etc.)
typedef Ldr::Remote_app_model<Spawn_model_base> Spawn_app_model;
