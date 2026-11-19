// Two independently checksummed records select immutable, content-addressed releases.
// Write and read back the payload before replacing the older record.
#include <l4/re/env>
#include <l4/turingos/ext4_file_proto.h>
#include <l4/turingos/spawn_ipc.h>
#include <mbedtls/sha256.h>
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <string>
#include <algorithm>
#include <type_traits>
#include <sys/stat.h>

namespace {
constexpr size_t max_image = 4 * 1024 * 1024;
struct Release {
  uint64_t generation;
  uint32_t flags;
  uint32_t wasm;
  char version[32];
  unsigned char digest[32], seal[32];
};

bool identifier(const char *s)
{
  if (!*s || strlen(s) >= 32) return false;
  for (; *s; ++s)
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')
          || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_'))
      return false;
  return true;
}

std::string hex(const unsigned char *digest)
{
  const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned i = 0; i < 32; ++i) {
    out += digits[digest[i] >> 4];
    out += digits[digest[i] & 15];
  }
  return out;
}

bool transfer(const char *path, unsigned char digest[32], FILE *dest = nullptr)
{
  FILE *in = fopen(path, "rb");
  if (!in) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = !mbedtls_sha256_starts_ret(&ctx, 0);
  unsigned char buf[4096];
  size_t total = 0, n;
  while (ok && (n = fread(buf, 1, sizeof(buf), in)) != 0) {
    total += n;
    ok = total <= max_image && (!dest || fwrite(buf, 1, n, dest) == n)
         && !mbedtls_sha256_update_ret(&ctx, buf, n);
  }
  ok = ok && !ferror(in) && total >= 8 && !mbedtls_sha256_finish_ret(&ctx, digest);
  fclose(in);
  mbedtls_sha256_free(&ctx);
  return ok;
}

bool loadable_elf(FILE *f, size_t size)
{
  using Header = std::conditional<sizeof(void *) == 4, Elf32_Ehdr, Elf64_Ehdr>::type;
  using Segment = std::conditional<sizeof(void *) == 4, Elf32_Phdr, Elf64_Phdr>::type;
  Header h{};
  if (fseek(f, 0, SEEK_SET) || fread(&h, 1, sizeof(h), f) != sizeof(h)
      || memcmp(h.e_ident, ELFMAG, SELFMAG) || h.e_ident[EI_DATA] != ELFDATA2LSB
      || h.e_ident[EI_CLASS] != (sizeof(void *) == 4 ? ELFCLASS32 : ELFCLASS64)
      || h.e_machine != (sizeof(void *) == 4 ? EM_ARM : EM_AARCH64)
      || (h.e_type != ET_EXEC && h.e_type != ET_DYN) || h.e_phentsize != sizeof(Segment)
      || h.e_phoff > size || h.e_phnum > (size - h.e_phoff) / sizeof(Segment)
      || fseek(f, h.e_phoff, SEEK_SET))
    return false;
  bool entry_found = false;
  for (unsigned i = 0; i < h.e_phnum; ++i) {
    Segment segment{};
    if (fread(&segment, 1, sizeof(segment), f) != sizeof(segment)) return false;
    if (segment.p_type != PT_LOAD) continue;
    if (segment.p_filesz > segment.p_memsz || segment.p_offset > size
        || segment.p_filesz > size - segment.p_offset)
      return false;
    if ((segment.p_flags & PF_X) && h.e_entry >= segment.p_vaddr
        && h.e_entry - segment.p_vaddr < segment.p_memsz)
      entry_found = true;
  }
  return entry_found;
}

int image_type(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  char magic[8];
  int type = -1;
  if (fread(magic, 1, sizeof(magic), f) == sizeof(magic)) {
    if (!memcmp(magic, "\0asm\1\0\0\0", 8)) type = 1;
    else {
      struct stat st;
      if (!fstat(fileno(f), &st) && st.st_size > 0 && loadable_elf(f, st.st_size)) type = 0;
    }
  }
  fclose(f);
  return type;
}

