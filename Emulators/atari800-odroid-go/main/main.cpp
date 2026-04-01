/*
 * Atari800 emulator for Odroid Go (ESP-IDF)
 *
 * Glue layer: wraps libatari800 core with Odroid Go hardware drivers.
 * Based on Peter Barrett's esp_8_bit emu_atari800.cpp and the
 * prosystem-odroid-go main.c pattern.
 *
 * Copyright (c) 2024 RetroESP32 project
 * Atari800 core: GPL v2+ (Atari800 development team)
 * Platform integration: ISC (Peter Barrett)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include <ctype.h>
#include <errno.h>

/* ---------- Odroid HAL headers (C code — needs extern "C") ---------- */
extern "C" {
#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_sdcard.h"
}

/* ---------- atari800 core headers (already C++) ---------- */
#include "config.h"
#include "libatari800.h"
#include "libatari800_main.h"
#include "atari.h"
#include "akey.h"
#include "input.h"
#include "memory.h"
#include "screen.h"
#include "sound.h"
#include "cartridge.h"
#include "sio.h"
#include "afile.h"
#include "pokey.h"
#include "statesav.h"
#include "libatari800_statesav.h"

#include <sys/stat.h>
#include <sys/unistd.h>

/* These are defined in the libatari800 core but we need access */
extern int libatari800_init(int argc, char **argv);
extern int libatari800_next_frame(input_template_t *input);
extern ULONG *Screen_atari;
extern UBYTE *MEMORY_mem;
extern UBYTE *under_atarixl_os;
extern UBYTE *under_cart809F;
extern UBYTE *under_cartA0BF;
extern int Atari800_machine_type;
extern int INPUT_key_code;
extern int INPUT_key_consol;
void Sound_Callback(UBYTE *buffer, unsigned int size);
void CARTRIDGE_Remove(void);
void CASSETTE_Remove(void);

/* ---------- Constants ---------- */
static const char *TAG = "atari800-go";
static const char *SD_BASE_PATH = "/sd";

/* Diagnostic: saved at init so we can print when serial connects */
static char diag_info[256] = "(not yet set)";
static int diag_printed = 0;

#define ATARI_SCREEN_WIDTH   384
#define ATARI_SCREEN_HEIGHT  240
#define ATARI_VISIBLE_WIDTH  336   /* middle 336 of 384 */
#define ATARI_VISIBLE_OFFSET  24   /* (384-336)/2 = 24 pixels on each side */
#define DISPLAY_WIDTH        320
#define DISPLAY_HEIGHT       240
#define CROP_OFFSET          ((ATARI_VISIBLE_WIDTH - DISPLAY_WIDTH) / 2) /* 8 */
#define SRC_X_START          (ATARI_VISIBLE_OFFSET + CROP_OFFSET)       /* 32 */

/* Audio: Atari POKEY native rate ~15720 Hz (262 lines * 60 fps) */
#define AUDIO_SAMPLE_RATE    15720

/* Input bit masks from emu_atari800_ref.cpp */
#define JOY_STICK_FORWARD  0x01
#define JOY_STICK_BACK     0x02
#define JOY_STICK_LEFT     0x04
#define JOY_STICK_RIGHT    0x08

/* ---------- Globals ---------- */
static QueueHandle_t vidQueue;
static uint8_t *framebuffer[2];   /* double-buffered 320 * 240 indexed color */
static int fb_index = 0;          /* which framebuffer emu_step writes to next */
static uint16_t rgb565_palette[256];
static int16_t *sampleBuffer;

/* Joystick / trigger state (read by PLATFORM_PORT / PLATFORM_TRIG) */
static int _joy[4]  = {0, 0, 0, 0};
static int _trig[4] = {0, 0, 0, 0};
static int console_keys = 7;      /* INPUT_CONSOL_NONE */

/* Frame skip control — render video every other frame for ~2x speedup.
   atari800_draw_frame is read by LIBATARI800_Frame → ANTIC_Frame to skip
   pixel rendering on non-display frames. */
int atari800_draw_frame = 1;

/* Sound_desired must be provided — sound.cpp declares it extern */
Sound_setup_t Sound_desired = {
    AUDIO_SAMPLE_RATE,
    1,  /* 8-bit samples (1 byte) */
    1,  /* mono */
    0,  /* buffer_ms — auto */
    0   /* buffer_frames — auto */
};

/* ---------- Save state ---------- */
static UBYTE *statesav_buffer = NULL;       /* 210 KB in SPIRAM */
static statesav_tags_t statesav_tags;       /* subsystem offsets */
static char save_path[256] = "";            /* /sd/odroid/data/a800/romname.sav */

/* AtariPot: paddle potentiometer value (0-228), used by input.cpp */
unsigned char AtariPot = 228;

/* Paddle potentiometer on GPIO15 (ADC2_CHANNEL_3)
 * Connect a 10K potentiometer: one end to GND, other to 3.3V, wiper to GPIO15.
 * The ADC reading (0-4095) maps to POKEY POT range (1-228). */
static bool paddle_adc_enabled = false;
#define PADDLE_ADC_CHANNEL  ADC2_CHANNEL_3
#define PADDLE_ADC_SAMPLES  8   /* samples per frame for median filter */
#define PADDLE_DEAD_ZONE    4   /* ADC units — small hysteresis */
#define PADDLE_POT_MIN      1
#define PADDLE_POT_MAX      228
#define PADDLE_ADC_LO       0    /* ADC full range: pot at GND end */
#define PADDLE_ADC_HI       4095 /* ADC full range: pot at 3.3V end */
#define PADDLE_DETECT_LO    50   /* ADC must be above this to detect pot */
#define PADDLE_DETECT_SPREAD 300 /* two readings must be within this range */
/* Software paddle acceleration: starts slow for precision, ramps up when held */
#define PADDLE_STEP_SLOW    1    /* POT units/frame for first ~250ms */
#define PADDLE_STEP_MED     3    /* POT units/frame after ~250ms */
#define PADDLE_STEP_FAST    6    /* POT units/frame after ~500ms */
#define PADDLE_ACCEL_MED    15   /* frames before medium speed (~250ms) */
#define PADDLE_ACCEL_FAST   30   /* frames before fast speed  (~500ms) */

/* PLATFORM_kbd_joy_0_enabled declared in platform.h, may be referenced */
int PLATFORM_kbd_joy_0_enabled = 0;
int PLATFORM_kbd_joy_1_enabled = 0;

