/**
 * \internal
 * \file
 */
/*
 * (c) 2010 Adam Lackorzynski <adam@os.inf.tu-dresden.de>
 *     economic rights: Technische Universität Dresden (Germany)
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#warning This file needs to be implemented.

L4_CV L4_INLINE
int
l4vcpu_is_irq_entry(l4_vcpu_state_t const *vcpu) L4_NOTHROW
{
  (void)vcpu;
  return 0; // TBD
}

L4_CV L4_INLINE
int
l4vcpu_is_page_fault_entry(l4_vcpu_state_t const *vcpu) L4_NOTHROW
{
  (void)vcpu;
  return 0; // TBD
}