std::string record(const std::string &base, int slot)
{
  return base + (slot ? ".1" : ".0");
}

std::string payload(const std::string &base, const Release &r)
{
  return base + "." + hex(r.digest);
}

bool read_release(const std::string &base, int slot, Release &r)
{
  FILE *f = fopen(record(base, slot).c_str(), "rb");
  if (!f) return false;
  bool ok = fread(&r, 1, sizeof(r), f) == sizeof(r) && fgetc(f) == EOF && !ferror(f);
  fclose(f);
  unsigned char seal[32];
  if (!ok || !r.generation || !memchr(r.version, 0, sizeof(r.version))
      || !identifier(r.version) || r.wasm > 1 || (r.flags & ~(SPAWN_NET | SPAWN_DATA | SPAWN_TMP))
      || mbedtls_sha256_ret(reinterpret_cast<unsigned char *>(&r), offsetof(Release, seal), seal, 0)
      || memcmp(seal, r.seal, 32))
    return false;
  return transfer(payload(base, r).c_str(), seal) && !memcmp(seal, r.digest, 32);
}

bool publish(const std::string &base, int slot, Release &r)
{
  if (mbedtls_sha256_ret(reinterpret_cast<unsigned char *>(&r), offsetof(Release, seal), r.seal, 0)) return false;
  FILE *f = fopen(record(base, slot).c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(&r, 1, sizeof(r), f) == sizeof(r);
  if (fclose(f)) return false;
  Release check{};
  return ok && read_release(base, slot, check) && !memcmp(&r, &check, sizeof(r));
}

long launch(L4::Cap<Spawn_svr> spawn, const char *program, char *const *args, int argc, unsigned flags)
{
  char buf[512];
  size_t used = 0;
  for (int i = 0; i < argc; ++i) {
    size_t n = strlen(args[i]) + 1;
    if (n > sizeof(buf) - used) return -L4_EINVAL;
    memcpy(buf + used, args[i], n);
    used += n;
  }
  return spawn->spawn(L4::Ipc::Array<char const>(strlen(program) + 1, program),
                      L4::Ipc::Array<char const>(used, buf), flags);
}
}