/* ===================================================================
 * RGB565 palette generation
 * Derived from atari800 palette in emu_atari800_ref.cpp
 * =================================================================== */

static const uint32_t atari_palette_rgb[256] = {
    0x000000,0x111111,0x222222,0x333333,0x444444,0x555555,0x666666,0x777777,
    0x888888,0x999999,0xaaaaaa,0xbbbbbb,0xcccccc,0xdddddd,0xeeeeee,0xffffff,
    0x190700,0x2a1800,0x3b2900,0x4c3a00,0x5d4b00,0x6e5c00,0x7f6d00,0x907e09,
    0xa18f1a,0xb3a02b,0xc3b13c,0xd4c24d,0xe5d35e,0xf7e46f,0xfff582,0xffff96,
    0x310000,0x3f0000,0x531700,0x642800,0x753900,0x864a00,0x975b0a,0xa86c1b,
    0xb97d2c,0xca8e3d,0xdb9f4e,0xecb05f,0xfdc170,0xffd285,0xffe39c,0xfff4b2,
    0x420404,0x4f0000,0x600800,0x711900,0x822a0d,0x933b1e,0xa44c2f,0xb55d40,
    0xc66e51,0xd77f62,0xe89073,0xf9a183,0xffb298,0xffc3ae,0xffd4c4,0xffe5da,
    0x410103,0x50000f,0x61001b,0x720f2b,0x83203c,0x94314d,0xa5425e,0xb6536f,
    0xc76480,0xd87591,0xe986a2,0xfa97b3,0xffa8c8,0xffb9de,0xffcaef,0xfbdcf6,
    0x330035,0x440041,0x55004c,0x660c5c,0x771d6d,0x882e7e,0x993f8f,0xaa50a0,
    0xbb61b1,0xcc72c2,0xdd83d3,0xee94e4,0xffa5e4,0xffb6e9,0xffc7ee,0xffd8f3,
    0x1d005c,0x2e0068,0x400074,0x511084,0x622195,0x7332a6,0x8443b7,0x9554c8,
    0xa665d9,0xb776ea,0xc887eb,0xd998eb,0xe9a9ec,0xfbbaeb,0xffcbef,0xffdff9,
    0x020071,0x13007d,0x240b8c,0x351c9d,0x462dae,0x573ebf,0x684fd0,0x7960e1,
    0x8a71f2,0x9b82f7,0xac93f7,0xbda4f7,0xceb5f7,0xdfc6f7,0xf0d7f7,0xffe8f8,
    0x000068,0x000a7c,0x081b90,0x192ca1,0x2a3db2,0x3b4ec3,0x4c5fd4,0x5d70e5,
    0x6e81f6,0x7f92ff,0x90a3ff,0xa1b4ff,0xb2c5ff,0xc3d6ff,0xd4e7ff,0xe5f8ff,
    0x000a4d,0x001b63,0x002c79,0x023d8f,0x134ea0,0x245fb1,0x3570c2,0x4681d3,
    0x5792e4,0x68a3f5,0x79b4ff,0x8ac5ff,0x9bd6ff,0xace7ff,0xbdf8ff,0xceffff,
    0x001a26,0x002b3c,0x003c52,0x004d68,0x065e7c,0x176f8d,0x28809e,0x3991af,
    0x4aa2c0,0x5bb3d1,0x6cc4e2,0x7dd5f3,0x8ee6ff,0x9ff7ff,0xb0ffff,0xc1ffff,
    0x01250a,0x023610,0x004622,0x005738,0x05684d,0x16795e,0x278a6f,0x389b80,
    0x49ac91,0x5abda2,0x6bceb3,0x7cdfc4,0x8df0d5,0x9effe5,0xaffff1,0xc0fffd,
    0x04260d,0x043811,0x054713,0x005a1b,0x106b1b,0x217c2c,0x328d3d,0x439e4e,
    0x54af5f,0x65c070,0x76d181,0x87e292,0x98f3a3,0xa9ffb3,0xbaffbf,0xcbffcb,
    0x00230a,0x003510,0x044613,0x155613,0x266713,0x377813,0x488914,0x599a25,
    0x6aab36,0x7bbc47,0x8ccd58,0x9dde69,0xaeef7a,0xbfff8b,0xd0ff97,0xe1ffa3,
    0x001707,0x0e2808,0x1f3908,0x304a08,0x415b08,0x526c08,0x637d08,0x748e0d,
    0x859f1e,0x96b02f,0xa7c140,0xb8d251,0xc9e362,0xdaf473,0xebff82,0xfcff8e,
    0x1b0701,0x2c1801,0x3c2900,0x4d3b00,0x5f4c00,0x705e00,0x816f00,0x938009,
    0xa4921a,0xb2a02b,0xc7b43d,0xd8c64e,0xead760,0xf6e46f,0xfffa84,0xffff99,
};

static void build_rgb565_palette(void)
{
    for (int i = 0; i < 256; i++) {
        uint32_t c = atari_palette_rgb[i];
        uint16_t r = (c >> 16) & 0xFF;
        uint16_t g = (c >> 8)  & 0xFF;
        uint16_t b = (c)       & 0xFF;
        rgb565_palette[i] = ((r << 8) & 0xF800) | ((g << 3) & 0x07E0) | (b >> 3);
    }
}

/* ===================================================================
 * Video task — runs on core 1
 * Reads indexed framebuffer, converts via palette, sends to LCD
 * =================================================================== */

static void videoTask(void *arg)
{
    while (1) {
        uint8_t *param;
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        /* Use the existing atari7800 palette display path which does
           per-pixel palette lookup from 8-bit indexed → RGB565 */
        ili9341_write_frame_atari7800(param, rgb565_palette);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }

    vTaskDelete(NULL);
    while (1) {}
}

/* ===================================================================
 * Platform interface functions required by libatari800 core
 * (PLATFORM_Keyboard, PLATFORM_PORT, PLATFORM_TRIG are called from
 *  libatari800_main.cpp → LIBATARI800_Frame → INPUT_Frame)
 * =================================================================== */

int PLATFORM_Keyboard(void)
{
    return INPUT_key_code;
}

int PLATFORM_PORT(int num)
{
    if (num == 0)
        return (_joy[0] | (_joy[1] << 4)) ^ 0xFF;
    if (num == 1)
        return (_joy[2] | (_joy[3] << 4)) ^ 0xFF;
    return 0xFF;
}

