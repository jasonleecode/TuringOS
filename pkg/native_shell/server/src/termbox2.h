/*
 * termbox2.h — minimal termbox2-compatible TUI for TuringOS ANSI vcon.
 *
 * Replaces the real termbox2 which requires tcsetattr (broken on L4Re/vcon).
 * Implements only the subset needed by cmd_top; API-compatible so the real
 * termbox2 can be swapped in later without changing callers.
 *
 * Key differences from real termbox2:
 *   - tcsetattr/tcgetattr are no-ops (vcon is already raw)
 *   - Input uses kbd_try_pop() from main.cc (our kbd ring buffer)
 *   - Terminal size defaults to 80x24 (use tb_set_size() to override)
 *   - Double-buffer diff rendering to STDOUT_FILENO via write()
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <unistd.h>

/* ---- colour / attribute constants (subset of real termbox2) ---- */
#define TB_DEFAULT  0x00u
#define TB_BLACK    0x01u
#define TB_RED      0x02u
#define TB_GREEN    0x03u
#define TB_YELLOW   0x04u
#define TB_BLUE     0x05u
#define TB_MAGENTA  0x06u
#define TB_CYAN     0x07u
#define TB_WHITE    0x08u

#define TB_BOLD      0x0100u
#define TB_REVERSE   0x0200u

typedef uintptr_t uintattr_t;

/* ---- event types / keys ---- */
#define TB_EVENT_KEY   1
#define TB_EVENT_NONE  0

#define TB_KEY_ESC    0x1b
#define TB_KEY_CTRL_C 0x03

struct tb_event {
    uint8_t  type;   /* TB_EVENT_KEY or TB_EVENT_NONE */
    uint16_t key;    /* TB_KEY_* or 0 */
    uint32_t ch;     /* printable character, or 0 */
};

/* ---- cell ---- */
struct tb_cell {
    uint32_t   ch;
    uintattr_t fg;
    uintattr_t bg;
};

/* ---- globals ---- */
static int  _tb_w     = 80;
static int  _tb_h     = 24;
static bool _tb_ready = false;

static constexpr int TB_MAX_W = 220;
static constexpr int TB_MAX_H = 60;

static tb_cell _tb_buf [TB_MAX_H][TB_MAX_W];
static tb_cell _tb_prev[TB_MAX_H][TB_MAX_W];

/* declared in main.cc, exposed via commands.h */
extern int kbd_try_pop();

