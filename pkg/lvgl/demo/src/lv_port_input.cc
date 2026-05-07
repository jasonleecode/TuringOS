/*
 * lv_port_input.cc — LVGL v9 input driver for L4Re / virtio-input
 *
 * Opens the "input" vbus cap, enumerates virtio-mmio devices (DeviceID=18),
 * detects tablet (EV_ABS capable) vs keyboard, sets up the eventq virtqueue
 * in polling mode (no IRQ), and registers LVGL pointer + keypad indevs.
 *
 * The indev read_cb is called by lv_timer_handler() every ~30 ms.  Inside,
 * we drain the virtqueue used ring and translate events to LVGL state.
 */

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <l4/re/mem_alloc>
#include <l4/re/rm>
#include <l4/re/dma_space>
#include <l4/vbus/vbus>
#include <l4/sys/err.h>
#include <l4/l4virtio/virtqueue>
#include <l4/sigma0/sigma0.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include "lvgl/lvgl.h"
}

#include "lv_port_input.h"

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

static const unsigned VQ_SIZE     = 16;
static const l4_size_t VQ_DESC_OFF  = 0;
static const l4_size_t VQ_AVAIL_OFF = VQ_SIZE * 16;        // 256
static const l4_size_t VQ_USED_OFF  = L4_PAGESIZE;         // 4096
static const l4_size_t VQ_EVT_OFF   = L4_PAGESIZE + 512;   // 4608
static const l4_size_t VQ_MEM_SIZE  = 2 * L4_PAGESIZE;     // 8192

static const l4_uint32_t VIRTIO_MAGIC   = 0x74726976u;
static const l4_uint32_t VIRTIO_DEV_INPUT = 18;

enum Mmio_reg : unsigned {
    Mmio_magic           = 0x00,
    Mmio_version         = 0x04,
    Mmio_device_id       = 0x08,
    Mmio_dev_features    = 0x10,
    Mmio_drv_features    = 0x20,
    Mmio_guest_page_size = 0x28,
    Mmio_queue_sel       = 0x30,
    Mmio_queue_num_max   = 0x34,
    Mmio_queue_num       = 0x38,
    Mmio_queue_align     = 0x3c,
    Mmio_queue_pfn       = 0x40,
    Mmio_queue_notify    = 0x050,
    Mmio_status          = 0x070,
    Mmio_config          = 0x100,
};

// Linux input event types / codes
static const l4_uint16_t EV_KEY   = 1;
static const l4_uint16_t EV_ABS   = 3;
static const l4_uint16_t ABS_X    = 0;
static const l4_uint16_t ABS_Y    = 1;
static const l4_uint16_t BTN_LEFT = 0x110;

// -----------------------------------------------------------------------
// VirtIO structures (legacy MMIO v1, little-endian)
// -----------------------------------------------------------------------

struct Vq_desc {
    l4_uint64_t addr;
    l4_uint32_t len;
    l4_uint16_t flags;
    l4_uint16_t next;
};

struct Vq_avail {
    l4_uint16_t flags;
    l4_uint16_t idx;
    l4_uint16_t ring[VQ_SIZE];
};

struct Vq_used_elem {
    l4_uint32_t id;
    l4_uint32_t len;
};

struct Vq_used {
    l4_uint16_t flags;
    l4_uint16_t idx;
    Vq_used_elem ring[VQ_SIZE];
};

struct Virtio_input_event {
    l4_uint16_t type;
    l4_uint16_t code;
    l4_uint32_t value;
};

// -----------------------------------------------------------------------
// Input device state
// -----------------------------------------------------------------------

struct Input_dev {
    L4::Cap<L4Re::Dma_space>  dma;
    L4::Cap<L4Re::Dataspace>  vq_ds;

    l4_addr_t         mmio_base;
    l4_addr_t         vq_virt;
    l4_uint64_t       vq_phys;

    Vq_desc*          desc;
    Vq_avail*         avail;
    Vq_used*          used;
    Virtio_input_event* events;

    l4_uint16_t       avail_idx;
    l4_uint16_t       used_idx;

    bool              is_tablet;
    bool              valid;

    // Pointer state (tablet)
    lv_point_t        pos;
    bool              pressed;

    // Keypad state (keyboard)
    uint32_t          last_key;
    lv_indev_state_t  key_state;
};

static const int MAX_DEVS = 2;
static Input_dev  s_devs[MAX_DEVS];
static Input_dev *s_tablet  = nullptr;
static Input_dev *s_kb      = nullptr;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static inline l4_uint32_t reg_read(l4_addr_t base, unsigned off)
{
    return *reinterpret_cast<volatile l4_uint32_t *>(base + off);
}

