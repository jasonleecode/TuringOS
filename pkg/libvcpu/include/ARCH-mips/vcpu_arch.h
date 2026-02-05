/*
 * (c) 2010 Adam Lackorzynski <adam@os.inf.tu-dresden.de>
 *     economic rights: Technische Universität Dresden (Germany)
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

L4_CV L4_INLINE
int
l4vcpu_is_irq_entry(l4_vcpu_state_t const *vcpu) L4_NOTHROW
{
  return ((vcpu->r.cause >> 2) & 0x1f) == 0;
}

L4_CV L4_INLINE
int
l4vcpu_is_page_fault_entry(l4_vcpu_state_t const *vcpu) L4_NOTHROW
{
  unsigned x = (vcpu->r.cause >> 2) & 0x1f;

  return ((1 <= x) && (x <= 3)) || ((19 <= x) && (x <= 20));
}
