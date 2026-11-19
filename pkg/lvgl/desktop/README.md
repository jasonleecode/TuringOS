# LVGL 桌面

从 QEMU virt 的串口运行 `run lvgl-desktop &`。在图形窗口单击开始菜单或桌面图标，拖动标题栏可以移动窗口。单击任务栏中的窗口名称可以最小化或恢复窗口。

## 美术资源

`src/meadow.rgb565` 是 800 × 600 的小端 RGB565 壁纸，通过 L4Re 的 `SRC_DATA` 嵌入程序。原图由内置 imagegen 生成，提示词如下。

> 4:3 desktop wallpaper photograph, classic early-2000s aesthetic. Rolling green hills, a deep blue sky and sunlit scattered cumulus clouds. A long gently curving grass ridge crosses the lower half. Clean natural landscape, no buildings, trees, text or interface.

图标由 LVGL 绘图接口绘制。中文标签使用 Noto Sans CJK SC 的字形子集，授权条款见 [FONT-LICENSE](FONT-LICENSE)。新增中文标签后，在本目录运行 `python3 mkfont.py /path/to/NotoSansCJK-Regular.ttc` 即可更新字形表，脚本需要 Pillow。
