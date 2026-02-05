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
  printf("%sip=%08lx sp=%08lx trapno=%08lx\n",
         prefix, vcpu->r.ip, vcpu->r.sp, vcpu->r.trapno);
  printf("%sax=%08lx dx=%08lx bx=%08lx cx=%08lx\n",
         prefix, vcpu->r.ax, vcpu->r.dx, vcpu->r.bx, vcpu->r.cx);
  printf("%ssi=%08lx di=%08lx bp=%08lx flags=%08lx\n",
         prefix, vcpu->r.si, vcpu->r.di, vcpu->r.bp, vcpu->r.flags);
  printf("%sds=%08lx es=%08lx gs=%08lx fs=%08lx\n",
         prefix, vcpu->r.ds, vcpu->r.es, vcpu->r.gs, vcpu->r.fs);
}