int main(int argc, char **argv)
{
  if (argc < 3 || !identifier(argv[2])) {
    puts("app install NAME VERSION FILE [none|net|data|tmp ...] [sha256=HEX]\napp status|rollback|run NAME [args...]");
    return 2;
  }
  auto spawn = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
  auto fs = L4Re::Env::env()->get_cap<Ext4_dir_ops>("ext4");
  if (!spawn.is_valid() || !fs.is_valid()) {
    puts("app: application store unavailable");
    return 1;
  }
  if (spawn->app_lock(1) < 0) {
    puts("app: application store busy or access denied");
    return 1;
  }
  const char dir[] = "/apps";
  long mk = fs->ext_mkdir(L4::Ipc::Array<char const, unsigned long>(sizeof(dir) - 1, dir));
  if (mk < 0 && mk != -L4_EEXIST) return 1;
  std::string base = std::string("/ext4/apps/") + argv[2];
  Release slots[2]{};
  bool valid[] = {read_release(base, 0, slots[0]), read_release(base, 1, slots[1])};
  int active = valid[1] && (!valid[0] || slots[1].generation > slots[0].generation) ? 1 : 0;
  int next = 1 - active;
  if (!strcmp(argv[1], "install") && argc >= 5 && identifier(argv[3])) {
    Release r{};
    r.generation = valid[active] ? slots[active].generation + 1 : 1;
    if (!r.generation || !transfer(argv[4], r.digest)) {
      puts("app: unreadable or oversized image");
      return 1;
    }
    strcpy(r.version, argv[3]);
    int type = image_type(argv[4]);
    if (type < 0) {
      puts("app: unsupported image");
      return 1;
    }
    r.wasm = type;
    for (int i = 5; i < argc; ++i) {
      if (!strcmp(argv[i], "net")) r.flags |= SPAWN_NET;
      else if (!strcmp(argv[i], "data")) r.flags |= SPAWN_DATA;
      else if (!strcmp(argv[i], "tmp")) r.flags |= SPAWN_TMP;
      else if (!strncmp(argv[i], "sha256=", 7)) {
        if (hex(r.digest) != argv[i] + 7) {
          puts("app: checksum mismatch");
          return 1;
        }
      } else if (strcmp(argv[i], "none")) {
        puts("app: unknown permission");
        return 2;
      }
    }
    if (r.wasm && r.flags) {
      puts("app: this WASM runtime supports only builtin imports");
      return 2;
    }
    std::string dest = payload(base, r);
    unsigned char check[32];
    if (!transfer(dest.c_str(), check) || memcmp(check, r.digest, 32)) {
      FILE *out = fopen(dest.c_str(), "wb");
      if (!out) return 1;
      bool copied = transfer(argv[4], check, out);
      if (fclose(out) || !copied || memcmp(check, r.digest, 32)
          || !transfer(dest.c_str(), check) || memcmp(check, r.digest, 32))
        return 1;
    }
    if (r.wasm) {
      char *args[] = {const_cast<char *>("wasm-run"), const_cast<char *>("--check"), const_cast<char *>(dest.c_str())};
      if (launch(spawn, "rom/wasm-run", args, 3, SPAWN_WAIT) != 0) return 1;
    }
    if (valid[active] && !strcmp(slots[active].version, r.version)
        && slots[active].flags == r.flags && !memcmp(slots[active].digest, r.digest, 32))
      return 0;
    if (!publish(base, next, r)) {
      puts("app: install verification failed");
      return 1;
    }
    printf("installed %s %s\n", argv[2], r.version);
    return 0;
  }
  if (!valid[active]) {
    puts("app: no verified release");
    return 1;
  }
  auto &r = slots[active];
  if (!strcmp(argv[1], "status")) {
    printf("%s %s %s permissions=%s%s%s%s sha256=%s\n", argv[2], r.version, r.wasm ? "wasm" : "elf",
           r.flags ? "" : "none", r.flags & SPAWN_NET ? "net " : "", r.flags & SPAWN_DATA ? "data " : "",
           r.flags & SPAWN_TMP ? "tmp" : "", hex(r.digest).c_str());
    return 0;
  }
  if (!strcmp(argv[1], "rollback")) {
    if (!valid[next]) {
      puts("app: no previous release");
      return 1;
    }
    // Remove the newest selector only after verifying the previous payload.
    std::string relative = record(base, active).substr(5);
    if (fs->ext_unlink(L4::Ipc::Array<char const, unsigned long>(relative.size(), relative.c_str())) < 0) return 1;
    printf("rolled back %s to %s\n", argv[2], slots[next].version);
    return 0;
  }
  if (!strcmp(argv[1], "run")) {
    // Leave the active selector intact on transient launch failures (e.g. low
    // memory). The previous verified release can still start this invocation.
    for (int index : {active, next}) {
      if (!valid[index]) continue;
      const Release &release = slots[index];
      std::string path = payload(base, release);
      const char *program = release.wasm ? "rom/wasm-run" : path.c_str();
      char *args[66];
      int count = 0;
      args[count++] = const_cast<char *>(program);
      if (release.wasm) args[count++] = const_cast<char *>(path.c_str());
      if (argc - 3 > 65 - count) return 2;
      for (int i = 3; i < argc; ++i) args[count++] = argv[i];
      long h = launch(spawn, program, args, count, release.flags);
      if (h <= 0) {
        fprintf(stderr, "app: could not launch %s (%ld)\n", release.version, h);
        continue;
      }
      spawn->app_lock(0);
      long rc = spawn->wait(h, WAIT_BLOCK);
      return rc < 0 ? 1 : rc;
    }
    return 1;
  }
  puts("app: invalid command");
  return 2;
}