int PLATFORM_TRIG(int num)
{
    if (num < 0 || num >= 4)
        return 1;
    return _trig[num] ^ 1;
}

void LIBATARI800_Mouse(void)
{
    /* No mouse on Odroid Go */
}

int LIBATARI800_Input_Initialise(int *argc, char *argv[])
{
    return TRUE;
}

/* ===================================================================
 * Memory pre-allocation
 * Must be called BEFORE libatari800_init() so the core finds non-NULL
 * pointers and skips its own malloc (which uses generic heap).
 * =================================================================== */

static void atari800_preallocate(void)
{
    ESP_LOGI(TAG, "Pre-allocating atari800 buffers...");

    /* Screen_atari: 384 * 240 = 92160 bytes indexed color
       The reference implementation (Peter Barrett) places this in internal
       DMA-capable SRAM for fastest access — ANTIC does many scattered
       UWORD/ULONG writes every scanline.  Internal SRAM avoids SPIRAM
       cache-coherency issues between cores and is ~3x faster for random
       byte writes.  Fall back to SPIRAM if internal DRAM is exhausted. */
    Screen_atari = (ULONG *)heap_caps_malloc(ATARI_SCREEN_WIDTH * ATARI_SCREEN_HEIGHT,
                                             MALLOC_CAP_DMA);
    if (!Screen_atari) {
        ESP_LOGW(TAG, "Internal DRAM insufficient for Screen_atari, falling back to SPIRAM");
        Screen_atari = (ULONG *)heap_caps_malloc(ATARI_SCREEN_WIDTH * ATARI_SCREEN_HEIGHT,
                                                 MALLOC_CAP_SPIRAM);
    }
    if (!Screen_atari) {
        ESP_LOGE(TAG, "Failed to allocate Screen_atari!");
        abort();
    }
    memset(Screen_atari, 0, ATARI_SCREEN_WIDTH * ATARI_SCREEN_HEIGHT);

    /* MEMORY_mem: 64KB + 4 bytes (6502 address space)
       Place in INTERNAL DRAM for fastest possible access — this is the single
       most performance-critical buffer.  Every CPU opcode fetch, operand read,
       stack push/pop hits MEMORY_mem.  SPIRAM would add ~100-cycle SPI penalty
       on every cache miss.  Internal DRAM is accessed at CPU speed (240 MHz). */
    MEMORY_mem = (UBYTE *)heap_caps_malloc(65536 + 4,
                                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!MEMORY_mem) {
        ESP_LOGW(TAG, "Internal DRAM insufficient for MEMORY_mem, falling back to SPIRAM");
        MEMORY_mem = (UBYTE *)heap_caps_malloc(65536 + 4, MALLOC_CAP_SPIRAM);
    }
    if (!MEMORY_mem) {
        ESP_LOGE(TAG, "Failed to allocate MEMORY_mem!");
        abort();
    }
    memset(MEMORY_mem, 0, 65536 + 4);

    /* XL/XE OS underlay buffers — also need byte access */
    under_atarixl_os = (UBYTE *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!under_atarixl_os) {
        ESP_LOGE(TAG, "Failed to allocate under_atarixl_os!");
        abort();
    }

    under_cart809F = (UBYTE *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    under_cartA0BF = (UBYTE *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!under_cart809F || !under_cartA0BF) {
        ESP_LOGE(TAG, "Failed to allocate cart underlay buffers!");
        abort();
    }

    /* Double-buffered framebuffers (320 * 240 indexed, converted by videoTask).
       Two buffers eliminate the race between emu_step writing the next frame
       on core 0 and videoTask reading the previous frame on core 1. */
    for (int i = 0; i < 2; i++) {
        framebuffer[i] = (uint8_t *)heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT,
                                                      MALLOC_CAP_SPIRAM);
        if (!framebuffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate display framebuffer[%d]!", i);
            abort();
        }
        memset(framebuffer[i], 0, DISPLAY_WIDTH * DISPLAY_HEIGHT);
    }

    ESP_LOGI(TAG, "Buffer allocation complete. Free heap: %d, free DMA: %d",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_DMA));
}

/* ===================================================================
 * Emulator init: parse file type, build command line, call libatari800
 * =================================================================== */

static const char *get_ext(const char *path)
{
    const char *dot = NULL;
    const char *p = path;
    while (*p) {
        if (*p == '.') dot = p + 1;
        p++;
    }
    return dot ? dot : "";
}

