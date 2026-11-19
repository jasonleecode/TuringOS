// IPv4 TCP client sockets backed by netd. libc routes connected fds through VFS.
#include <l4/l4re_vfs/backend>
#include <l4/re/env>
#include <l4/turingos/net_ipc.h>
#include <netinet/in.h>
#include <algorithm>
#include <cerrno>

namespace {
class Tcp_file : public L4Re::Vfs::Be_file {
  L4::Cap<Net_svr> net;
  unsigned handle = 0;
  bool connected = false;
public:
  explicit Tcp_file(L4::Cap<Net_svr> cap) : net(cap)
  {}
  ~Tcp_file() noexcept
  {
    if (connected) net->close(handle);
  }

  int connect(const sockaddr *addr, socklen_t len) noexcept override
  {
    if (connected) return -EISCONN;
    if (!addr || len < sizeof(sockaddr_in)) return -EINVAL;
    if (addr->sa_family != AF_INET) return -EAFNOSUPPORT;
    auto in = reinterpret_cast<const sockaddr_in *>(addr);
    long r = net->tcp_connect(in->sin_addr.s_addr, ntohs(in->sin_port), handle);
    connected = r == 0;
    return r;
  }

  ssize_t recv(void *dst, size_t len, int flags) noexcept override
  {
    if (!connected) return -ENOTCONN;
    if (flags) return -EOPNOTSUPP;
    if (!len) return 0;
    unsigned n = std::min<size_t>(len, 128);
    // netd periodically releases its dispatcher while a blocking read is idle.
    for (;;) {
      L4::Ipc::Array<char> bytes(n, static_cast<char *>(dst));
      long r = net->recv(handle, n, bytes);
      if (r != -L4_EAGAIN) return r < 0 ? r : bytes.length;
    }
  }

  ssize_t send(const void *src, size_t len, int flags) noexcept override
  {
    if (!connected) return -ENOTCONN;
    if (flags & ~MSG_NOSIGNAL) return -EOPNOTSUPP;
    if (!len) return 0;
    unsigned sent = 0, n = std::min<size_t>(len, 128);
    long r = net->send(handle, L4::Ipc::Array<char const>(n, static_cast<const char *>(src)), sent);
    return r < 0 ? r : (ssize_t)sent;
  }

  ssize_t readv(const iovec *v, int n) noexcept override
  {
    for (int i = 0; i < n; ++i) if (v[i].iov_len) return recv(v[i].iov_base, v[i].iov_len, 0);
    return 0;
  }

  ssize_t writev(const iovec *v, int n) noexcept override
  {
    for (int i = 0; i < n; ++i) if (v[i].iov_len) return send(v[i].iov_base, v[i].iov_len, 0);
    return 0;
  }
};
}

int socket(int domain, int type, int protocol) noexcept
{
  if (domain != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (type != SOCK_STREAM || (protocol && protocol != IPPROTO_TCP)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  auto net = L4Re::Env::env()->get_cap<Net_svr>("netd");
  if (!net.is_valid()) {
    errno = EACCES;
    return -1;
  }
  auto file = cxx::make_ref_obj<Tcp_file>(net);
  int fd = file ? L4Re::Vfs::vfs_ops->alloc_fd(file) : -ENOMEM;
  if (fd < 0) {
    errno = -fd;
    return -1;
  }
  return fd;
}
