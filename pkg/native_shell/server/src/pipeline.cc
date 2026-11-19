#include "commands.h"
#include <l4/re/env>
#include <l4/turingos/spawn_ipc.h>
#include <l4/turingos/task_io.h>
#include <l4/util/util.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

bool builtin(const char *s)
{
  for (int i = 0; i < num_commands; ++i) if (!strcmp(s, commands[i].name)) return true;
  return false;
}
struct Pump {
  int src, dst;
};

void *copy_stream(void *arg)
{
  auto p = static_cast<Pump *>(arg);
  char buf[2048];
  ssize_t n;
  while ((n = read(p->src, buf, sizeof(buf))) > 0) {
    ssize_t pos = 0;
    while (pos < n) {
      ssize_t w = write(p->dst, buf + pos, n - pos);
      if (w <= 0) {
        n = -1;
        break;
      }
      pos += w;
    }
    if (n < 0) break;
  }
  if (n < 0 && errno != EPIPE) perror("shell: redirect");
  close(p->src);
  close(p->dst);
  delete p;
  return nullptr;
}

bool start_pump(int src, int dst, pthread_t &thread)
{
  auto p = new(std::nothrow) Pump{src, dst};
  if (p && !pthread_create(&thread, nullptr, copy_stream, p)) return true;
  close(src);
  close(dst);
  delete p;
  return false;
}
struct Job {
  L4::Cap<Spawn_svr> spawn;
  unsigned handles[32]{};
  int count = 0;
  pthread_t pumps[2]{};
  int npumps = 0;
};

void *finish(void *arg)
{
  auto job = static_cast<Job *>(arg);
  for (int i = 0; i < job->count; ++i) job->spawn->wait(job->handles[i], WAIT_BLOCK);
  for (int i = 0; i < job->npumps; ++i) pthread_join(job->pumps[i], nullptr);
  delete job;
  return nullptr;
}

long launch(L4::Cap<Spawn_svr> svr, char **argv, int input, int output, const char *cwd)
{
  if (!strcmp(argv[0], "run")) ++argv;
  if (!argv[0]) return -L4_EINVAL;
  char packed[512];
  size_t size = 0;
  for (int i = 0; argv[i]; ++i) {
    size_t n = strlen(argv[i]) + 1;
    if (n > sizeof(packed) - size) return -L4_EINVAL;
    memcpy(packed + size, argv[i], n);
    size += n;
  }
  int reader = input < 0 ? 0 : task_pipe_handle(input), writer = output < 0 ? 0 : task_pipe_handle(output);
  if (reader < 0 || writer < 0) return -L4_EINVAL;
  return svr->spawn_io(L4::Ipc::Array<char const>(strlen(argv[0]) + 1, argv[0]),
                       L4::Ipc::Array<char const>(size, packed), SPAWN_BG, reader, writer,
                       L4::Ipc::Array<char const>(strlen(cwd) + 1, cwd));
}
}

bool shell_pipeline(int argc, char **argv, const bool *operators)
{
  bool piped = false, redirected = false;
  for (int i = 0; i < argc; ++i) if (operators[i]) {
      if (!strcmp(argv[i], "|")) piped = true;
      if (strchr("<>", argv[i][0])) redirected = true;
    }
  if (!piped && (!redirected || (builtin(argv[0]) && strcmp(argv[0], "run")))) return false;
  char *words[96], **stages[32];
  int nwords = 0, nstages = 1;
  stages[0] = words;
  const char *input_path = nullptr, *output_path = nullptr;
  bool append = false, bg = false, ok = true;
  for (int i = 0; i < argc; ++i) {
    if (!operators[i]) {
      words[nwords++] = argv[i];
      continue;
    }
    if (!strcmp(argv[i], "|")) {
      if (nstages == 32 || words + nwords == stages[nstages - 1] || output_path) {
        ok = false;
        break;
      }
      words[nwords++] = nullptr;
      stages[nstages++] = words + nwords;
    } else if (!strcmp(argv[i], "&")) {
      bg = true;
      if (i != argc - 1) ok = false;
    } else {
      char op = argv[i][0];
      append = op == '>' ? argv[i][1] == '>' : append;
      if (++i == argc || operators[i]) {
        ok = false;
        break;
      }
      if (op == '<') {
        if (input_path || nstages != 1) ok = false;
        input_path = argv[i];
      } else {
        if (output_path) ok = false;
        output_path = argv[i];
      }
    }
  }
  words[nwords] = nullptr;
  if (!ok || !*stages[nstages - 1]) {
    puts("shell: invalid pipeline");
    return true;
  }
  for (int i = 0; i < nstages; ++i) if (builtin(stages[i][0]) && strcmp(stages[i][0], "run")) {
      printf("shell: %s is a session builtin\n", stages[i][0]);
      return true;
    }
  char cwd[256];
  auto svr = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  if (!getcwd(cwd, sizeof(cwd)) || !svr.is_valid()) {
    puts("shell: loader unavailable");
    return true;
  }
  auto job = new(std::nothrow) Job{};
  if (!job) return true;
  job->spawn = svr;
  int input = -1;
  if (input_path) {
    int f = open(input_path, O_RDONLY), p[2];
    if (f < 0) {
      perror(input_path);
      ok = false;
    } else if (pipe(p) < 0) {
      close(f);
      ok = false;
    } else {
      input = p[0];
      ok = start_pump(f, p[1], job->pumps[job->npumps]);
      if (ok) ++job->npumps;
    }
  }
  for (int i = 0; ok && i < nstages; ++i) {
    int p[2] = {-1, -1};
    if ((i + 1 < nstages || output_path) && pipe(p) < 0) {
      ok = false;
      break;
    }
    if (i + 1 == nstages && output_path) {
      int f = open(output_path, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC), 0644);
      if (f < 0) {
        perror(output_path);
        close(p[0]);
        close(p[1]);
        ok = false;
        break;
      }
      if (!start_pump(p[0], f, job->pumps[job->npumps])) {
        close(p[1]);
        ok = false;
        break;
      }
      ++job->npumps;
      p[0] = -1;
    }
    long h = launch(svr, stages[i], input, p[1], cwd);
    if (h <= 0) ok = false;
    else job->handles[job->count++] = h;
    if (input >= 0) close(input);
    if (p[1] >= 0) close(p[1]);
    input = p[0];
  }
  if (input >= 0) close(input);
  if (!ok) {
    puts("shell: pipeline launch or redirection failed");
    for (int i = 0; i < job->count; ++i) svr->kill(job->handles[i]);
    job->count = 0;
  } else if (bg) {
    unsigned last = job->handles[--job->count];
    task_register_proc(last, stages[nstages - 1][0]);
    printf("[%u] %s\n", last, stages[nstages - 1][0]);
    pthread_t t;
    if (!pthread_create(&t, nullptr, finish, job)) {
      pthread_detach(t);
      return true;
    }
  }
  if (!bg) {
    for (int i = 0; i < job->count; ++i) {
      while (svr->wait(job->handles[i], WAIT_NOHANG) == -L4_EAGAIN) {
        if (g_shell_interrupt) {
          for (int j = i; j < job->count; ++j) svr->kill(job->handles[j]);
          break;
        }
        l4_sleep(5);
      }
    }
    job->count = 0;
  }
  finish(job);
  return true;
}