static int strcicmp_ext(const char *a, const char *b)
{
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static void emu_init(const char *filename)
{
    ESP_LOGI(TAG, "emu_init: %s", filename);

    atari800_preallocate();

    const char *ext = get_ext(filename);

    /* Build libatari800 command-line arguments.
       The command-line parser in atari.cpp processes these:
       -xl / -atari / -5200 : machine type
       -cart-type N -cart path : cartridge
       -boottape path : cassette
       -nobasic : disable BASIC ROM
       path : auto-detect via AFILE */
    const char *argv[16];
    int argc = 0;
    argv[argc++] = "atari800";

    /* Determine machine type and args from file extension and size */
    int cart_type = 0;

    if (strcicmp_ext(ext, "car") == 0) {
        /* .car files have a CART header — let libatari800 auto-detect */
        argv[argc++] = "-xl";
        argv[argc++] = "-nobasic";
        argv[argc++] = (char *)filename;
    }
    else if (strcicmp_ext(ext, "rom") == 0 || strcicmp_ext(ext, "bin") == 0) {
        /* Try to guess cart type from file size */
        FILE *fp = fopen(filename, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fclose(fp);

            if (len == 0x8000) {      /* 32KB → 5200 */
                argv[argc++] = "-5200";
                argv[argc++] = "-cart-type";
                argv[argc++] = "4";
                argv[argc++] = "-cart";
                argv[argc++] = (char *)filename;
            } else if (len == 0x4000) { /* 16KB → XL standard */
                argv[argc++] = "-atari";
                argv[argc++] = "-cart-type";
                argv[argc++] = "2";
                argv[argc++] = "-cart";
                argv[argc++] = (char *)filename;
            } else if (len == 0x2000) { /* 8KB → 800 standard */
                argv[argc++] = "-atari";
                argv[argc++] = "-cart-type";
                argv[argc++] = "1";
                argv[argc++] = "-cart";
                argv[argc++] = (char *)filename;
            } else {
                /* Unknown size — let auto-detect handle it */
                argv[argc++] = "-xl";
                argv[argc++] = "-nobasic";
                argv[argc++] = (char *)filename;
            }
        } else {
            argv[argc++] = "-xl";
            argv[argc++] = "-nobasic";
            argv[argc++] = (char *)filename;
        }
    }
    else if (strcicmp_ext(ext, "cas") == 0) {
        argv[argc++] = "-xl";
        argv[argc++] = "-boottape";
        argv[argc++] = (char *)filename;
    }
    else if (strcicmp_ext(ext, "atr") == 0) {
        /* Disk image — mount as drive 1, boot XL with no BASIC */
        argv[argc++] = "-xl";
        argv[argc++] = "-nobasic";
        argv[argc++] = (char *)filename;
    }
    else {
        /* .xex, .com, .bas or anything else — XL mode */
        argv[argc++] = "-xl";
        argv[argc++] = "-nobasic";
        argv[argc++] = (char *)filename;
    }

    /* TV standard: default NTSC for now */
    argv[argc++] = "-ntsc";

    /* Diagnostic: check if the ROM file exists and dump its header */
    {
        FILE *dbg_fp = fopen(filename, "rb");
        if (dbg_fp) {
            unsigned char hdr[8];
            size_t n = fread(hdr, 1, 8, dbg_fp);
            fseek(dbg_fp, 0, SEEK_END);
            long fsize = ftell(dbg_fp);
            fclose(dbg_fp);
            snprintf(diag_info, sizeof(diag_info),
                     "ROM OK sz=%ld hdr=%02X%02X%02X%02X%02X%02X%02X%02X",
                     fsize,
                     n>0?hdr[0]:0, n>1?hdr[1]:0, n>2?hdr[2]:0, n>3?hdr[3]:0,
                     n>4?hdr[4]:0, n>5?hdr[5]:0, n>6?hdr[6]:0, n>7?hdr[7]:0);
            ESP_LOGI(TAG, "%s", diag_info);
        } else {
            snprintf(diag_info, sizeof(diag_info), "ROM NOT FOUND errno=%d", errno);
            ESP_LOGE(TAG, "ROM file NOT FOUND: %s (errno=%d)", filename, errno);
        }
    }

    ESP_LOGI(TAG, "Calling libatari800_init with %d args", argc);
    for (int i = 0; i < argc; i++) {
        ESP_LOGI(TAG, "  argv[%d] = %s", i, argv[i]);
    }

    int result = libatari800_init(argc, (char **)argv);
    ESP_LOGI(TAG, "libatari800_init returned %d", result);
    ESP_LOGI(TAG, "TV mode: %s (%d scanlines/frame)",
             Atari800_tv_mode == 262 ? "NTSC" : "PAL", Atari800_tv_mode);

    /* Set audio frequency */
    Sound_desired.freq = AUDIO_SAMPLE_RATE;
}

/* ===================================================================
 * Frame step: read input, run one frame, produce audio + video
 * =================================================================== */

static void emu_step(odroid_gamepad_state *gamepad)
{
    static int frame_counter = 0;

    /* --- Read paddle potentiometer on GPIO36 (ADC1) --- */
    if (paddle_adc_enabled) {
        static int ema_adc = -1;  /* EMA state, -1 = uninitialized */

        /* Read paddle ADC from the gamepad task's atomic read (avoids
         * cross-core ADC1 SAR contention with joystick reads on core 1). */
        int paddle_adc = odroid_paddle_adc_raw;

        /* EMA smoothing: alpha ~0.2 using fixed-point (51/256).
         * Heavier smoothing to suppress ADC noise at rest. */
        if (ema_adc < 0) {
            ema_adc = paddle_adc;  /* seed on first frame */
        } else {
            ema_adc = (51 * paddle_adc + 205 * ema_adc + 128) >> 8;
        }

        /* Map EMA-filtered ADC range to POKEY POT range */
        int clamped = ema_adc;
        if (clamped < PADDLE_ADC_LO) clamped = PADDLE_ADC_LO;
        if (clamped > PADDLE_ADC_HI) clamped = PADDLE_ADC_HI;
        int target_pot = PADDLE_POT_MIN + ((clamped - PADDLE_ADC_LO) * (PADDLE_POT_MAX - PADDLE_POT_MIN)) / (PADDLE_ADC_HI - PADDLE_ADC_LO);
        if (target_pot < PADDLE_POT_MIN) target_pot = PADDLE_POT_MIN;
        if (target_pot > PADDLE_POT_MAX) target_pot = PADDLE_POT_MAX;

        /* Dead zone: only update if pot moved enough (kills stationary jitter) */
        int diff = target_pot - (int)AtariPot;
        if (diff < 0) diff = -diff;
        if (diff >= PADDLE_DEAD_ZONE) {
            AtariPot = (unsigned char)target_pot;
        }
    } else {
        /* Software paddle: D-pad left/right adjusts AtariPot with acceleration */
        static int paddle_hold_frames = 0;
        int dir = 0;
        if (gamepad->values[ODROID_INPUT_LEFT])  dir = -1;
        if (gamepad->values[ODROID_INPUT_RIGHT]) dir =  1;

        if (dir != 0) {
            paddle_hold_frames++;
            int step;
            if (paddle_hold_frames >= PADDLE_ACCEL_FAST)
                step = PADDLE_STEP_FAST;
            else if (paddle_hold_frames >= PADDLE_ACCEL_MED)
                step = PADDLE_STEP_MED;
            else
                step = PADDLE_STEP_SLOW;

            int pot = (int)AtariPot + dir * step;
            if (pot < PADDLE_POT_MIN) pot = PADDLE_POT_MIN;
            if (pot > PADDLE_POT_MAX) pot = PADDLE_POT_MAX;
            AtariPot = (unsigned char)pot;
        } else {
            paddle_hold_frames = 0;
        }
    }

    /* --- Map Odroid Go buttons to Atari joystick/console --- */
    _joy[0] = 0;
    _trig[0] = 0;

    if (gamepad->values[ODROID_INPUT_UP])    _joy[0] |= JOY_STICK_FORWARD;
    if (gamepad->values[ODROID_INPUT_DOWN])  _joy[0] |= JOY_STICK_BACK;
    if (gamepad->values[ODROID_INPUT_LEFT])  _joy[0] |= JOY_STICK_LEFT;
    if (gamepad->values[ODROID_INPUT_RIGHT]) _joy[0] |= JOY_STICK_RIGHT;

    /* A button = Fire (trigger) */
    if (gamepad->values[ODROID_INPUT_A])     _trig[0] = 1;

    /* B button = second action — map to fire as well for most games */
    if (gamepad->values[ODROID_INPUT_B])     _trig[0] = 1;

    /* Console keys */
    console_keys = 0x07; /* all released */
    if (gamepad->values[ODROID_INPUT_START])
        console_keys &= ~0x01; /* START pressed */
    if (gamepad->values[ODROID_INPUT_SELECT])
        console_keys &= ~0x02; /* SELECT pressed */

    INPUT_key_consol = console_keys;

    /* 5200 special handling */
    if (Atari800_machine_type == Atari800_MACHINE_5200) {
        INPUT_key_code = AKEY_NONE;
        if (gamepad->values[ODROID_INPUT_START])
            INPUT_key_code = AKEY_5200_START;
    } else {
        INPUT_key_code = AKEY_NONE;
    }

    /* --- Frame skip: ANTIC always renders (avoids state divergence in the
       non-draw path), but we only push to LCD every other frame. --- */
    atari800_draw_frame = 1;           /* ANTIC always renders pixels */
    int push_this_frame = (frame_counter & 1) == 0;
    frame_counter++;

    /* --- Run one emulator frame --- */
    libatari800_next_frame(NULL);

    /* --- Video: copy every frame (Screen_atari is always fresh), but
           only push to LCD on display frames to avoid blocking on SPI. --- */
    if (push_this_frame) {
        /* Extract video: crop 384→320 from Screen_atari into back buffer */
        uint8_t *src = (uint8_t *)Screen_atari;
        uint8_t *dst = framebuffer[fb_index];
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            memcpy(dst, src + SRC_X_START, DISPLAY_WIDTH);
            dst += DISPLAY_WIDTH;
            src += ATARI_SCREEN_WIDTH;
        }

        /* Submit back buffer to video queue, then swap */
        uint8_t *fb_ptr = framebuffer[fb_index];
        xQueueSend(vidQueue, &fb_ptr, portMAX_DELAY);
        fb_index ^= 1;
    }

    /* --- Audio (every frame for smooth sound) --- */
    /* At ~52 FPS we need 15720/52 ≈ 302 samples per frame to keep the I2S
       DMA fed.  Using 60 (= 262 samples) causes underruns → artifacts.
       The I2S portMAX_DELAY backpressure naturally paces us to ~52 FPS. */
    int samples_per_frame = 302;

    if (!sampleBuffer) {
        sampleBuffer = (int16_t *)heap_caps_malloc(samples_per_frame * 2 * sizeof(int16_t),
                                                    MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!sampleBuffer) {
            sampleBuffer = (int16_t *)heap_caps_malloc(samples_per_frame * 2 * sizeof(int16_t),
                                                        MALLOC_CAP_SPIRAM);
        }
        if (!sampleBuffer) {
            ESP_LOGE(TAG, "Failed to allocate audio sample buffer!");
            return;
        }
    }

    /* Get 8-bit unsigned samples from libatari800 */
    uint8_t audio8[512];
    Sound_Callback(audio8, samples_per_frame);

    /* Convert 8-bit unsigned → 16-bit signed stereo for I2S */
    for (int i = 0; i < samples_per_frame; i++) {
        int16_t s16 = ((int)audio8[i] - 128) << 8;
        sampleBuffer[i * 2]     = s16; /* left */
        sampleBuffer[i * 2 + 1] = s16; /* right */
    }

    odroid_audio_submit(sampleBuffer, samples_per_frame);
}

