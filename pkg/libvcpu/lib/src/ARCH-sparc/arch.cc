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
  printf("%svcpu=%p ip=%08lx  sp=%08lx err=%08lx label=%08lx\n",
         prefix, vcpu, vcpu->r.ip, vcpu->r.sp, vcpu->r.err, vcpu->i.label);
  printf("%s r0=%08lx  r1=%08lx  r2=%08lx  r3=%08lx\n",
         prefix, vcpu->r.r[0], vcpu->r.r[1], vcpu->r.r[2], vcpu->r.r[3]);
  printf("%s r4=%08lx  r5=%08lx  r6=%08lx  r7=%08lx\n",
         prefix, vcpu->r.r[4], vcpu->r.r[5], vcpu->r.r[6], vcpu->r.r[7]);
  printf("%s r8=%08lx  r9=%08lx r10=%08lx r11=%08lx\n",
         prefix, vcpu->r.r[8], vcpu->r.r[9], vcpu->r.r[10], vcpu->r.r[11]);
  printf("%sr12=%08lx  lr=%08lx flags=%08lx\n",
         prefix, vcpu->r.r[12], vcpu->r.lr, vcpu->r.flags);
}
