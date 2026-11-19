#include <l4/l4re_vfs/backend>
#include <l4/re/env>
#include <l4/util/util.h>
#include <l4/turingos/spawn_ipc.h>
#include <l4/turingos/task_io.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr unsigned long handle_request = 0x545001;
class Pipe_file : public L4Re::Vfs::Be_file {
  L4::Cap<Spawn_svr> server;
  unsigned id;
public:
  Pipe_file(L4::Cap<Spawn_svr> s, unsigned handle) : server(s), id(handle)
  {}
  ~Pipe_file() noexcept
  {
    server->pipe_close(id);
  }

  int ioctl(unsigned long op, va_list) noexcept override
  {
    return op == handle_request ? (int)id : -ENOTTY;
  }

  ssize_t readv(const iovec *v, int count) noexcept override
  {
    for (int i = 0; i < count; ++i) {
      if (!v[i].iov_len) continue;
      unsigned n = std::min<size_t>(128, v[i].iov_len);
      for (;;) {
        L4::Ipc::Array<char> bytes(n, static_cast<char *>(v[i].iov_base));
        long rc = server->pipe_read(id, n, bytes);
        if (rc != -L4_EAGAIN) return rc < 0 ? rc : bytes.length;
        l4_sleep(1);
      }
    }
    return 0;
  }

  ssize_t writev(const iovec *v, int count) noexcept override
  {
    ssize_t total = 0;
    for (int i = 0; i < count; ++i) {
      auto p = static_cast<const char *>(v[i].iov_base);
      size_t left = v[i].iov_len;
      while (left) {
        unsigned n = std::min<size_t>(left, 128);
        long rc = server->pipe_write(id, L4::Ipc::Array<char const>(n, p));
        if (rc == -L4_EAGAIN) {
          l4_sleep(1);
          continue;
        }
        if (rc <= 0) return total ? total : rc;
        p += rc;
        left -= rc;
        total += rc;
      }
    }
    return total;
  }
};

int install(L4::Cap<Spawn_svr> server, unsigned id, int target = -1)
{
  auto file = cxx::make_ref_obj<Pipe_file>(server, id);
  if (!file) {
    server->pipe_close(id);
    return -ENOMEM;
  }
  if (target < 0) return L4Re::Vfs::vfs_ops->alloc_fd(file);
  return -L4Re::Vfs::vfs_ops->set_fd(target, file).second;
}
struct Standard_streams {
  Standard_streams()
  {
    auto server = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
    if (!server.is_valid()) return;
    if (auto cwd = getenv("PWD")) chdir(cwd);
    const char *names[] = {"TURING_STDIN", "TURING_STDOUT"};
    for (int fd = 0; fd < 2; ++fd) {
      auto value = getenv(names[fd]);
      if (value && atoi(value) > 0 && install(server, atoi(value), fd) < 0) _exit(126);
    }
  }
} init __attribute__((init_priority(INIT_PRIO_LATE + 3)));
}

extern "C" void task_io_init()
{}

int task_pipe_handle(int fd)
{
  return ioctl(fd, handle_request);
}

int pipe(int fds[2]) noexcept
{
  auto svr = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  if (!svr.is_valid()) {
    errno = ENOSYS;
    return -1;
  }
  unsigned r, w;
  long rc = svr->pipe_create(r, w);
  if (rc < 0) {
    errno = -rc;
    return -1;
  }
  fds[0] = install(svr, r);
  if (fds[0] < 0) {
    svr->pipe_close(w);
    errno = EMFILE;
    return -1;
  }
  fds[1] = install(svr, w);
  if (fds[1] < 0) {
    close(fds[0]);
    errno = EMFILE;
    return -1;
  }
  return 0;
}