/* ===================================================================
 * In-game menu: Continue / Restart / Quit
 * =================================================================== */

/* Minimal 5x7 bitmap font for menu text (ASCII 32-122) */
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, /* 32 space */
    0x04,0x04,0x04,0x00,0x04, /* 33 ! */
    0x0A,0x0A,0x00,0x00,0x00, /* 34 " */
    0x0A,0x1F,0x0A,0x1F,0x0A, /* 35 # */
    0x04,0x0F,0x06,0x1E,0x04, /* 36 $ */
    0x19,0x1A,0x04,0x0B,0x13, /* 37 % */
    0x06,0x09,0x16,0x09,0x16, /* 38 & */
    0x04,0x04,0x00,0x00,0x00, /* 39 ' */
    0x08,0x04,0x04,0x04,0x08, /* 40 ( */
    0x02,0x04,0x04,0x04,0x02, /* 41 ) */
    0x04,0x15,0x0E,0x15,0x04, /* 42 * */
    0x00,0x04,0x0E,0x04,0x00, /* 43 + */
    0x00,0x00,0x00,0x04,0x02, /* 44 , */
    0x00,0x00,0x0E,0x00,0x00, /* 45 - */
    0x00,0x00,0x00,0x00,0x04, /* 46 . */
    0x10,0x08,0x04,0x02,0x01, /* 47 / */
    0x0E,0x19,0x15,0x13,0x0E, /* 48 0 */
    0x04,0x06,0x04,0x04,0x0E, /* 49 1 */
    0x0E,0x11,0x0C,0x02,0x1F, /* 50 2 */
    0x0E,0x11,0x0C,0x11,0x0E, /* 51 3 */
    0x08,0x0C,0x0A,0x1F,0x08, /* 52 4 */
    0x1F,0x01,0x0F,0x10,0x0F, /* 53 5 */
    0x0E,0x01,0x0F,0x11,0x0E, /* 54 6 */
    0x1F,0x10,0x08,0x04,0x04, /* 55 7 */
    0x0E,0x11,0x0E,0x11,0x0E, /* 56 8 */
    0x0E,0x11,0x1E,0x10,0x0E, /* 57 9 */
    0x00,0x04,0x00,0x04,0x00, /* 58 : */
    0x00,0x04,0x00,0x04,0x02, /* 59 ; */
    0x08,0x04,0x02,0x04,0x08, /* 60 < */
    0x00,0x0E,0x00,0x0E,0x00, /* 61 = */
    0x02,0x04,0x08,0x04,0x02, /* 62 > */
    0x0E,0x11,0x08,0x00,0x04, /* 63 ? */
    0x0E,0x11,0x1D,0x01,0x0E, /* 64 @ */
    0x0E,0x11,0x1F,0x11,0x11, /* 65 A */
    0x0F,0x11,0x0F,0x11,0x0F, /* 66 B */
    0x0E,0x11,0x01,0x11,0x0E, /* 67 C */
    0x07,0x09,0x11,0x09,0x07, /* 68 D */
    0x1F,0x01,0x0F,0x01,0x1F, /* 69 E */
    0x1F,0x01,0x0F,0x01,0x01, /* 70 F */
    0x0E,0x01,0x19,0x11,0x0E, /* 71 G */
    0x11,0x11,0x1F,0x11,0x11, /* 72 H */
    0x0E,0x04,0x04,0x04,0x0E, /* 73 I */
    0x1C,0x08,0x08,0x09,0x06, /* 74 J */
    0x11,0x09,0x07,0x09,0x11, /* 75 K */
    0x01,0x01,0x01,0x01,0x1F, /* 76 L */
    0x11,0x1B,0x15,0x11,0x11, /* 77 M */
    0x11,0x13,0x15,0x19,0x11, /* 78 N */
    0x0E,0x11,0x11,0x11,0x0E, /* 79 O */
    0x0F,0x11,0x0F,0x01,0x01, /* 80 P */
    0x0E,0x11,0x15,0x09,0x16, /* 81 Q */
    0x0F,0x11,0x0F,0x09,0x11, /* 82 R */
    0x0E,0x01,0x0E,0x10,0x0F, /* 83 S */
    0x1F,0x04,0x04,0x04,0x04, /* 84 T */
    0x11,0x11,0x11,0x11,0x0E, /* 85 U */
    0x11,0x11,0x0A,0x0A,0x04, /* 86 V */
    0x11,0x11,0x15,0x1B,0x11, /* 87 W */
    0x11,0x0A,0x04,0x0A,0x11, /* 88 X */
    0x11,0x0A,0x04,0x04,0x04, /* 89 Y */
    0x1F,0x08,0x04,0x02,0x1F, /* 90 Z */
    0x0E,0x02,0x02,0x02,0x0E, /* 91 [ */
    0x01,0x02,0x04,0x08,0x10, /* 92 \ */
    0x0E,0x08,0x08,0x08,0x0E, /* 93 ] */
    0x04,0x0A,0x11,0x00,0x00, /* 94 ^ */
    0x00,0x00,0x00,0x00,0x1F, /* 95 _ */
    0x02,0x04,0x00,0x00,0x00, /* 96 ` */
    0x00,0x0E,0x12,0x12,0x1C, /* 97 a */
    0x01,0x0F,0x11,0x11,0x0F, /* 98 b */
    0x00,0x0E,0x01,0x01,0x0E, /* 99 c */
    0x10,0x1E,0x11,0x11,0x1E, /* 100 d */
    0x00,0x0E,0x1F,0x01,0x0E, /* 101 e */
    0x0C,0x02,0x07,0x02,0x02, /* 102 f */
    0x00,0x1E,0x11,0x1E,0x10, /* 103 g */
    0x01,0x0F,0x11,0x11,0x11, /* 104 h */
    0x04,0x00,0x04,0x04,0x04, /* 105 i */
    0x08,0x00,0x08,0x08,0x06, /* 106 j */
    0x01,0x09,0x07,0x09,0x11, /* 107 k */
    0x06,0x04,0x04,0x04,0x0E, /* 108 l */
    0x00,0x0B,0x15,0x15,0x11, /* 109 m */
    0x00,0x0F,0x11,0x11,0x11, /* 110 n */
    0x00,0x0E,0x11,0x11,0x0E, /* 111 o */
    0x00,0x0F,0x11,0x0F,0x01, /* 112 p */
    0x00,0x1E,0x11,0x1E,0x10, /* 113 q */
    0x00,0x0D,0x13,0x01,0x01, /* 114 r */
    0x00,0x0E,0x06,0x18,0x0E, /* 115 s */
    0x02,0x07,0x02,0x02,0x0C, /* 116 t */
    0x00,0x11,0x11,0x11,0x1E, /* 117 u */
    0x00,0x11,0x11,0x0A,0x04, /* 118 v */
    0x00,0x11,0x15,0x15,0x0A, /* 119 w */
    0x00,0x11,0x0A,0x0A,0x11, /* 120 x */
    0x00,0x11,0x1E,0x10,0x0E, /* 121 y */
    0x00,0x1F,0x08,0x04,0x1F, /* 122 z */
};

