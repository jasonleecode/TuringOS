#pragma once

// Runs the LVGL event loop on the task's framebuffer and input capabilities.
int lv_port_run(const char *name, void (*create_ui)());
void lv_port_quit();
