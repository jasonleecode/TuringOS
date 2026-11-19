# LVGL 桌面

从 QEMU virt 的串口运行 `run lvgl-desktop &`。在图形窗口单击开始菜单或桌面图标，拖动标题栏可以移动窗口。单击任务栏中的窗口名称可以最小化或恢复窗口。

## 扫雷

单击桌面上的「扫雷」旗帜，或从「开始」菜单的「程序」栏打开游戏。棋盘有 9 × 9 格，共 10 颗雷。首次翻开的格子及其周围都安全。数字表示相邻八格中的雷数，空白区域会自动展开。

单击「插旗」切换到标记模式，再点格子可插旗或拔旗。再次单击「插旗」回到翻格模式。翻开所有安全格即获胜，踩雷后会显示雷的位置，错旗显示为 X。单击「新一局」重开。最小化窗口会保留当前棋局。

从仓库根目录可以运行棋盘规则测试：

```sh
c++ -std=c++17 pkg/lvgl/desktop/src/minesweeper.cc pkg/lvgl/desktop/tests/minesweeper.cc -o /tmp/test-minesweeper
/tmp/test-minesweeper
```

## 美术资源

`src/meadow.rgb565` 是 800 × 600 的小端 RGB565 壁纸，通过 L4Re 的 `SRC_DATA` 嵌入程序。原图由内置 imagegen 生成，提示词如下。

> 4:3 desktop wallpaper photograph, classic early-2000s aesthetic. Rolling green hills, a deep blue sky and sunlit scattered cumulus clouds. A long gently curving grass ridge crosses the lower half. Clean natural landscape, no buildings, trees, text or interface.

图标由 LVGL 绘图接口绘制。中文标签使用 Noto Sans CJK SC 的字形子集，授权条款见 [FONT-LICENSE](FONT-LICENSE)。新增中文标签后，在本目录运行 `python3 mkfont.py /path/to/NotoSansCJK-Regular.ttc` 即可更新字形表，脚本需要 Pillow。