static void menu_draw_char(uint16_t *fb, int cx, int cy, char ch, uint16_t color, int scale)
{
    if (ch < 32 || ch > 122) return;
    const uint8_t *glyph = &font5x7[(ch - 32) * 5];
    for (int row = 0; row < 5; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = cx + col * scale + sx;
                        int py = cy + row * scale + sy;
                        if (px >= 0 && px < 320 && py >= 0 && py < 240)
                            fb[py * 320 + px] = color;
                    }
                }
            }
        }
    }
}

static void menu_draw_string(uint16_t *fb, int x, int y, const char *str, uint16_t color, int scale)
{
    while (*str) {
        menu_draw_char(fb, x, y, *str, color, scale);
        x += 6 * scale;
        str++;
    }
}

static int menu_string_width(const char *str, int scale)
{
    int len = 0;
    while (*str++) len++;
    return len * 6 * scale;
}

/* ===================================================================
 * Save / Load state helpers
 * =================================================================== */

static void ensure_save_dir(void)
{
    /* Create /sd/odroid/data/a800/ hierarchy if needed */
    struct stat st;
    const char *dirs[] = {
        "/sd/odroid",
        "/sd/odroid/data",
        "/sd/odroid/data/a800"
    };
    for (int i = 0; i < 3; i++) {
        if (stat(dirs[i], &st) != 0) {
            mkdir(dirs[i], 0777);
        }
    }
}

static void build_save_path(const char *romfile)
{
    /* Extract filename from full path */
    const char *name = romfile;
    const char *p = romfile;
    while (*p) {
        if (*p == '/' || *p == '\\') name = p + 1;
        p++;
    }
    snprintf(save_path, sizeof(save_path), "/sd/odroid/data/a800/%s.sav", name);
    ESP_LOGI(TAG, "Save path: %s", save_path);
}