static inline void reg_write(l4_addr_t base, unsigned off, l4_uint32_t val)
{
    *reinterpret_cast<volatile l4_uint32_t *>(base + off) = val;
}

static uint32_t linux_to_lv_key(l4_uint16_t code)
{
    switch (code)
    {
    case 103: return LV_KEY_UP;
    case 108: return LV_KEY_DOWN;
    case 105: return LV_KEY_LEFT;
    case 106: return LV_KEY_RIGHT;
    case 28:  return LV_KEY_ENTER;
    case 14:  return LV_KEY_BACKSPACE;
    case  1:  return LV_KEY_ESC;
    case 15:  return LV_KEY_NEXT;
    case 107: return LV_KEY_END;
    case 102: return LV_KEY_HOME;
    case 111: return LV_KEY_DEL;
    default:  return 0;
    }
}

// -----------------------------------------------------------------------
// Virtqueue polling — called from LVGL indev read_cb
// -----------------------------------------------------------------------

static void poll_device(Input_dev &dev)
{
    L4virtio::rmb();
    bool recycled = false;

    while (dev.used_idx != dev.used->idx)
    {
        unsigned slot    = dev.used_idx % VQ_SIZE;
        l4_uint32_t id   = dev.used->ring[slot].id;

        if (id < VQ_SIZE)
        {
            L4virtio::rmb();
            Virtio_input_event const &ev = dev.events[id];

            if (dev.is_tablet)
            {
                if (ev.type == EV_ABS)
                {
                    lv_display_t *disp = lv_display_get_default();
                    if (ev.code == ABS_X)
                    {
                        int32_t w = lv_display_get_horizontal_resolution(disp);
                        dev.pos.x = (int32_t)((l4_uint64_t)ev.value * (l4_uint32_t)w / 32767u);
                    }
                    else if (ev.code == ABS_Y)
                    {
                        int32_t h = lv_display_get_vertical_resolution(disp);
                        dev.pos.y = (int32_t)((l4_uint64_t)ev.value * (l4_uint32_t)h / 32767u);
                    }
                }
                else if (ev.type == EV_KEY && ev.code == BTN_LEFT)
                {
                    dev.pressed = (ev.value != 0);
                }
            }
            else
            {
                // keyboard: value=1 down, value=0 up, value=2 auto-repeat
                if (ev.type == EV_KEY)
                {
                    uint32_t k = linux_to_lv_key(ev.code);
                    if (k)
                    {
                        dev.last_key = k;
                        dev.key_state = (ev.value != 0)
                                        ? LV_INDEV_STATE_PRESSED
                                        : LV_INDEV_STATE_RELEASED;
                    }
                }
            }

            // Recycle the descriptor back to the avail ring
            dev.avail->ring[dev.avail_idx % VQ_SIZE] = (l4_uint16_t)id;
            L4virtio::wmb();
            dev.avail_idx++;
            dev.avail->idx = dev.avail_idx;
            recycled = true;
        }

        dev.used_idx++;
    }

    if (recycled)
    {
        L4virtio::wmb();
        reg_write(dev.mmio_base, Mmio_queue_notify, 0);
    }
}

// -----------------------------------------------------------------------
// LVGL indev read callbacks
// -----------------------------------------------------------------------

static void ptr_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data)
{
    if (!s_tablet) return;
    poll_device(*s_tablet);
    data->point = s_tablet->pos;
    data->state = s_tablet->pressed
                  ? LV_INDEV_STATE_PRESSED
                  : LV_INDEV_STATE_RELEASED;
}

static void key_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data)
{
    if (!s_kb) return;
    poll_device(*s_kb);
    data->key   = s_kb->last_key;
    data->state = s_kb->key_state;
}

// -----------------------------------------------------------------------
// Device initialisation — virtio MMIO v1 (legacy) setup
// -----------------------------------------------------------------------

