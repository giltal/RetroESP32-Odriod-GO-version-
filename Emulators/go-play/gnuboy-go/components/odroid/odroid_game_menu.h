/*
 * odroid_game_menu.h — Lightweight in-game menu for go-play emulators
 *
 * Self-contained: includes a minimal 5x7 pixel font (A-Z, 0-9, &, space)
 * and renders to a SPIRAM framebuffer via ili9341_write_frame_rectangleLE().
 *
 * Usage:  int choice = goplay_game_menu();
 *   Returns: 0=Continue, 1=Save&Continue, 2=Save&Quit, 3=Restart, 4=Quit
 */
#ifndef ODROID_GAME_MENU_H
#define ODROID_GAME_MENU_H

#include <stdint.h>
#include <string.h>
#include "odroid_display.h"
#include "odroid_input.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- Minimal 5x7 font (uppercase A-Z, space, &) ---- */
/* Each char: 7 rows, each byte lower 5 bits = pixel columns L→R */

static const uint8_t _gm_font5x7[][7] = {
    /* [ 0] space */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* [ 1] A */     {0x04,0x0A,0x11,0x1F,0x11,0x11,0x11},
    /* [ 2] B */     {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* [ 3] C */     {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* [ 4] D */     {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* [ 5] E */     {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* [ 6] F */     {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* [ 7] G */     {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    /* [ 8] H */     {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* [ 9] I */     {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* [10] J */     {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* [11] K */     {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* [12] L */     {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* [13] M */     {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* [14] N */     {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* [15] O */     {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* [16] P */     {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* [17] Q */     {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* [18] R */     {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* [19] S */     {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    /* [20] T */     {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* [21] U */     {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* [22] V */     {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    /* [23] W */     {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    /* [24] X */     {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* [25] Y */     {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* [26] Z */     {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* [27] & */     {0x04,0x0A,0x0A,0x04,0x15,0x12,0x0D},
};

static inline int _gm_char_idx(char c) {
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 1 + (c - 'a'); /* treat lowercase as upper */
    if (c == '&') return 27;
    return 0; /* space and anything else */
}

/* Draw one 2x-scaled character (10x14 pixels) at (x,y) into 320-wide buf */
static inline void _gm_draw_char(uint16_t *buf, int x, int y, char c, uint16_t color) {
    int idx = _gm_char_idx(c);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = _gm_font5x7[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                int px = x + col * 2;
                int py = y + row * 2;
                if (px >= 0 && px+1 < 320 && py >= 0 && py+1 < 240) {
                    buf[py * 320 + px]       = color;
                    buf[py * 320 + px + 1]   = color;
                    buf[(py+1) * 320 + px]   = color;
                    buf[(py+1) * 320 + px+1] = color;
                }
            }
        }
    }
}

/* Draw a null-terminated string; returns pixel width */
static inline int _gm_draw_str(uint16_t *buf, int x, int y, const char *s, uint16_t color) {
    int ox = x;
    while (*s) {
        _gm_draw_char(buf, x, y, *s, color);
        x += 12; /* 10px char + 2px gap */
        s++;
    }
    return x - ox;
}

/* Measure string width in pixels */
static inline int _gm_str_width(const char *s) {
    int n = 0;
    while (*s) { n++; s++; }
    return n * 12;
}

/* Fill a rectangle in buf (320-wide) */
static inline void _gm_fill_rect(uint16_t *buf, int x, int y, int w, int h, uint16_t color) {
    for (int r = y; r < y + h && r < 240; r++) {
        for (int c = x; c < x + w && c < 320; c++) {
            if (r >= 0 && c >= 0)
                buf[r * 320 + c] = color;
        }
    }
}

/* Color definitions (RGB565 big-endian for LE write) */
#define _GM_NAVY    0x0010
#define _GM_WHITE   0xFFFF
#define _GM_YELLOW  0xFFE0
#define _GM_BLACK   0x0000
#define _GM_DKGRAY  0x4208

/*
 * goplay_game_menu — Show a 5-option in-game menu
 *
 * Returns: 0=Continue, 1=Save & Continue, 2=Save & Quit,
 *          3=Restart Game, 4=Quit to Menu
 */
static int goplay_game_menu(void)
{
    uint16_t *mbuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!mbuf) return 0; /* fallback: continue */

    const char *title = "GAME MENU";
    const char *opts[] = {
        "CONTINUE",
        "SAVE & CONTINUE",
        "SAVE & QUIT",
        "RESTART GAME",
        "QUIT TO MENU"
    };
    const int nopts = 5;
    int sel = 0;

    /* Box geometry */
    const int BX = 40, BY = 30, BW = 240, BH = 185;
    const int title_y = BY + 8;
    const int sep_y = title_y + 18;
    const int opt_y0 = sep_y + 10;
    const int opt_step = 28;

    /* Wait for MENU button release */
    odroid_gamepad_state js;
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        odroid_input_gamepad_read(&js);
    } while (js.values[ODROID_INPUT_MENU]);

    int last_dir = -1;
    bool redraw = true;

    while (1) {
        if (redraw) {
            /* Background */
            for (int i = 0; i < 320 * 240; i++) mbuf[i] = _GM_BLACK;

            /* Box */
            _gm_fill_rect(mbuf, BX, BY, BW, BH, _GM_NAVY);
            /* Border */
            _gm_fill_rect(mbuf, BX, BY, BW, 2, _GM_WHITE);
            _gm_fill_rect(mbuf, BX, BY + BH - 2, BW, 2, _GM_WHITE);
            _gm_fill_rect(mbuf, BX, BY, 2, BH, _GM_WHITE);
            _gm_fill_rect(mbuf, BX + BW - 2, BY, 2, BH, _GM_WHITE);

            /* Title (centered) */
            int tw = _gm_str_width(title);
            _gm_draw_str(mbuf, BX + (BW - tw) / 2, title_y, title, _GM_WHITE);

            /* Separator */
            _gm_fill_rect(mbuf, BX + 10, sep_y, BW - 20, 2, _GM_WHITE);

            /* Options */
            for (int i = 0; i < nopts; i++) {
                int oy = opt_y0 + i * opt_step;
                uint16_t fg, bg;
                if (i == sel) {
                    _gm_fill_rect(mbuf, BX + 6, oy - 3, BW - 12, 20, _GM_YELLOW);
                    fg = _GM_BLACK;
                } else {
                    fg = _GM_WHITE;
                }
                int ow = _gm_str_width(opts[i]);
                _gm_draw_str(mbuf, BX + (BW - ow) / 2, oy, opts[i], fg);
            }

            ili9341_write_frame_rectangleLE(0, 0, 320, 240, mbuf);
            redraw = false;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
        odroid_input_gamepad_read(&js);

        int new_sel = sel;
        if (js.values[ODROID_INPUT_DOWN] && last_dir != ODROID_INPUT_DOWN)
            new_sel = (sel + 1) % nopts;
        if (js.values[ODROID_INPUT_UP] && last_dir != ODROID_INPUT_UP)
            new_sel = (sel - 1 + nopts) % nopts;

        if (new_sel != sel) {
            sel = new_sel;
            redraw = true;
        }

        if (js.values[ODROID_INPUT_A]) {
            break;
        }
        if (js.values[ODROID_INPUT_B] || js.values[ODROID_INPUT_MENU]) {
            sel = 0; /* Continue */
            break;
        }

        /* Debounce direction */
        last_dir = -1;
        if (js.values[ODROID_INPUT_UP])   last_dir = ODROID_INPUT_UP;
        if (js.values[ODROID_INPUT_DOWN]) last_dir = ODROID_INPUT_DOWN;
    }

    /* Wait for button release */
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        odroid_input_gamepad_read(&js);
    } while (js.values[ODROID_INPUT_A] || js.values[ODROID_INPUT_B] || js.values[ODROID_INPUT_MENU]);

    heap_caps_free(mbuf);
    return sel;
}

#endif /* ODROID_GAME_MENU_H */