static bool SaveState(void)
{
    if (!statesav_buffer || save_path[0] == '\0') return false;

    ensure_save_dir();

    /* Serialize entire machine state into RAM buffer */
    memset(statesav_buffer, 0, STATESAV_MAX_SIZE);
    LIBATARI800_StateSave(statesav_buffer, &statesav_tags);
    ULONG used = StateSav_Tell();
    ESP_LOGI(TAG, "State serialized: %lu bytes", (unsigned long)used);

    /* Write buffer to SD */
    FILE *f = fopen(save_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", save_path);
        return false;
    }
    size_t written = fwrite(statesav_buffer, 1, used, f);
    fclose(f);

    if (written != used) {
        ESP_LOGE(TAG, "Save write incomplete: %u / %lu", written, (unsigned long)used);
        return false;
    }
    ESP_LOGI(TAG, "State saved OK (%lu bytes)", (unsigned long)used);

    /* Mark DataSlot=1 so next boot auto-loads */
    odroid_settings_DataSlot_set(1);

    return true;
}

static bool LoadState(void)
{
    if (!statesav_buffer || save_path[0] == '\0') return false;

    struct stat st;
    if (stat(save_path, &st) != 0) {
        ESP_LOGI(TAG, "No save file found: %s", save_path);
        return false;
    }
    if (st.st_size > STATESAV_MAX_SIZE || st.st_size < 16) {
        ESP_LOGW(TAG, "Save file invalid size: %ld", (long)st.st_size);
        return false;
    }

    FILE *f = fopen(save_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for reading", save_path);
        return false;
    }
    size_t rd = fread(statesav_buffer, 1, st.st_size, f);
    fclose(f);

    if ((off_t)rd != st.st_size) {
        ESP_LOGE(TAG, "Save read incomplete: %u / %ld", rd, (long)st.st_size);
        return false;
    }

    /* Deserialize into emulator */
    LIBATARI800_StateLoad(statesav_buffer);
    ESP_LOGI(TAG, "State loaded OK (%ld bytes)", (long)st.st_size);
    return true;
}

static int show_game_menu(void)
{
    uint16_t *menu_fb = (uint16_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!menu_fb) return 0;

    const char *options[] = { "Continue", "Save & Continue", "Load State", "Save & Quit", "Restart Game", "Quit to Menu" };
    int count = 6;
    int selected = 0;

    odroid_gamepad_state prev;
    odroid_input_gamepad_read(&prev);

    while (1) {
        /* Dark background */
        memset(menu_fb, 0, 320 * 240 * 2);

        /* Box dimensions */
        int bx0 = 50, bx1 = 270, by0 = 30, by1 = 216;

        /* Fill box interior (dark blue) */
        for (int y = by0 + 1; y < by1; y++)
            for (int x = bx0 + 1; x < bx1; x++)
                menu_fb[y * 320 + x] = 0x0010;

        /* White border (top, bottom, left, right) */
        for (int x = bx0; x <= bx1; x++) {
            menu_fb[by0 * 320 + x] = 0xFFFF;
            menu_fb[by1 * 320 + x] = 0xFFFF;
        }
        for (int y = by0; y <= by1; y++) {
            menu_fb[y * 320 + bx0] = 0xFFFF;
            menu_fb[y * 320 + bx1] = 0xFFFF;
        }

        /* Title text (scale 2) */
        const char *title = "ATARI 800";
        int tw = menu_string_width(title, 2);
        menu_draw_string(menu_fb, (320 - tw) / 2, by0 + 8, title, 0xFFFF, 2);

        /* Separator line under title */
        int sep_y = by0 + 26;
        for (int x = bx0 + 8; x <= bx1 - 8; x++) {
            menu_fb[sep_y * 320 + x] = 0xFFFF;
        }

        /* Options (scale 2) */
        int opt_y0 = sep_y + 12;
        int opt_spacing = 26;
        for (int i = 0; i < count; i++) {
            int y_pos = opt_y0 + i * opt_spacing;
            if (i == selected) {
                for (int y = y_pos - 3; y <= y_pos + 13; y++)
                    for (int x = bx0 + 4; x <= bx1 - 4; x++)
                        menu_fb[y * 320 + x] = 0xFFE0; /* yellow highlight */
            }
            uint16_t text_color = (i == selected) ? 0x0000 : 0xFFFF;
            int ow = menu_string_width(options[i], 2);
            menu_draw_string(menu_fb, (320 - ow) / 2, y_pos, options[i], text_color, 2);
        }

        ili9341_write_frame_rectangleLE(0, 0, 320, 240, menu_fb);

        odroid_gamepad_state state;
        odroid_input_gamepad_read(&state);

        if (!prev.values[ODROID_INPUT_DOWN] && state.values[ODROID_INPUT_DOWN])
            selected = (selected + 1) % count;
        if (!prev.values[ODROID_INPUT_UP] && state.values[ODROID_INPUT_UP])
            selected = (selected - 1 + count) % count;
        if (!prev.values[ODROID_INPUT_A] && state.values[ODROID_INPUT_A])
            break;
        if (!prev.values[ODROID_INPUT_B] && state.values[ODROID_INPUT_B]) {
            selected = 0;
            break;
        }
        if (!prev.values[ODROID_INPUT_MENU] && state.values[ODROID_INPUT_MENU]) {
            selected = 0;
            break;
        }

        prev = state;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Wait for key release */
    odroid_gamepad_state rel;
    int timeout = 100;
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        odroid_input_gamepad_read(&rel);
        if (--timeout <= 0) break;
    } while (rel.values[ODROID_INPUT_A] || rel.values[ODROID_INPUT_B] ||
             rel.values[ODROID_INPUT_MENU]);

    free(menu_fb);
    return selected;
}

