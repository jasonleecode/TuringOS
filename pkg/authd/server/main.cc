/*
 * authd — the TuringOS authentication server.
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * Step ② of decomposing native_shell: pull login/credential handling out of the
 * shell into an isolated service.  authd owns the credential store — salted
 * SHA-256 hashes in /ext4/etc/shadow (format `user:salt_hex:hash_hex`, with
 * hash = SHA-256(salt_bytes || password)) — and answers a single question over
 * IPC (Auth_svr, auth_ipc.h): are these credentials valid?  The shell binary
 * therefore contains no password and never compares one itself.
 *
 * If the shadow file can't be read (e.g. a diskless boot with no ext4), authd
 * falls back to one built-in entry so the system isn't bricked — clearly logged
 * as a degraded mode.  The fallback hash is the same salted SHA-256 scheme, so
 * the secret still never appears in cleartext.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <l4/re/env>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>

#include <mbedtls/sha256.h>

#include <auth_ipc.h>

namespace {

#define SHADOW_PATH "/ext4/etc/shadow"

struct Cred
{
  std::string user;
  std::vector<unsigned char> salt;   // decoded from salt_hex
  std::string hash_hex;              // lowercase hex of SHA-256(salt || pass)
};

std::vector<Cred> g_creds;

// ---- hex helpers ----------------------------------------------------------

int hex_nibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hex_decode(const std::string &hex, std::vector<unsigned char> &out)
{
  if (hex.size() % 2 != 0) return false;
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2)
    {
      int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
      if (hi < 0 || lo < 0) return false;
      out.push_back((unsigned char)((hi << 4) | lo));
    }
  return true;
}

std::string hex_encode(const unsigned char *p, size_t n)
{
  static const char *d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i)
    {
      s.push_back(d[p[i] >> 4]);
      s.push_back(d[p[i] & 0xf]);
    }
  return s;
}

// SHA-256(salt || pass) -> lowercase hex.
std::string salted_hash(const std::vector<unsigned char> &salt,
                        const char *pass, size_t pass_len)
{
  unsigned char out[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0 /* SHA-256, not 224 */);
  if (!salt.empty())
    mbedtls_sha256_update_ret(&ctx, salt.data(), salt.size());
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)pass, pass_len);
  mbedtls_sha256_finish_ret(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return hex_encode(out, sizeof(out));
}

// Constant-time string compare (length-leaking only, which is fine for hex of
// a fixed-size digest).
bool ct_equal(const std::string &a, const std::string &b)
{
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

// ---- credential store -----------------------------------------------------

// Parse one `user:salt_hex:hash_hex` line into g_creds.  Returns true if added.
bool add_shadow_line(const char *line)
{
  std::string s(line);
  // strip trailing newline / CR / spaces
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  if (s.empty() || s[0] == '#') return false;

  size_t c1 = s.find(':');
  if (c1 == std::string::npos) return false;
  size_t c2 = s.find(':', c1 + 1);
  if (c2 == std::string::npos) return false;

  Cred e;
  e.user     = s.substr(0, c1);
  std::string salt_hex = s.substr(c1 + 1, c2 - c1 - 1);
  e.hash_hex = s.substr(c2 + 1);
  if (e.user.empty() || e.hash_hex.empty()) return false;
  if (!hex_decode(salt_hex, e.salt)) return false;

  g_creds.push_back(std::move(e));
  return true;
}

void install_fallback()
{
  // root / 12345678, salt 0123456789abcdef0123456789abcdef.
  // hash = SHA-256(salt || "12345678").  Kept here only so a diskless boot can
  // still log in; the cleartext password never appears.
  if (!add_shadow_line(
        "root:0123456789abcdef0123456789abcdef:"
        "73c589806fe85d70363a88077d418adad034bd3fad37315681d7f80984710f00"))
    printf("[authd] ERROR: built-in fallback credential is malformed\n");
  else
    printf("[authd] using built-in fallback credentials (DEGRADED — no "
           SHADOW_PATH ")\n");
}

void load_credentials()
{
  FILE *f = fopen(SHADOW_PATH, "r");
  if (!f)
    {
      printf("[authd] cannot open " SHADOW_PATH "\n");
      install_fallback();
      return;
    }

  char line[256];
  int n = 0;
  while (fgets(line, sizeof(line), f))
    if (add_shadow_line(line))
      ++n;
  fclose(f);

  if (n == 0)
    {
      printf("[authd] " SHADOW_PATH " has no usable entries\n");
      install_fallback();
      return;
    }
  printf("[authd] loaded %d credential(s) from " SHADOW_PATH "\n", n);
}

// ---- server ---------------------------------------------------------------

class Auth_impl : public L4::Epiface_t<Auth_impl, Auth_svr>
{
public:
  long op_authenticate(Auth_svr::Rights,
                       L4::Ipc::Array_ref<char const> user,
                       L4::Ipc::Array_ref<char const> pass)
  {
    std::string u(user.data, user.length);
    for (auto const &e : g_creds)
      {
        if (e.user != u)
          continue;
        std::string got = salted_hash(e.salt, pass.data, pass.length);
        if (ct_equal(got, e.hash_hex))
          {
            printf("[authd] auth OK for '%s'\n", u.c_str());
            return L4_EOK;
          }
        printf("[authd] auth FAIL for '%s' (bad password)\n", u.c_str());
        return -L4_EPERM;
      }
    printf("[authd] auth FAIL for '%s' (no such user)\n", u.c_str());
    return -L4_EPERM;
  }
};

} // namespace

static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;

int main()
{
  printf("[authd] starting\n");

  load_credentials();

  static Auth_impl impl;
  if (!server.registry()->register_obj(&impl, "svr").is_valid())
    {
      printf("[authd] ERROR: cannot bind 'svr' gate — exiting\n");
      return 1;
    }
  printf("[authd] service ready (Auth_svr on 'svr')\n");

  server.loop();
  return 0;
}
