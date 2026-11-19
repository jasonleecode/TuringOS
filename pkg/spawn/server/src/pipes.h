// Dispatcher-owned byte streams. Handles are scoped to the caller's task gate.
#pragma once
#include <l4/turingos/spawn_ipc.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

class Pipes {
  struct Pipe {
    unsigned readers = 0, writers = 0, head = 0, used = 0;
    char data[4096];
  } pipes[24]{};
  struct End {
    unsigned owner = 0;
    Pipe *pipe = nullptr;
    bool writer = false;
  };
  End ends[128]{};

  End *lookup(unsigned owner, unsigned id)
  {
    if (!id || id > sizeof(ends) / sizeof(*ends)) return nullptr;
    auto &e = ends[id - 1];
    return e.pipe && e.owner == owner ? &e : nullptr;
  }

  unsigned add(unsigned owner, Pipe *p, bool writer)
  {
    for (auto &e : ends) if (!e.pipe) {
        e = {owner, p, writer};
        ++(writer ? p->writers : p->readers);
        return (&e - ends) + 1;
      }
    return 0;
  }
public:

  long create(unsigned owner, unsigned &r, unsigned &w)
  {
    for (auto &p : pipes) if (!p.readers && !p.writers) {
        p.head = p.used = 0;
        r = add(owner, &p, false);
        w = r ? add(owner, &p, true) : 0;
        if (r && w) return 0;
        close(owner, r);
        return -L4_ENOMEM;
      }
    return -L4_ENOMEM;
  }

  long close(unsigned owner, unsigned id)
  {
    auto e = lookup(owner, id);
    if (!e) return -L4_EINVAL;
    --(e->writer ? e->pipe->writers : e->pipe->readers);
    e->pipe = nullptr;
    return 0;
  }

  void release(unsigned owner)
  {
    for (auto &e : ends) if (e.pipe && e.owner == owner) close(owner, (&e - ends) + 1);
  }

  int inherit(unsigned owner, unsigned id, unsigned child, bool writer)
  {
    if (!id) return 0;
    auto e = lookup(owner, id);
    if (!e || e->writer != writer) return -L4_EINVAL;
    unsigned result = add(child, e->pipe, writer);
    return result ? (int)result : -L4_ENOMEM;
  }

  long write(unsigned owner, unsigned id, const char *src, unsigned n)
  {
    auto e = lookup(owner, id);
    if (!e || !e->writer) return -L4_EINVAL;
    auto &p = *e->pipe;
    if (!p.readers) return -EPIPE;
    if (n > sizeof(p.data) - p.used) return -L4_EAGAIN;
    for (unsigned i = 0; i < n; ++i) p.data[(p.head + p.used + i) % sizeof(p.data)] = src[i];
    p.used += n;
    return n;
  }

  long read(unsigned owner, unsigned id, char *dst, unsigned n)
  {
    auto e = lookup(owner, id);
    if (!e || e->writer) return -L4_EINVAL;
    auto &p = *e->pipe;
    if (!p.used && p.writers && n) return -L4_EAGAIN;
    n = std::min(n, p.used);
    for (unsigned i = 0; i < n; ++i) dst[i] = p.data[(p.head + i) % sizeof(p.data)];
    p.head = (p.head + n) % sizeof(p.data);
    p.used -= n;
    return n;
  }
};