/* ===================================================================
 * app_main — entry point
 * =================================================================== */

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "atari800-go started.");
    ESP_LOGI(TAG, "HEAP: 0x%x, DMA: 0x%x",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_DMA));

    nvs_flash_init();
    odroid_system_init();
    odroid_input_gamepad_init();
    odroid_input_battery_level_init();

    ili9341_prepare();
    ili9341_init();
    ili9341_clear(0x0000);

    /* Safe mode: hold A during boot to bail out to launcher */
    {
        odroid_gamepad_state bail;
        odroid_input_gamepad_read(&bail);
        if (bail.values[ODROID_INPUT_A]) {
            ESP_LOGW(TAG, "Button A held at boot — returning to launcher");
            vTaskDelay(pdMS_TO_TICKS(200));
            odroid_system_application_set(0);
            esp_restart();
        }
    }

    /* Open SD card */
    esp_err_t r = odroid_sdcard_open(SD_BASE_PATH);
    if (r != ESP_OK) {
        odroid_display_show_sderr(ODROID_SD_ERR_NOCARD);
        abort();
    }

    /* Get ROM path from NVS (set by launcher) */
    const char *romfile = odroid_settings_RomFilePath_get();
    if (!romfile || strlen(romfile) < 4) {
        ESP_LOGW(TAG, "No ROM path in NVS — using hardcoded test ROM.");
        romfile = "/sd/roms/a800/test.xex";
    }
    ESP_LOGI(TAG, "ROM: %s", romfile);

    ili9341_clear(0x0000);

    /* Build palette */
    build_rgb565_palette();

    /* Initialize emulator */
    emu_init(romfile);

    /* Allocate save state buffer in SPIRAM */
    statesav_buffer = (UBYTE *)heap_caps_malloc(STATESAV_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (statesav_buffer) {
        memset(statesav_buffer, 0, STATESAV_MAX_SIZE);
        build_save_path(romfile);
        ESP_LOGI(TAG, "Save state buffer: %d bytes in SPIRAM", STATESAV_MAX_SIZE);
    } else {
        ESP_LOGW(TAG, "Failed to allocate save state buffer — save disabled");
    }

    /* Auto-load saved state if DataSlot==1 (set by Save or launcher Resume) */
    int32_t startAction = odroid_settings_DataSlot_get();
    ESP_LOGI(TAG, "DataSlot (startAction): %d", startAction);
    if (startAction == 1 && statesav_buffer && save_path[0] != '\0') {
        if (LoadState()) {
            ESP_LOGI(TAG, "Resumed from saved state");
        }
    }
    /* Clear DataSlot so a crash/power-cycle won't keep resuming */
    odroid_settings_DataSlot_set(0);

    /* Paddle potentiometer ADC on GPIO15 (ADC2_CH3) — detection.
     * A real pot gives stable readings at any position (even 0).
     * A floating pin reads erratically. Take 4 samples and check max spread. */
    adc2_config_channel_atten(PADDLE_ADC_CHANNEL, ADC_ATTEN_11db);
    {
        const int N = 4;
        int vals[4];
        int lo = 4095, hi = 0;
        for (int i = 0; i < N; i++) {
            int v = 0;
            if (adc2_get_raw(PADDLE_ADC_CHANNEL, ADC_WIDTH_12Bit, &v) == ESP_OK) {
                vals[i] = v;
            } else {
                vals[i] = -1;  /* read failed */
            }
            if (vals[i] >= 0 && vals[i] < lo) lo = vals[i];
            if (vals[i] > hi) hi = vals[i];
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        int spread = hi - lo;
        printf("[PADDLE-DBG] detect: vals=%d,%d,%d,%d  lo=%d hi=%d spread=%d\n",
               vals[0], vals[1], vals[2], vals[3], lo, hi, spread);
        if (spread < PADDLE_DETECT_SPREAD) {
            paddle_adc_enabled = true;
            printf("[PADDLE-DBG] => ENABLED (ADC2/GPIO15)\n");
        } else {
            paddle_adc_enabled = false;
            printf("[PADDLE-DBG] => DISABLED (floating — spread=%d)\n", spread);
        }
    }

    /* Audio */
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* Video queue and task */
    vidQueue = xQueueCreate(1, sizeof(uint8_t *));
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024 * 4, NULL, 5, NULL, 1);

    /* Main emulation loop on core 0 */
    uint32_t startTime;
    uint32_t stopTime;
    uint32_t totalElapsedTime = 0;
    int frame = 0;

    odroid_gamepad_state last_gamepad;
    odroid_input_gamepad_read(&last_gamepad);

    while (1) {
        startTime = xthal_get_ccount();

        odroid_gamepad_state gamepad;
        odroid_input_gamepad_read(&gamepad);

        /* Menu button handler */
        if (last_gamepad.values[ODROID_INPUT_MENU] &&
            !gamepad.values[ODROID_INPUT_MENU])
        {
            /* Drain video queue */
            uint8_t *dummy;
            while (xQueueReceive(vidQueue, &dummy, 0) == pdTRUE) {}
            vTaskDelay(50 / portTICK_PERIOD_MS);

            int choice = show_game_menu();
            if (choice == 1) {
                /* Save & Continue */
                SaveState();
            } else if (choice == 2) {
                /* Load State */
                LoadState();
            } else if (choice == 3) {
                /* Save & Quit */
                SaveState();
                odroid_system_application_set(0);
                esp_restart();
            } else if (choice == 4) {
                /* Restart game — clear DataSlot so we don't auto-load */
                odroid_settings_DataSlot_set(0);
                esp_restart();
            } else if (choice == 5) {
                odroid_system_application_set(0);
                esp_restart(); /* Quit to launcher */
            }
            /* choice 0 = Continue */
        }

        /* Volume button */
        if (!last_gamepad.values[ODROID_INPUT_VOLUME] &&
            gamepad.values[ODROID_INPUT_VOLUME])
        {
            odroid_audio_volume_change();
            ESP_LOGI(TAG, "Volume: %d", odroid_audio_volume_get());
        }

        /* Run one frame */
        emu_step(&gamepad);

        last_gamepad = gamepad;

        /* Performance stats every 60 frames */
        stopTime = xthal_get_ccount();

        uint32_t elapsedTime;
        if (stopTime > startTime)
            elapsedTime = stopTime - startTime;
        else
            elapsedTime = ((uint64_t)stopTime + 0xFFFFFFFF) - startTime;

        totalElapsedTime += elapsedTime;
        frame++;

        if (frame == 60) {
            float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f);
            float fps = frame / seconds;

            ESP_LOGI(TAG, "HEAP:0x%x DMA:0x%x FPS:%.1f",
                     esp_get_free_heap_size(),
                     heap_caps_get_free_size(MALLOC_CAP_DMA),
                     fps);

            /* Print diagnostic info first 3 times so serial monitor can catch it */
            if (diag_printed < 3) {
                ESP_LOGI(TAG, "DIAG: %s", diag_info);
                diag_printed++;
            }

            frame = 0;
            totalElapsedTime = 0;
        }
    }
}
