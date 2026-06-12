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

// ============================================================================
// Silent debug sink for libloader
// ============================================================================
struct Shell_dbg {
  void printf(const char *, ...) const __attribute__((format(printf, 2, 3))) {}
  void cprintf(const char *, ...) const __attribute__((format(printf, 2, 3))) {}
};

// ============================================================================
// File storage abstraction
// ============================================================================

enum class File_backend { ROM, EXT4, MEMORY };

class File_descriptor {
public:
  virtual ~File_descriptor() = default;
  virtual L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap load_as_dataspace() = 0;
  virtual size_t  size()    const = 0;
  virtual bool    exists()  const = 0;
  virtual File_backend backend() const = 0;
  const char* path() const { return _path; }
  const char* name() const { return _name; }
protected:
  explicit File_descriptor(const char* path);
  const char* _path;
  char        _name[256];
};

class Rom_file : public File_descriptor {
public:
  explicit Rom_file(const char* path,
                    L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap cap);
  L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap load_as_dataspace() override;
  size_t  size()   const override { return _size; }
  bool    exists() const override;
  File_backend backend() const override { return File_backend::ROM; }
private:
  L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap _ds;
  size_t _size;
};

class File_resolver {
public:
  // Called once at startup; programs_ns may be invalid (graceful fallback)
  static void set_programs_library(L4::Cap<L4Re::Namespace> programs_ns);

  // Resolve a path to a File_descriptor; caller owns the returned pointer
  static File_descriptor* resolve(const char* path);

  static bool file_exists(const char* path);

  // Try "rom/<name>" then "<name>" in the environment namespace; returns an
  // invalid cap (quietly) if not found.  Used for PATH-style auto-exec.
  static L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap open_from_env(const char* name);

  // Extract basename from a path into buf (three-arg version avoids conflict
  // with the POSIX basename(char*))
  static char* basename(const char* path, char* buf, size_t sz);

private:
  static L4::Cap<L4Re::Namespace> g_programs_ns;
};

// ============================================================================
// Stack for the remote (child) application
// ============================================================================

class Shell_stack : public Ldr::Remote_stack<>
{
public:
  Shell_stack() : Ldr::Remote_stack<>(nullptr), _vma(nullptr), _vma_size(0) {}
  ~Shell_stack();

  // Map the stack dataspace locally so we can build the startup stack.
  // Called from Shell_model_base::alloc_app_stack().
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

struct Shell_model_base : public Ldr::Base_app_model<Shell_stack>
{
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Dataspace;
  typedef L4Re::Util::Ref_cap<L4Re::Dataspace>::Cap Const_dataspace;

  enum { Utcb_area_start = 0xb3000000UL };

  Shell_model_base();

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

  // ---- Accessors used by Simple_task_manager after launch ----

  l4_cap_idx_t child_task_idx()     const { return _child_task_cap.cap(); }
  l4_cap_idx_t child_thread_idx()   const { return _child_thread_cap.cap(); }
  l4_cap_idx_t child_rm_idx()       const { return _child_rm.get().cap(); }
  l4_cap_idx_t parent_gate_idx()    const { return _parent_gate.get().cap(); }

  // Release ownership of caps so Task_handle can take over.
  void release_caps(l4_cap_idx_t *task, l4_cap_idx_t *thread,
                    l4_cap_idx_t *rm,   l4_cap_idx_t *gate);

private:
  void push_argv_strings();
  void push_env_strings();

  // Child cap slots — pre-allocated in constructor.
  L4Re::Util::Unique_del_cap<L4::Task>   _child_task_cap;
  L4Re::Util::Unique_del_cap<L4::Thread> _child_thread_cap;
  L4Re::Util::Unique_del_cap<L4Re::Rm>   _child_rm;
  L4Re::Util::Unique_del_cap<L4::Ipc_gate> _parent_gate;

  // Source argv/envp (not owned)
  int              _argc;
  const char* const* _argv;
  const char* const* _envp;
};

// Simple_app_model = Base model + Remote_app_model infrastructure (alloc_prog,
// start_prog, add_env, prog_attach_stack, prog_reserve_utcb_area, etc.)
typedef Ldr::Remote_app_model<Shell_model_base> Simple_app_model;

// ============================================================================
// Task manager
// ============================================================================

class Simple_task_manager {
public:
  struct Task_handle {
    l4_cap_idx_t task_cap;
    l4_cap_idx_t thread_cap;
    l4_cap_idx_t rm_cap;
    l4_cap_idx_t parent_gate_cap;
    char  name[64];
    bool  running;
  };

  Simple_task_manager();
  ~Simple_task_manager();

  // Launch a program; returns handle on success, nullptr on failure.
  Task_handle* spawn(const char* program,
                     char* const* argv,
                     char* const* envp);

  // Block until the task sends its exit signal; returns exit code.
  int wait(Task_handle* task);

  // Terminate the task immediately.
  void kill(Task_handle* task);

  bool is_running(Task_handle* task) const { return task && task->running; }

private:
  static constexpr int MAX_TASKS = 16;
  Task_handle _tasks[MAX_TASKS];
  int         _task_count;

  Task_handle* alloc_task_handle();
  void         free_task_handle(Task_handle* task);
};
