/*
 * (c) 2010 Adam Lackorzynski <adam@os.inf.tu-dresden.de>
 *     economic rights: Technische Universität Dresden (Germany)
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <l4/vcpu/vcpu>
#include <l4/re/util/kumem_alloc>
#include <cstdio>
#include <cstring>

L4_CV void
l4vcpu_print_state(l4_vcpu_state_t const *vcpu,
                   const char *prefix) L4_NOTHROW
{
  printf("%svcpu=%p state=%x savedstate=%x label=%lx\n",
         prefix, vcpu, vcpu->state, vcpu->saved_state, vcpu->i.label);
  printf("%ssticky=%x user_task=%lx  pfa=%lx\n",
         prefix, vcpu->sticky_flags, vcpu->user_task << L4_CAP_SHIFT,
         vcpu->r.pfa);
  printf("%sentry_sp=%lx entry_ip=%lx\n",
         prefix, vcpu->entry_sp, vcpu->entry_ip);
  l4vcpu_print_state_arch(vcpu, prefix);
}

static int
do_l4vcpu_ext_alloc(L4vcpu::Vcpu **vcpu,
                    l4_addr_t *ext_state,
                    L4::Cap<L4::Task> task,
                    L4::Cap<L4Re::Rm> rm) L4_NOTHROW
{
  int r;
  l4_addr_t v;

  if ((r = L4Re::Util::kumem_alloc(&v, 0, task, rm)))
    return r;

  *vcpu      = L4vcpu::Vcpu::cast(v);
  *ext_state = v + L4_VCPU_OFFSET_EXT_STATE;

  return 0;
}

L4_CV int
l4vcpu_ext_alloc(l4_vcpu_state_t **vcpu, l4_addr_t *ext_state,
                 l4_cap_idx_t task, l4_cap_idx_t regmgr) L4_NOTHROW
{
  L4::Cap<L4::Task> t(task);
  L4::Cap<L4Re::Rm> r(regmgr);
  L4vcpu::Vcpu **v = reinterpret_cast<L4vcpu::Vcpu **>(vcpu);
  return do_l4vcpu_ext_alloc(v, ext_state, t, r);
}

L4_CV int
L4vcpu::Vcpu::ext_alloc(Vcpu **vcpu,
                        l4_addr_t *ext_state,
                        L4::Cap<L4::Task> task,
                        L4::Cap<L4Re::Rm> rm) throw()
{
  return do_l4vcpu_ext_alloc(vcpu, ext_state, task, rm);
}
