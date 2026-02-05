#include <l4/crtn/initpriorities.h>
#include <lwip/tcpip.h>

struct Lwip_init { Lwip_init(); };

Lwip_init::Lwip_init()
{
  // Start lwIP's TCPIP thread, includes lwip_init()
  tcpip_init(NULL, NULL);
}

static Lwip_init i __attribute__((init_priority(INIT_PRIO_TCPIP_INIT)));
