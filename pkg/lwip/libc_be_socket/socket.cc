/*
 * Copyright (C) 2014-2015, 2017, 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <l4/cxx/ref_ptr>
#include <l4/l4re_vfs/backend>
#include <l4/l4re_vfs/vfs.h>

#include <lwip/sockets.h>
#include <lwip/tcpip.h>

namespace {

class Socket_file : public L4Re::Vfs::Be_file
{
public:
  explicit Socket_file(int lwip_fd) : _lwip_fd(lwip_fd) {}

  ~Socket_file() throw()
  { lwip_close(_lwip_fd); }

  // The libc file backend expects the errno in the return value,
  // while lwIP already places it in errno.
  template<typename T>
  static T ret_errno(T ret)
  { return ret < 0 ? -errno : ret; }

  ssize_t readv(const iovec *iov, int iovcnt) throw() override
  { return ret_errno(lwip_readv(_lwip_fd, iov, iovcnt)); }
  ssize_t writev(const iovec *iov, int iovcnt) throw() override
  { return ret_errno(lwip_writev(_lwip_fd, iov, iovcnt)); }
  int ioctl(unsigned long cmd, va_list args) throw() override
  { return ret_errno(lwip_ioctl(_lwip_fd, cmd, va_arg(args, void*))); }
  int get_status_flags() const throw() override
  { return ret_errno(lwip_fcntl(_lwip_fd, F_GETFL, 0)); }
  int set_status_flags(long val) throw() override
  { return ret_errno(lwip_fcntl(_lwip_fd, F_SETFL, val)); }

  bool check_ready(Ready_type rt) throw() override;

  // Socket interface
  int bind(const sockaddr *name, socklen_t namelen) throw() override
  { return ret_errno(lwip_bind(_lwip_fd, name, namelen)); }
  int connect(const sockaddr *name, socklen_t namelen) throw() override
  { return ret_errno(lwip_connect(_lwip_fd, name, namelen)); }
  ssize_t send(const void *dataptr, size_t size, int flags) throw() override
  { return ret_errno(lwip_send(_lwip_fd, dataptr, size, flags)); }
  ssize_t recv(void *mem, size_t len, int flags) throw() override
  { return ret_errno(lwip_recv(_lwip_fd, mem, len, flags)); }
  ssize_t sendto(const void *dataptr, size_t size, int flags, const sockaddr *to, socklen_t tolen) throw() override
  { return ret_errno(lwip_sendto(_lwip_fd, dataptr, size, flags, to, tolen)); }
  ssize_t recvfrom(void *mem, size_t len, int flags, sockaddr *from, socklen_t *fromlen) throw() override
  { return ret_errno(lwip_recvfrom(_lwip_fd, mem, len, flags, from, fromlen)); }
  ssize_t sendmsg(const msghdr *message, int flags) throw() override
  { return ret_errno(lwip_sendmsg(_lwip_fd, message, flags)); }
  ssize_t recvmsg(msghdr *message, int flags) throw() override
  { return ret_errno(lwip_recvmsg(_lwip_fd, message, flags)); }
  int getsockopt(int level, int optname, void *optval, socklen_t *optlen) throw() override
  { return ret_errno(lwip_getsockopt(_lwip_fd, level, optname, optval, optlen)); }
  int setsockopt(int level, int optname, const void *optval, socklen_t optlen) throw() override
  { return ret_errno(lwip_setsockopt(_lwip_fd, level, optname, optval, optlen)); }
  int listen(int backlog) throw() override
  { return ret_errno(lwip_listen(_lwip_fd, backlog)); }
  int accept(sockaddr *addr, socklen_t *addrlen) throw() override
  { return ret_errno(alloc_fd(lwip_accept(_lwip_fd, addr, addrlen))); }
  int shutdown(int how) throw() override
  { return ret_errno(lwip_shutdown(_lwip_fd, how)); }
  int getsockname(sockaddr *name, socklen_t *namelen) throw() override
  { return ret_errno(lwip_getsockname(_lwip_fd, name, namelen)); }
  int getpeername(sockaddr *name, socklen_t *namelen) throw() override
  { return ret_errno(lwip_getpeername(_lwip_fd, name, namelen)); }

  static int alloc_fd(int lwip_fd) throw();

private:
  int _lwip_fd;
};

/**
 * Check whether the socket is ready for an I/O operation/condition.
 *
 * The check is delegated to internal tests that implement the same logic
 * as #lwip_select() and #lwip_poll().
 *
 * \param rt  Type of the I/O operation/condition to be ready.
 *
 * \retval true   Socket is ready.
 * \retval false  Socket is not ready.
 */
bool
Socket_file::check_ready(Ready_type rt) throw()
{
  switch (rt)
    {
      case Read:
        return lwip_sock_status_read(_lwip_fd);
      case Write:
        return lwip_sock_status_write(_lwip_fd);
      case Exception:
        return lwip_sock_status_exception(_lwip_fd);
    }

  return false;
}

int
Socket_file::alloc_fd(int lwip_fd) throw()
{
  if (lwip_fd < 0)
    return -1;

  cxx::Ref_ptr<Socket_file> s(new Socket_file(lwip_fd));
  if (!s)
    {
      lwip_close(lwip_fd);
      errno = ENOBUFS;
      return -1;
    }

  int fd = L4Re::Vfs::vfs_ops->alloc_fd(s);
  if (fd < 0)
    {
      errno = ENFILE;
      return -1;
    }

  return fd;
}
} // namespace

int socket(int domain, int type, int protocol) throw()
{
  return Socket_file::alloc_fd(lwip_socket(domain, type, protocol));
}
