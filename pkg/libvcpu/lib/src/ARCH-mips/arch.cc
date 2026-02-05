/*
 * (c) 2010 Adam Lackorzynski <adam@os.inf.tu-dresden.de>
 *     economic rights: Technische Universität Dresden (Germany)
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <l4/vcpu/vcpu.h>

#include <stdio.h>

void l4vcpu_print_state_arch(l4_vcpu_state_t const *vcpu,
                             const char *prefix) L4_NOTHROW
{
  printf("%svcpu=%p ip=%08lx  status=%08lx cause=%08lx label=%08lx\n",
         prefix, vcpu, vcpu->r.epc, vcpu->r.status, vcpu->r.cause, vcpu->i.label);
  for (unsigned i = 0; i < 32; ++i)
    printf("%sr[%2d]=%08lx%s", (i & 3) == 0 ? prefix : "", i, vcpu->r.r[i],
           (i & 3) == 3 ? "\n" : " ");
  printf("%shi=%08lx lo=%08lx\n", prefix, vcpu->r.hi, vcpu->r.lo);
}
