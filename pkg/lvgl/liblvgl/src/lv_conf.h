/* clang-format off */
#if 1 /*Set it to "1" to enable the content below*/

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 32

/*=========================
   MEMORY SETTINGS
 *=========================*/
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE   (2U * 1024U * 1024U)  /* 2 MB */
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC   malloc
#define LV_MEM_POOL_FREE    free

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM 0
#define LV_DEF_REFR_PERIOD 16  /* [ms] ~60 fps target */

/*=====================
   OS / THREADING
 *=====================*/
#define LV_USE_OS LV_OS_NONE

/*=====================
   DRAWING
 *=====================*/
#define LV_USE_DRAW_SW 1
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0

/*===================
   LOGGING
 *===================*/
#define LV_USE_LOG      1
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF   1
#define LV_LOG_TIMESTAMP 0

/*===================
   ASSERT
 *===================*/
#define LV_USE_ASSERT_NULL    1
#define LV_USE_ASSERT_MALLOC  1
#define LV_USE_ASSERT_STYLE   0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ     0

/*=====================
   FONT USAGE
 *=====================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*=======================
   FILE SYSTEM — OFF
 *=======================*/
#define LV_USE_FS_POSIX  0
#define LV_USE_FS_STDIO  0
#define LV_USE_FS_WIN32  0
#define LV_USE_FS_FATFS  0
#define LV_USE_FS_MEMFS  0
#define LV_USE_FS_LITTLEFS 0

/*======================
   IMAGE CODECS — OFF
 *======================*/
#define LV_USE_LODEPNG    0
#define LV_USE_LIBPNG     0
#define LV_USE_BMP        0
#define LV_USE_TJPGD      0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF        0
#define LV_USE_QRCODE     0
#define LV_USE_BARCODE    0
#define LV_USE_FREETYPE   0
#define LV_USE_TINY_TTF   0
#define LV_USE_RLOTTIE    0
#define LV_USE_FFMPEG     0
#define LV_USE_VECTOR_GRAPHIC 0

/*======================
   GPU BACKENDS — OFF
 *======================*/
#define LV_USE_DRAW_VGLITE    0
#define LV_USE_DRAW_PXP       0
#define LV_USE_DRAW_SDL       0
#define LV_USE_DRAW_OPENGLES  0

/*=====================
   OTHERS — OFF
 *=====================*/
#define LV_USE_SNAPSHOT     0
#define LV_USE_SYSMON       0
#define LV_USE_PROFILER     0
#define LV_USE_MONKEY       0
#define LV_USE_GRIDNAV      0
#define LV_USE_FRAGMENT     0
#define LV_USE_IMGFONT      0
#define LV_USE_MSG          0
#define LV_USE_IME_PINYIN   0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*======================
   WIDGETS
 *======================*/
#define LV_USE_ARC       1
#define LV_USE_BAR       1
#define LV_USE_BTN       1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CALENDAR  1
#define LV_USE_CANVAS    1
#define LV_USE_CHART     1
#define LV_USE_CHECKBOX  1
#define LV_USE_COLORWHEEL 1
#define LV_USE_DROPDOWN  1
#define LV_USE_IMG       1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_KEYBOARD  1
#define LV_USE_LABEL     1
#define LV_USE_LED       1
#define LV_USE_LINE      1
#define LV_USE_LIST      1
#define LV_USE_MENU      1
#define LV_USE_MSGBOX    1
#define LV_USE_ROLLER    1
#define LV_USE_SCALE     1
#define LV_USE_SLIDER    1
#define LV_USE_SPAN      1
#define LV_USE_SPINBOX   1
#define LV_USE_SPINNER   1
#define LV_USE_SWITCH    1
#define LV_USE_TABLE     1
#define LV_USE_TABVIEW   1
#define LV_USE_TEXTAREA  1
#define LV_USE_TILEVIEW  1
#define LV_USE_WIN       1

/*======================
   DEMOS
 *======================*/
#define LV_USE_DEMO_WIDGETS         1
#define LV_DEMO_WIDGETS_SLIDESHOW   0
#define LV_USE_DEMO_BENCHMARK       0
#define LV_USE_DEMO_STRESS          0
#define LV_USE_DEMO_MUSIC           0
#define LV_USE_DEMO_FLEX_LAYOUT     0
#define LV_USE_DEMO_MULTILANG       0
#define LV_USE_DEMO_SCROLL          0
#define LV_USE_DEMO_VECTOR_GRAPHIC  0

#endif /* LV_CONF_H */

#endif /*End of "Content enable"*/
