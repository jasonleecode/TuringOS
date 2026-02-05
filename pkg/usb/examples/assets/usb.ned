-- vim:set ft=lua:

-- Include L4 functionality
local L4 = require("L4");

-- Some shortcut for less typing
local l = L4.default_loader;

-- Channel for the programs to talk to each other.
local vbus_usb = l:new_channel();
local usb = l:new_channel();
local fbdrv = l:new_channel();
local ev = l:new_channel();

l:start(
    {
        caps = {
            sigma0 = L4.Env.sigma0,
            icu = L4.Env.icu,
            iommu   = L4.Env.iommu,
            vbus_usb = vbus_usb:svr(),
            fbdrv = fbdrv:svr(),
        },
        log = {"IO", "yellow"},
        l4re_dbg = L4.Dbg.DEBUG2,
    },
    "rom/io rom/usb.io rom/x86-legacy.devs rom/x86-fb.io"
);

-- the usb-server
l:start(
    {
        caps = {
            vbus = vbus_usb,
            svr = usb:svr(),
        },
        log = {"usb-drv", "green"},
        --l4re_dbg = L4.Dbg.All,
    },
    "rom/usb-drv"
)

-- hid driver
l:start(
    {
        caps = {
            ev = ev:svr(),
            dev = usb:create(0, "0627:0001"),   -- QEMU Mouse/Keyboard
            --dev = usb:create(0, "0738:1704"),   -- Mad Catz R.A.T.T.TE
            --dev = usb:create(0, "80ee:0021"),   -- virtual box tablet

        },
        log = {"hid-drv", "white"}
    },
    "rom/hid-drv"
)

if false then
  l:start({ caps = {dev = usb:create(0, "0c45:7160")} },
          "rom/ledfan"
  )
end


-- Start fb-drv (but only if we need to)
local fbdrv_fb = L4.Env.vesa;
if (not fbdrv_fb) then
  fbdrv_fb = l:new_channel();
  l:start({
           caps = {
             vbus = fbdrv,
             fb   = fbdrv_fb:svr(),
           },
           log = { "fbdrv", "r" },
         },
         "rom/fb-drv");
end

local mag_caps = {
    mag = l:new_channel(),
    svc = l:new_channel(),
};

l:start({
    caps = {
        fb   = fbdrv_fb,
        mag  = mag_caps.mag:svr(),
        svc  = mag_caps.svc:svr(),
        ev   = ev,
    },
    log      = { "mag", "g" },
    }, "rom/mag"
);