// Returns false for non-input devices (silent skip); throws on actual errors.
static bool setup_device(Input_dev &dev, l4_addr_t mmio_base)
{
    auto *env = L4Re::Env::env();

    // Sanity checks — silent skip for wrong device type
    if (reg_read(mmio_base, Mmio_magic)     != VIRTIO_MAGIC)    return false;
    if (reg_read(mmio_base, Mmio_version)   != 1)                return false;
    if (reg_read(mmio_base, Mmio_device_id) != VIRTIO_DEV_INPUT) return false;

    // Detect tablet vs keyboard via config space:
    //   write select=VIRTIO_INPUT_CFG_EV_BITS(0x11), subsel=EV_ABS(3)
    //   read size: > 0 means EV_ABS is supported → tablet
    volatile l4_uint8_t *cfg =
        reinterpret_cast<volatile l4_uint8_t *>(mmio_base + Mmio_config);
    cfg[0] = 0x11;  // VIRTIO_INPUT_CFG_EV_BITS
    cfg[1] = 3;     // EV_ABS
    dev.is_tablet = (cfg[2] > 0);

    printf("[lv_input] virtio-input at 0x%lx: %s\n",
           mmio_base, dev.is_tablet ? "tablet" : "keyboard");

    // DMA space — Phys_space gives identity mapping on QEMU virt (no IOMMU)
    dev.dma = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4Re::Dma_space>(),
                           "alloc dma cap");
    L4Re::chksys(env->user_factory()->create(dev.dma), "create dma space");
    L4Re::chksys(dev.dma->associate(
            L4::Ipc::Cap<L4::Task>::from_ci(L4_INVALID_CAP),
            L4Re::Dma_space::Space_attrib::Phys_space), "associate phys");

    // Virtqueue memory: 2 pages, physically contiguous + pinned
    dev.vq_ds = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>(),
                             "alloc vq ds cap");
    L4Re::chksys(env->mem_alloc()->alloc(VQ_MEM_SIZE, dev.vq_ds,
                                         L4Re::Mem_alloc::Continuous
                                         | L4Re::Mem_alloc::Pinned),
                 "alloc vq mem");

    dev.vq_virt = 0;
    L4Re::chksys(env->rm()->attach(&dev.vq_virt, VQ_MEM_SIZE,
                                   L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
                                   L4::Ipc::make_cap_rw(dev.vq_ds),
                                   0, L4_PAGESHIFT),
                 "map vq mem");

    memset(reinterpret_cast<void *>(dev.vq_virt), 0, VQ_MEM_SIZE);

    l4_size_t sz = VQ_MEM_SIZE;
    L4Re::chksys(dev.dma->map(L4::Ipc::make_cap_rw(dev.vq_ds), 0, &sz,
                               L4Re::Dma_space::Attributes::None,
                               L4Re::Dma_space::Direction::Bidirectional,
                               &dev.vq_phys),
                 "dma map vq");

    // Ring pointers within the virtqueue allocation
    dev.desc   = reinterpret_cast<Vq_desc *>(dev.vq_virt + VQ_DESC_OFF);
    dev.avail  = reinterpret_cast<Vq_avail *>(dev.vq_virt + VQ_AVAIL_OFF);
    dev.used   = reinterpret_cast<Vq_used *>(dev.vq_virt + VQ_USED_OFF);
    dev.events = reinterpret_cast<Virtio_input_event *>(dev.vq_virt + VQ_EVT_OFF);
    dev.avail_idx = 0;
    dev.used_idx  = 0;

    // VirtIO v1 (legacy) initialisation sequence
    reg_write(mmio_base, Mmio_status, 0);                // reset
    reg_write(mmio_base, Mmio_status, 1 | 2);            // ACKNOWLEDGE | DRIVER
    l4_uint32_t feat = reg_read(mmio_base, Mmio_dev_features);
    reg_write(mmio_base, Mmio_drv_features, feat);
    reg_write(mmio_base, Mmio_guest_page_size, L4_PAGESIZE);

    // Queue 0 (eventq): device → driver
    reg_write(mmio_base, Mmio_queue_sel, 0);
    l4_uint32_t qmax = reg_read(mmio_base, Mmio_queue_num_max);
    l4_uint32_t qnum = (qmax < VQ_SIZE) ? qmax : VQ_SIZE;
    reg_write(mmio_base, Mmio_queue_num, qnum);
    reg_write(mmio_base, Mmio_queue_align, L4_PAGESIZE);
    reg_write(mmio_base, Mmio_queue_pfn,
              static_cast<l4_uint32_t>(dev.vq_phys >> L4_PAGESHIFT));

    reg_write(mmio_base, Mmio_status, 1 | 2 | 4);       // + DRIVER_OK

    // Descriptors: each slot points to one event buffer, writable by device
    for (unsigned i = 0; i < qnum; ++i)
    {
        dev.desc[i].addr  = dev.vq_phys + VQ_EVT_OFF
                            + i * sizeof(Virtio_input_event);
        dev.desc[i].len   = sizeof(Virtio_input_event);
        dev.desc[i].flags = 2;  // VRING_DESC_F_WRITE (device writes)
        dev.desc[i].next  = 0;
    }

    // Post all descriptors to the avail ring and kick the device
    for (unsigned i = 0; i < qnum; ++i)
        dev.avail->ring[i] = static_cast<l4_uint16_t>(i);
    L4virtio::wmb();
    dev.avail->idx = static_cast<l4_uint16_t>(qnum);
    dev.avail_idx  = static_cast<l4_uint16_t>(qnum);
    L4virtio::wmb();
    reg_write(mmio_base, Mmio_queue_notify, 0);

    dev.mmio_base = mmio_base;
    dev.valid     = true;
    dev.pos       = {0, 0};
    dev.pressed   = false;
    dev.last_key  = 0;
    dev.key_state = LV_INDEV_STATE_RELEASED;

    return true;
}

