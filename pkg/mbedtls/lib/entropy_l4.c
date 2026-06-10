/*
 * L4Re entropy backend for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT).
 * Copyright (c) 2026 Jason Lee <jasonlee@turingos.org>
 * License: MIT
 *
 * mbedTLS calls mbedtls_hardware_poll() to fill its entropy pool.  We route it
 * through a pluggable backend so real per-board entropy sources can be slotted
 * in later WITHOUT touching mbedTLS or lwIP:
 *
 *   virt / QEMU  -> a virtio-rng client driver (host entropy)
 *   i.MX6UL      -> the RNGB hardware true-RNG
 *   BBB/AM335x   -> physical timing jitter / ADC noise (no on-chip TRNG)
 *
 * Until such a backend registers via l4_mbedtls_set_entropy_backend(), we fall
 * back to a WEAK software jitter source (below).  On real silicon the
 * clock-domain jitter carries some genuine entropy; under QEMU it is mostly
 * host-scheduling artefacts and is NOT cryptographically sound.  This is fine
 * for bring-up / demos only — never ship it.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <l4/sys/kip.h>
#include <l4/re/env.h>

/* Optional real backend; NULL => weak software fallback. */
static int (*g_entropy_backend)(unsigned char *out, size_t len, size_t *olen);

void l4_mbedtls_set_entropy_backend(
    int (*fn)(unsigned char *out, size_t len, size_t *olen))
{
  g_entropy_backend = fn;
}

/* Weak software entropy: sample the KIP microsecond clock across a
 * data-dependent busy loop and mix the jittered low bits. */
static uint32_t s_mix = 0x12345678u;

static int weak_poll(unsigned char *out, size_t len, size_t *olen)
{
  l4_kernel_info_t const *kip = l4re_kip();
  for (size_t i = 0; i < len; i++)
    {
      uint32_t acc = s_mix;
      volatile unsigned spin = (acc & 0x3f) + 17;     /* data-dependent length */
      while (spin--)
        acc ^= (uint32_t)l4_kip_clock(kip);            /* capture timing jitter */

      uint64_t t = l4_kip_clock(kip);
      acc ^= (uint32_t)t ^ (uint32_t)(t >> 32);
      acc *= 2654435761u;                              /* Knuth multiplicative */
      acc ^= acc >> 15;
      s_mix = acc + 0x9e3779b9u;
      out[i] = (unsigned char)(acc >> 17);
    }
  *olen = len;
  return 0;
}

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
  static int warned;
  (void)data;

  if (!warned)
    {
      warned = 1;
      printf("\n*** mbedTLS entropy: WEAK software jitter source "
             "— NOT for production use ***\n");
    }

  if (g_entropy_backend)
    return g_entropy_backend(output, len, olen);
  return weak_poll(output, len, olen);
}
