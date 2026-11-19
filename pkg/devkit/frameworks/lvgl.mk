# lv_conf.h and the public lvgl/lvgl.h come from the port's configured LVGL tree.
PRIVATE_INCDIR += $(L4DIR)/pkg/lvgl/liblvgl/src
REQUIRES_LIBS += lvgl-port