// -----------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------

void lv_port_input_init()
{
    auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("input");
    if (!vbus.is_valid())
    {
        printf("[lv_input] No 'input' cap — input disabled\n");
        return;
    }

    int num_devs = 0;
    L4vbus::Device vdev;
    l4vbus_device_t di;

    while (vbus->root().next_device(&vdev, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
        if (vdev.is_compatible("virtio,mmio") != 1) continue;
        if (num_devs >= MAX_DEVS) break;

        // Collect MMIO resource
        l4_addr_t mmio_start = 0;
        l4_size_t mmio_size  = 0;
        bool found_mmio = false;

        for (unsigned i = 0; i < di.num_resources; ++i)
        {
            l4vbus_resource_t res;
            if (vdev.get_resource(i, &res) != L4_EOK) continue;
            if (res.type == L4VBUS_RESOURCE_MEM && !found_mmio)
            {
                mmio_start = (l4_addr_t)res.start;
                mmio_size  = res.end - res.start + 1;
                found_mmio = true;
            }
        }
        if (!found_mmio) continue;

        // Map MMIO uncached; device registers start at mmio_start
        l4_addr_t page_off  = mmio_start & (l4_addr_t)(L4_PAGESIZE - 1);
        l4_addr_t phys_base = mmio_start - page_off;
        l4_size_t map_size  = (l4_size_t)((mmio_size + page_off + L4_PAGESIZE - 1)
                                           & ~(l4_size_t)(L4_PAGESIZE - 1));
        l4_addr_t mmio_virt = 0;
        int r = -1;

        // Try sigma0 identity mapping (phys == virt) to bypass Moe RM
        // object_pool restriction on non-Moe objects.
        auto sig0 = L4Re::Env::env()->get_cap<void>("sigma0");
        if (sig0.is_valid()) {
            int sr = l4sigma0_map_iomem(sig0.cap(), phys_base, phys_base,
                                        map_size, 0 /* uncached */);
            if (sr == 0) {
                mmio_virt = phys_base;
                r = 0;
            }
        }
        // Fallback: vbus-as-DS (works with ITAS in-process RM / ned)
        if (r < 0) {
            auto iods = L4::Ipc::make_cap_rw(
                L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));
            mmio_virt = 0;
            r = L4Re::Env::env()->rm()->attach(
                &mmio_virt, map_size,
                L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW
                | L4Re::Rm::F::Cache_uncached,
                iods, phys_base, L4_PAGESHIFT);
        }
        if (r < 0)
        {
            printf("[lv_input] Failed to map MMIO 0x%lx (err %d)\n",
                   mmio_start, r);
            continue;
        }
        l4_addr_t dev_base = mmio_virt + page_off;

        Input_dev &idev = s_devs[num_devs];
        bool ok = false;
        try { ok = setup_device(idev, dev_base); }
        catch (L4::Runtime_error const &e)
        {
            printf("[lv_input] setup error at 0x%lx: %s\n", mmio_start, e.str());
        }
        if (!ok)
        {
            L4Re::Env::env()->rm()->detach(mmio_virt, nullptr);
            continue;
        }

        if (idev.is_tablet && !s_tablet)
            s_tablet = &idev;
        else if (!idev.is_tablet && !s_kb)
            s_kb = &idev;

        ++num_devs;
    }

    if (num_devs == 0)
    {
        printf("[lv_input] No virtio-input devices found\n");
        return;
    }

    // Register LVGL indevs for discovered devices
    if (s_tablet)
    {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, ptr_read_cb);
        lv_timer_set_period(lv_indev_get_read_timer(indev), 16);
        printf("[lv_input] Registered pointer indev (tablet)\n");
    }
    if (s_kb)
    {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(indev, key_read_cb);
        lv_timer_set_period(lv_indev_get_read_timer(indev), 16);
        printf("[lv_input] Registered keypad indev (keyboard)\n");
    }
}