/* ---- helpers ---- */
static inline void _tb_esc(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

/* Map TB_* colour index to ANSI SGR code */
static inline int _tb_ansi_fg(uintattr_t c)
{
    static const int map[] = { 39, 30, 31, 32, 33, 34, 35, 36, 37 };
    int idx = (int)(c & 0xff);
    return (idx >= 0 && idx <= 8) ? map[idx] : 39;
}
static inline int _tb_ansi_bg(uintattr_t c)
{
    static const int map[] = { 49, 40, 41, 42, 43, 44, 45, 46, 47 };
    int idx = (int)(c & 0xff);
    return (idx >= 0 && idx <= 8) ? map[idx] : 49;
}

/* ---- public API ---- */

static inline void tb_set_size(int w, int h)
{
    if (w > 0 && w <= TB_MAX_W) _tb_w = w;
    if (h > 0 && h <= TB_MAX_H) _tb_h = h;
}

static inline int tb_init()
{
    _tb_esc("\033[2J\033[H");   /* clear screen, cursor home */
    _tb_esc("\033[?25l");       /* hide cursor */
    memset(_tb_buf,  0, sizeof(_tb_buf));
    memset(_tb_prev, 0, sizeof(_tb_prev));
    /* Mark all prev cells dirty so first tb_present() does a full redraw */
    for (int y = 0; y < TB_MAX_H; y++)
        for (int x = 0; x < TB_MAX_W; x++)
            _tb_prev[y][x].ch = 0xFFFFFFFFu;
    _tb_ready = true;
    return 0;
}

static inline void tb_shutdown()
{
    if (!_tb_ready) return;
    _tb_esc("\033[0m");         /* reset attributes */
    _tb_esc("\033[?25h");       /* show cursor */
    _tb_esc("\033[2J\033[H");   /* clear screen */
    _tb_ready = false;
}

static inline int  tb_width()  { return _tb_w; }
static inline int  tb_height() { return _tb_h; }

static inline void tb_clear()
{
    for (int y = 0; y < _tb_h; y++)
        for (int x = 0; x < _tb_w; x++)
            _tb_buf[y][x] = { (uint32_t)' ', TB_DEFAULT, TB_DEFAULT };
}

static inline void tb_set_cell(int x, int y, uint32_t ch,
                                uintattr_t fg, uintattr_t bg)
{
    if (x < 0 || x >= _tb_w || y < 0 || y >= _tb_h) return;
    _tb_buf[y][x] = { ch, fg, bg };
}

static inline void tb_print(int x, int y, uintattr_t fg, uintattr_t bg,
                             const char *str)
{
    for (; *str && x < _tb_w; str++, x++)
        tb_set_cell(x, y, (unsigned char)*str, fg, bg);
}

static inline void tb_printf(int x, int y, uintattr_t fg, uintattr_t bg,
                              const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tb_print(x, y, fg, bg, buf);
}

static inline void tb_present()
{
    char out[8192];
    int  pos = 0;

    /* lambda-like flush helper using a nested struct */
    struct Flush {
        char *out; int &pos;
        void operator()(const char *s, int n) {
            if (pos + n >= 8192) { write(STDOUT_FILENO, out, pos); pos = 0; }
            memcpy(out + pos, s, n); pos += n;
        }
    } emit{out, pos};

    uintattr_t cur_fg = ~(uintattr_t)0u, cur_bg = ~(uintattr_t)0u;
    bool cur_bold = false, cur_rev = false;
    int  last_y = -1, last_x = -1;

    for (int y = 0; y < _tb_h; y++) {
        for (int x = 0; x < _tb_w; x++) {
            const tb_cell &nc = _tb_buf [y][x];
            const tb_cell &pc = _tb_prev[y][x];
            if (nc.ch == pc.ch && nc.fg == pc.fg && nc.bg == pc.bg) continue;

            if (y != last_y || x != last_x + 1) {
                char mv[16];
                int n = snprintf(mv, sizeof(mv), "\033[%d;%dH", y + 1, x + 1);
                emit(mv, n);
            }
            last_y = y; last_x = x;

            bool bold = !!(nc.fg & TB_BOLD);
            bool rev  = !!(nc.fg & TB_REVERSE);
            uintattr_t fg = nc.fg & 0xff;
            uintattr_t bg = nc.bg & 0xff;

            if (fg != cur_fg || bg != cur_bg || bold != cur_bold || rev != cur_rev) {
                char attr[32];
                int n = snprintf(attr, sizeof(attr), "\033[0;%d;%d%s%sm",
                    _tb_ansi_fg(fg), _tb_ansi_bg(bg),
                    bold ? ";1" : "", rev ? ";7" : "");
                emit(attr, n);
                cur_fg = fg; cur_bg = bg; cur_bold = bold; cur_rev = rev;
            }

            char ch = (nc.ch >= 0x20 && nc.ch < 0x7f) ? (char)nc.ch : ' ';
            emit(&ch, 1);
            _tb_prev[y][x] = nc;
        }
    }

    if (pos > 0) write(STDOUT_FILENO, out, pos);

    /* Park cursor at bottom row */
    char park[16];
    int n = snprintf(park, sizeof(park), "\033[%d;1H", _tb_h);
    write(STDOUT_FILENO, park, n);
}

static inline int tb_peek_event(struct tb_event *ev, int /*timeout_ms*/)
{
    int c = kbd_try_pop();
    if (c < 0) { ev->type = TB_EVENT_NONE; return TB_EVENT_NONE; }
    ev->type = TB_EVENT_KEY;
    ev->key  = (uint16_t)c;
    ev->ch   = (c >= 0x20 && c < 0x7f) ? (uint32_t)c : 0;
    return TB_EVENT_KEY;
}
