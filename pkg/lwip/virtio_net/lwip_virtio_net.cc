/*
 * Copyright (C) 2022 Kernkonzept GmbH.
 * Author(s): Stephan Gerhold <stephan.gerhold@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <pthread.h>
#include <pthread-l4.h>

#include <l4/crtn/initpriorities.h>
#include <l4/cxx/unique_ptr>
#include <l4/l4virtio/client/virtio-net>
#include <l4/l4virtio/virtio_net.h>

#include <lwip/err.h>
#include <lwip/etharp.h>
#include <lwip/ethip6.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/snmp.h>
#include <lwip/sys.h>

class Netdev
{
private:
  struct pbuf_custom_rx
  {
    pbuf_custom pbuf;
    Netdev *ndev;
  };

  err_t init_netif();
  err_t output(struct pbuf *p);
  void *input_loop();
  void free_rx_pbuf(pbuf_custom_rx *p);

  static Netdev *dev(struct netif *n)
  { return static_cast<Netdev*>(n->state); }

  static err_t _output(struct netif *n, struct pbuf *p)
  { return dev(n)->output(p); }

  static void *_input_loop(void *arg)
  { return reinterpret_cast<Netdev *>(arg)->input_loop(); }

  static err_t _init_netif(struct netif *n)
  { return dev(n)->init_netif(); }

  static void _free_rx_pbuf(pbuf *p)
  {
    auto prx = reinterpret_cast<pbuf_custom_rx*>(p);
    prx->ndev->free_rx_pbuf(prx);
  }

public:
  explicit Netdev(L4::Cap<L4virtio::Device> vnet);

  struct netif netif;
  pthread_t input_thread;
  L4virtio::Driver::Virtio_net_device vdev;

  cxx::unique_ptr<pbuf_custom_rx[]> rx_pbufs;
};

err_t
Netdev::output(struct pbuf *p)
{
  // This should be only called from the lwIP core (with the lock held)
  LWIP_ASSERT_CORE_LOCKED();

  return vdev.tx([p] (L4virtio::Driver::Virtio_net_device::Packet &pkt)
    {
      pkt.hdr.hdr_len = PBUF_TRANSPORT;
      return pbuf_copy_partial(p, &pkt.data, sizeof(pkt.data), ETH_PAD_SIZE);
    }
  ) ? ERR_OK : ERR_MEM;
}

void *
Netdev::input_loop()
{
  vdev.bind_rx_notification_irq(Pthread::L4::cap(pthread_self()), 0);
  vdev.queue_rx();
  while (true)
    {
      l4_uint32_t len;
      auto descno = vdev.wait_rx(&len);
      auto &pkt = vdev.rx_pkt(descno);
      auto pbuf = pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF,
                                      &rx_pbufs[descno].pbuf, &pkt.data,
                                      sizeof(pkt.data));
      netif.input(pbuf, &netif);
    }
}

void
Netdev::free_rx_pbuf(pbuf_custom_rx *p)
{
  SYS_ARCH_DECL_PROTECT(old_level);
  l4_uint16_t descno = p - rx_pbufs.get();

  // This might be called concurrently from multiple threads
  SYS_ARCH_PROTECT(old_level);
  vdev.finish_rx(descno);
  vdev.queue_rx();
  SYS_ARCH_UNPROTECT(old_level);
}

err_t
Netdev::init_netif()
{
  static l4_uint8_t netif_num;

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "lwip";
#endif

  MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, 100000000);

  netif.name[0] = 'v';
  netif.name[1] = 'n';

#if LWIP_IPV4
  netif.output = etharp_output;
#endif
#if LWIP_IPV6
  netif.output_ip6 = ethip6_output;
#endif
  netif.linkoutput = _output;

  netif.mtu = 1500;

  if (vdev.feature_negotiated(L4VIRTIO_NET_F_MAC))
    {
      auto &cfg = vdev.device_config();
      static_assert(sizeof(netif.hwaddr) >= sizeof(cfg.mac),
                    "Virtio MAC address larger than lwIP hwaddr");
      memcpy(netif.hwaddr, cfg.mac, sizeof(cfg.mac));
      netif.hwaddr_len = sizeof(cfg.mac);
    }
  else
    {
      // Assign dummy MAC address
      netif.hwaddr[0] = 0x02; // prefix for locally administered address
      netif.hwaddr[1] = 0x00;
      netif.hwaddr[2] = 0x00;
      netif.hwaddr[3] = 0x00;
      netif.hwaddr[4] = 0x00;
      netif.hwaddr[5] = ++netif_num;
      netif.hwaddr_len = ETH_HWADDR_LEN;
    }

  netif.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

#if LWIP_IPV6
  netif_create_ip6_linklocal_address(&netif, 1);
#endif

  // Use as default for outgoing routes if this is the first network interface
  if (!netif_default)
    netif_set_default(&netif);
  return ERR_OK;
}

Netdev::Netdev(L4::Cap<L4virtio::Device> vnet) : netif(), vdev()
{
  vdev.setup_device(vnet);

  // Prepare PBUFs used for RX packets
  auto rxqsz = vdev.rx_queue_size();
  rx_pbufs = cxx::make_unique<pbuf_custom_rx[]>(rxqsz);
  for (auto i = 0; i < rxqsz; ++i)
    {
      rx_pbufs[i].pbuf.custom_free_function = _free_rx_pbuf;
      rx_pbufs[i].ndev = this;
    }

  // Register network interface
  if (netifapi_netif_add(&netif,
#if LWIP_IPV4
                         nullptr, nullptr, nullptr, // No IPv4 address
#endif
                         this, _init_netif, tcpip_input))
    throw L4::Runtime_error(-L4_ENODEV, "Failed to initialize network interface");

  if (pthread_create(&input_thread, nullptr, _input_loop, this))
    throw L4::Runtime_error(-L4_ENODEV, "Failed to start input thread");
}

struct Virtio_netdev { Virtio_netdev(); };
Virtio_netdev::Virtio_netdev()
{
  for (auto *i = L4Re::Env::env()->initial_caps(); i && i->flags != ~0UL; ++i)
    {
      if (strncmp(i->name, "virtnet", strlen("virtnet")) == 0)
        new Netdev(L4::Cap<L4virtio::Device>(i->cap));
    }
}

// Use explicit init priority to ensure that the network interface is already
// registered when running other constructors (e.g. from ifconfig_env).
Virtio_netdev vnet __attribute__((init_priority(INIT_PRIO_LATE)));
