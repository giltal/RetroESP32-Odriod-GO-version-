#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_ota_ops.h"

#include "pce.h"

extern char *rom_file_name;
extern uchar *SPM_raw;
extern uchar *SPM;
extern char *syscard_filename;

/////////

#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_sdcard.h"
#include "../components/odroid/odroid_ui.h"
#include "../components/odroid/odroid_ui_choosefile.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NOINLINE  __attribute__ ((noinline))

/* Flag: load save state after ResetPCE() in RunPCE() */
bool pce_should_resume = false;

const char* SD_BASE_PATH = "/sd";
#define PATH_MAX_MY 128

#define TASK_BREAK (void*)1
uchar* osd_gfx_buffer = NULL;
uchar* XBuf;
#ifdef MY_GFX_AS_TASK
QueueHandle_t vidQueue;
TaskHandle_t videoTaskHandle;
volatile bool videoTaskIsRunning = false;
#endif

#ifdef MY_SND_AS_TASK
QueueHandle_t audioQueue;
TaskHandle_t audioTaskHandle;
volatile bool audioTaskIsRunning = false;
#endif
uint8_t* framebuffer[2];
uint16_t* my_palette;

void dump_heap_info_short() {
    printf("LARGEST: 8BIT: %u\n", heap_caps_get_largest_free_block( MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT ));
    printf("LARGEST: 32BIT: %u\n", heap_caps_get_largest_free_block( MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT ));
    printf("LARGEST: MALLOC_CAP_INTERNAL: %u\n", heap_caps_get_largest_free_block( MALLOC_CAP_INTERNAL ));
    printf("LARGEST: MALLOC_CAP_DEFAULT: %u\n", heap_caps_get_largest_free_block( MALLOC_CAP_DEFAULT ));
}

void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size) {
    uint32_t caps = (speed?MALLOC_CAP_INTERNAL:MALLOC_CAP_SPIRAM) | 
      ( bytes==1?MALLOC_CAP_8BIT:MALLOC_CAP_32BIT);
      /*
    if (speed) {
        uint32_t max = heap_caps_get_largest_free_block(caps);
        if (max < size) {
            printf("ALLOC: Size: %u; Max FREE for internal is '%u'. Allocating in SPI RAM\n", (unsigned int)size, max);
            caps = MALLOC_CAP_SPIRAM | ( size==1?MALLOC_CAP_8BIT:MALLOC_CAP_32BIT);
        }
    } else {
      caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT;
    }
    */
    if (!speed) caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT;
    //if (!speed || size!=65536) caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT; // only RAM
    void *rc = heap_caps_malloc(size, caps);
    printf("ALLOC: Size: %-10u; SPI: %u; 32BIT: %u; RC: %p\n", (unsigned int)size, (caps&MALLOC_CAP_SPIRAM)!=0, (caps&MALLOC_CAP_32BIT)!=0, rc);
    if (!rc) { dump_heap_info_short(); abort(); }
    return rc;
}

#ifdef MY_GFX_AS_TASK

#ifdef MY_VIDEO_MODE_SCANLINES

#define VID_TASK(func) \
    struct my_scanline param; \
    videoTaskIsRunning = true; \
    printf("%s: STARTED\n", __func__); \
     \
    while(1) \
    { \
        xQueuePeek(vidQueue, &param, portMAX_DELAY); \
 \
        if (param.buffer == TASK_BREAK) \
            break; \
 \
        odroid_display_lock(); \
        func(&param, my_palette); \
        odroid_display_unlock(); \
        /* odroid_input_battery_level_read(&battery);*/ \
        xQueueReceive(vidQueue, &param, portMAX_DELAY); \
    } \
    xQueueReceive(vidQueue, &param, portMAX_DELAY); \
    /*odroid_display_lock();*/ \
    /*odroid_display_show_hourglass();*/ \
    /*odroid_display_unlock();*/ \
    videoTaskIsRunning = false; \
    printf("%s: FINISHED\n", __func__); \
    vTaskDelete(NULL); \
    while (1) {}


void videoTask_mode0(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_scanlines) }
void videoTask_mode0_w224(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_scanlines_w224) }
void videoTask_mode0_w256(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_scanlines_w256) }
void videoTask_mode0_w320(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_scanlines_w320) }
void videoTask_mode0_w336(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_scanlines_w336) }

#else


#define VID_TASK(func) \
    uint8_t* param; \
    videoTaskIsRunning = true; \
    printf("%s: STARTED\n", __func__); \
     \
    while(1) \
    { \
        xQueuePeek(vidQueue, &param, portMAX_DELAY); \
 \
        if (param == TASK_BREAK) \
            break; \
 \
        odroid_display_lock(); \
        func(param, my_palette); \
        odroid_display_unlock(); \
        /* odroid_input_battery_level_read(&battery);*/ \
        xQueueReceive(vidQueue, &param, portMAX_DELAY); \
    } \
    xQueueReceive(vidQueue, &param, portMAX_DELAY); \
    /*odroid_display_lock();*/ \
    /*odroid_display_show_hourglass();*/ \
    /*odroid_display_unlock();*/ \
    videoTaskIsRunning = false; \
    printf("%s: FINISHED\n", __func__); \
    vTaskDelete(NULL); \
    while (1) {}


void videoTask_mode0(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0) }
void videoTask_mode0_w224(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_w224) }
void videoTask_mode0_w256(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_w256) }
void videoTask_mode0_w320(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_w320) }
void videoTask_mode0_w336(void *arg) { VID_TASK(ili9341_write_frame_pcengine_mode0_w336) }
#endif

#ifdef MY_SND_AS_TASK
#define AUDIO_SAMPLE_RATE (22050)
#define AUDIO_BUFFER_SIZE (4096) 
//(1920*4)
#define AUDIO_CHANNELS 6
short *sbuf_mix[2];
char *sbuf[AUDIO_CHANNELS];
void audioTask_mode0(void *arg) {
    uint8_t* param;
    audioTaskIsRunning = true;
    printf("%s: STARTED\n", __func__);
    uint8_t buf = 0;
    
    while(1)
    {
        if (xQueuePeek(audioQueue, &param, 0) == pdTRUE)
        {
            if (param == TASK_BREAK)
                break;
            xQueueReceive(audioQueue, &param, portMAX_DELAY);
        }
        //memset(sbuf[buf], 0, AUDIO_BUFFER_SIZE);
        if (odroid_ui_menu_opened)
        {
            usleep(100*1000);
            continue;
        }
        usleep(10*1000);
        char *sbufp[AUDIO_CHANNELS];
        for (int i = 0;i < AUDIO_CHANNELS;i++)
        {
          sbufp[i] = sbuf[i];
          WriteBuffer((char*)sbuf[i], i, AUDIO_BUFFER_SIZE/2);
        }
        /*
        uchar lvol, rvol;
        lvol = (io.psg_volume >> 4) * 1.22;
        rvol = (io.psg_volume & 0x0F) * 1.22;
        
        short *p = sbuf_mix[buf];
        for (int i = 0;i < AUDIO_BUFFER_SIZE/2;i++)
        {
             short lval = 0;
             short rval = 0;
            for (int j = 0;j< AUDIO_CHANNELS;j++)
            {
                lval+=( short)(*sbufp[j]);
                rval+=( short)(*(sbufp[j]+1));
                sbufp[j]++;
            }
            lval = lval * lvol;
            rval = rval * rvol;
            *p = lval;
            *(p+1) = rval;
            p+=2;
        }
        */
        uchar lvol, rvol;
        lvol = (io.psg_volume >> 4) * 1.22;
        rvol = (io.psg_volume & 0x0F) * 1.22;
        
        short *p = sbuf_mix[buf];
        for (int i = 0;i < AUDIO_BUFFER_SIZE/2;i++)
        {
             short lval = 0;
             short rval = 0;
            for (int j = 0;j< AUDIO_CHANNELS;j++)
            {
                lval+=( short)sbufp[j][i];
                rval+=( short)sbufp[j][i+1];
            }
            //lval = lval/AUDIO_CHANNELS;
            //rval = rval/AUDIO_CHANNELS;
            lval = lval * lvol;
            rval = rval * rvol;
            *p = lval;
            *(p+1) = rval;
            p+=2;
        }
        
        odroid_audio_submit((short*)sbuf_mix[buf], AUDIO_BUFFER_SIZE/4);
        buf = buf?0:1;
    }
    xQueueReceive(audioQueue, &param, portMAX_DELAY);
    audioTaskIsRunning = false;
    printf("%s: FINISHED\n", __func__);
    vTaskDelete(NULL);
    while (1) {}
}
#endif

void StopVideo()
{
#ifdef MY_VIDEO_MODE_SCANLINES
    struct my_scanline paramVideo;
    paramVideo.buffer = TASK_BREAK;
#else
    uint8_t* paramVideo = TASK_BREAK;
#endif
#ifdef MY_GFX_AS_TASK
    xQueueSend(vidQueue, &paramVideo, portMAX_DELAY);
    while (videoTaskIsRunning) { vTaskDelay(1); }
#endif
}

int old_width = -1;

NOINLINE void update_display_task(int width)
{
    if (width == old_width) return;
    old_width = width;

    printf("VIDEO: Task: Start\n");
    if (videoTaskIsRunning)
    {
        printf("VIDEO: Task: Stop\n");
        StopVideo();
        printf("VIDEO: Task: Stop done\n");
        
        printf("VIDEO: Clear display\n");
        //odroid_display_lock();
        ili9341_clear(0);
        //odroid_display_unlock();
    }
    
    TaskFunction_t taskFunc;
    if (width == 224)
        taskFunc = &videoTask_mode0_w224;
    else if (width == 256)
        taskFunc = &videoTask_mode0_w256;
    else if (width == 320)
        taskFunc = &videoTask_mode0_w320;
    else if (width == 336)
        taskFunc = &videoTask_mode0_w336;
    else
        taskFunc = &videoTask_mode0;
    xTaskCreatePinnedToCore(taskFunc, "videoTask", 1024 * 4, NULL, 5, &videoTaskHandle, 1);
    while (!videoTaskIsRunning) { vTaskDelay(1); }
    printf("VIDEO: Task: Start done\n");
}

#endif

void EmuAudio(bool enable)
{
    uint8_t* paramAudio = TASK_BREAK;
    if (enable == audioTaskIsRunning) return;
    if (enable)
    {
        xTaskCreatePinnedToCore(&audioTask_mode0, "audioTask", 1024 * 4, NULL, 5, &audioTaskHandle, 1);
        while (!audioTaskIsRunning) { vTaskDelay(1); }
    }
    else
    {
        xQueueSend(audioQueue, &paramAudio, portMAX_DELAY);
        while (audioTaskIsRunning) { vTaskDelay(1); }
    }
}

void DoMenuHome(bool save)
{
    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");
#ifdef MY_SND_AS_TASK
    EmuAudio(false);
    odroid_audio_terminate();
#endif

    // Stop tasks
    printf("PowerDown: stopping tasks.\n");
    StopVideo();
    //DoReboot(save);
    DoReboot(false);
}


NOINLINE void app_init(void)
{
    printf("pcengine (%s-%s).\n", COMPILEDATE, GITREV);
    
    nvs_flash_init();

    odroid_system_init();

    ili9341_init();
    ili9341_clear(0);
    odroid_audio_init(odroid_settings_AudioSink_get(), AUDIO_SAMPLE_RATE);

    // Joystick.
    odroid_input_gamepad_init();
    odroid_input_battery_level_init();

    // Safe mode: hold A during boot to bail out to launcher
    {
        odroid_gamepad_state bail;
        odroid_input_gamepad_read(&bail);
        if (bail.values[ODROID_INPUT_A]) {
            printf("pcengine-go: Button A held at boot — returning to launcher\n");
            vTaskDelay(200 / portTICK_PERIOD_MS);
            odroid_system_application_set(0);
            esp_restart();
        }
    }

    //printf("osd_init: ili9341_prepare\n");
    ili9341_prepare();

    /////
    check_boot_cause();
    
    esp_err_t r = odroid_sdcard_open(SD_BASE_PATH);
    if (r != ESP_OK)
    {
        odroid_display_show_sderr(ODROID_SD_ERR_NOCARD);
        abort();
    }
    
    // Read ROM path directly from NVS (set by launcher)
    char *rom_file = odroid_settings_RomFilePath_get();
    if (rom_file && strlen(rom_file) >= 4)
    {
        printf("pcengine: ROM from NVS: '%s'\n", rom_file);
    }
    else
    {
        // Fallback to file browser if no ROM path in NVS
        if (rom_file) free(rom_file);
        rom_file = odroid_ui_choose_file("/sd/roms/pce", "pce");
        if (!rom_file)
        {
            printf("No file selected!\n");
            abort();
        }
    }
    
    cart_name = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    short_cart_name = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    short_iso_name = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    rom_file_name = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    config_basepath = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    sav_path = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    sav_basepath = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    tmp_basepath = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    video_path = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    ISO_filename = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    syscard_filename = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    cdsystem_path = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    
    spr_init_pos = (uint32 *)my_special_alloc(false, 4, 1024 * 4);
    SPM_raw = (uchar*)my_special_alloc(false, 1, XBUF_WIDTH * XBUF_HEIGHT);
    SPM = SPM_raw + XBUF_WIDTH * 64 + 32;
    memset(SPM_raw,0, XBUF_WIDTH * XBUF_HEIGHT);
    log_filename = (char *)my_special_alloc(false, 1, PATH_MAX_MY);
    strcpy(cart_name, "");
    strcpy(short_cart_name, "");
    strcpy(rom_file_name, "");
    strcpy(config_basepath, "/sd/odroid/data/pce");
    strcpy(tmp_basepath, "");
    strcpy(video_path, "");
    strcpy(ISO_filename, "");
    strcpy(sav_basepath,"/sd/odroid/data");
    strcpy(sav_path,"pce");

    /* Ensure save directory exists */
    mkdir("/sd/odroid", 0755);
    mkdir("/sd/odroid/data", 0755);
    mkdir("/sd/odroid/data/pce", 0755);
    
    framebuffer[0] = my_special_alloc(false, 1, XBUF_WIDTH * XBUF_HEIGHT);
    // framebuffer[0] = heap_caps_malloc(XBUF_WIDTH * XBUF_HEIGHT, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!framebuffer[0]) abort();
    printf("app_main: framebuffer[0]=%p\n", framebuffer[0]);

     framebuffer[1] = my_special_alloc(false, 1, XBUF_WIDTH * XBUF_HEIGHT);
    //framebuffer[1] = heap_caps_malloc(XBUF_WIDTH * XBUF_HEIGHT, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!framebuffer[1]) abort();
    printf("app_main: framebuffer[1]=%p\n", framebuffer[1]);
    memset(framebuffer[0], 0, XBUF_WIDTH * XBUF_HEIGHT);
    memset(framebuffer[1], 0, XBUF_WIDTH * XBUF_HEIGHT);
    
    my_palette = my_special_alloc(false, 1, 256 * sizeof(uint16_t));
    if (!my_palette) abort();
    
    XBuf = framebuffer[0];
    osd_gfx_buffer = XBuf + 32 + 64 * XBUF_WIDTH;
#ifdef MY_GFX_AS_TASK
#ifdef MY_VIDEO_MODE_SCANLINES
    vidQueue = xQueueCreate(64, sizeof(struct my_scanline));
#else
    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
#endif
#endif

    QuickSaveSetBuffer(my_special_alloc(false, 1, 512));
    
    odroid_audio_init(odroid_settings_AudioSink_get(), AUDIO_SAMPLE_RATE);
    
    InitPCE(rom_file);
    osd_init_machine();

    /* DataSlot-based auto-load: set flag, actual load happens after ResetPCE() */
    {
        int32_t dataSlot = odroid_settings_DataSlot_get();
        printf("pcengine: DataSlot=%d\n", dataSlot);
        if (dataSlot == 1) {
            pce_should_resume = true;
        }
        odroid_settings_DataSlot_set(0); /* clear to prevent crash-resume loops */
    }
#ifdef MY_GFX_AS_TASK
    update_display_task(77);
#endif
#ifdef MY_SND_AS_TASK
    host.sound.stereo = true;
    host.sound.signed_sound = false;
    host.sound.freq = AUDIO_SAMPLE_RATE;
    host.sound.sample_size = 1;
    for (int i = 0;i < AUDIO_CHANNELS; i++)
    {
        sbuf[i] = my_special_alloc(false, 1, AUDIO_BUFFER_SIZE/2);
    } 
    sbuf_mix[0] = my_special_alloc(false, 1, AUDIO_BUFFER_SIZE);
    sbuf_mix[1] = my_special_alloc(false, 1, AUDIO_BUFFER_SIZE);
    
    audioQueue = xQueueCreate(1, sizeof(uint16_t*));
    EmuAudio(true);
    printf("VIDEO: Task: Start done\n");
#endif
}

NOINLINE void app_loop(void)
{
   printf("up and running\n");
   //if (!(*osd_gfx_driver_list[video_driver].init) ())
   odroid_ui_enter_loop();
   RunPCE();
   abort();
}

void app_main(void)
{
    app_init();
    app_loop();
}


/*
 * Save state format (version 2):
 *   4 bytes: magic "PCE\x02"
 *   CPU registers + timing
 *   IO struct (raw, pointers patched on load)
 *   VCE palette (0x200 pairs = 1024 bytes)
 *   PSG DA data (6 * 1024 = 6144 bytes)
 *   RAM (0x8000 = 32768 bytes)
 *   WRAM (0x2000 = 8192 bytes)
 *   VRAM (0x10000 = 65536 bytes)
 *   SPRAM (64*4*2 = 512 bytes)
 *   Pal (512 bytes)
 *   snd_vol[6][32] (192 bytes)
 *   VDC registers (21 pairs = 42 bytes)
 *   Total: ~116 KB
 */

extern uint32 TimerCount;
extern signed char snd_vol[6][32];
extern DRAM_ATTR uint32 reg_pc_;
extern DRAM_ATTR uchar reg_a_;
extern DRAM_ATTR uchar reg_x_;
extern DRAM_ATTR uchar reg_y_;
extern DRAM_ATTR uchar reg_p_;
extern DRAM_ATTR uchar reg_s_;
extern DRAM_ATTR uint32 cycles_;

#define SAVE_MAGIC "PCE\x02"

bool QuickSaveState(FILE *f)
{
    if (!f) return false;

    /* Magic */
    fwrite(SAVE_MAGIC, 1, 4, f);

    /* CPU registers */
    fwrite(&reg_pc_, sizeof(reg_pc_), 1, f);
    fwrite(&reg_a_, sizeof(reg_a_), 1, f);
    fwrite(&reg_x_, sizeof(reg_x_), 1, f);
    fwrite(&reg_y_, sizeof(reg_y_), 1, f);
    fwrite(&reg_p_, sizeof(reg_p_), 1, f);
    fwrite(&reg_s_, sizeof(reg_s_), 1, f);
    fwrite(&cycles_, sizeof(cycles_), 1, f);

    /* Memory mapping */
    fwrite(mmr, 1, 8, f);

    /* Timing */
    fwrite(&TimerCount, sizeof(TimerCount), 1, f);
    uint32 sc = scanline;
    fwrite(&sc, sizeof(sc), 1, f);
    uint32 cc = cyclecount;
    fwrite(&cc, sizeof(cc), 1, f);
    uint32 cco = *p_cyclecountold;
    fwrite(&cco, sizeof(cco), 1, f);

    /* IO struct — save raw (includes embedded pointer values, patched on load) */
    fwrite(&io, sizeof(IO), 1, f);

    /* VCE palette data */
    fwrite(io.VCE, sizeof(pair), 0x200, f);

    /* PSG direct access data */
    for (int i = 0; i < 6; i++) {
        fwrite(io.psg_da_data[i], 1, PSG_DIRECT_ACCESS_BUFSIZE, f);
    }

    /* RAM */
    fwrite(RAM, 1, 0x8000, f);

    /* WRAM (backup RAM) */
    fwrite(WRAM, 1, 0x2000, f);

    /* VRAM */
    fwrite(VRAM, 1, 0x10000, f);

    /* Sprite RAM */
    fwrite(SPRAM, sizeof(uint16), 64 * 4, f);

    /* Palette conversion table */
    fwrite(Pal, 1, 512, f);

    /* PSG volume table */
    fwrite(snd_vol, 1, sizeof(snd_vol), f);

    /* VDC registers (21 standalone globals, NOT part of io struct) */
    fwrite(&IO_VDC_00_MAWR, sizeof(pair), 1, f);
    fwrite(&IO_VDC_01_MARR, sizeof(pair), 1, f);
    fwrite(&IO_VDC_02_VWR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_03_vdc3, sizeof(pair), 1, f);
    fwrite(&IO_VDC_04_vdc4, sizeof(pair), 1, f);
    fwrite(&IO_VDC_05_CR,   sizeof(pair), 1, f);
    fwrite(&IO_VDC_06_RCR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_07_BXR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_08_BYR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_09_MWR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0A_HSR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0B_HDR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0C_VPR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0D_VDW,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0E_VCR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_0F_DCR,  sizeof(pair), 1, f);
    fwrite(&IO_VDC_10_SOUR, sizeof(pair), 1, f);
    fwrite(&IO_VDC_11_DISTR,sizeof(pair), 1, f);
    fwrite(&IO_VDC_12_LENR, sizeof(pair), 1, f);
    fwrite(&IO_VDC_13_SATB, sizeof(pair), 1, f);
    fwrite(&IO_VDC_14,      sizeof(pair), 1, f);

    printf("QuickSaveState: saved OK\n");
    return true;
}

bool QuickLoadState(FILE *f)
{
    if (!f) return false;

    /* Verify magic */
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) return false;
    if (memcmp(magic, SAVE_MAGIC, 4) != 0) {
        printf("QuickLoadState: bad magic\n");
        return false;
    }

    /* CPU registers */
    fread(&reg_pc_, sizeof(reg_pc_), 1, f);
    fread(&reg_a_, sizeof(reg_a_), 1, f);
    fread(&reg_x_, sizeof(reg_x_), 1, f);
    fread(&reg_y_, sizeof(reg_y_), 1, f);
    fread(&reg_p_, sizeof(reg_p_), 1, f);
    fread(&reg_s_, sizeof(reg_s_), 1, f);
    fread(&cycles_, sizeof(cycles_), 1, f);

    /* Memory mapping — save current values, restore after bank_set */
    uchar saved_mmr[8];
    fread(saved_mmr, 1, 8, f);

    /* Timing */
    fread(&TimerCount, sizeof(TimerCount), 1, f);
    uint32 sc, cc, cco;
    fread(&sc, sizeof(sc), 1, f);
    *p_scanline = sc;
    fread(&cc, sizeof(cc), 1, f);
    *p_cyclecount = cc;
    fread(&cco, sizeof(cco), 1, f);
    *p_cyclecountold = cco;

    /* IO struct — save current pointers before overwriting */
    pair *cur_VCE = io.VCE;
    uchar *cur_psg_da[6];
    for (int i = 0; i < 6; i++) cur_psg_da[i] = io.psg_da_data[i];

    /* Load IO struct (overwrites pointers with stale values from save) */
    fread(&io, sizeof(IO), 1, f);

    /* Restore valid pointers from current session */
    io.VCE = cur_VCE;
    for (int i = 0; i < 6; i++) io.psg_da_data[i] = cur_psg_da[i];

    /* Load VCE palette data */
    fread(io.VCE, sizeof(pair), 0x200, f);

    /* Load PSG direct access data */
    for (int i = 0; i < 6; i++) {
        fread(io.psg_da_data[i], 1, PSG_DIRECT_ACCESS_BUFSIZE, f);
    }

    /* RAM */
    fread(RAM, 1, 0x8000, f);

    /* WRAM */
    fread(WRAM, 1, 0x2000, f);

    /* VRAM */
    fread(VRAM, 1, 0x10000, f);

    /* Sprite RAM */
    fread(SPRAM, sizeof(uint16), 64 * 4, f);

    /* Palette conversion table */
    fread(Pal, 1, 512, f);

    /* PSG volume table */
    fread(snd_vol, 1, sizeof(snd_vol), f);

    /* VDC registers (21 standalone globals) */
    fread(&IO_VDC_00_MAWR, sizeof(pair), 1, f);
    fread(&IO_VDC_01_MARR, sizeof(pair), 1, f);
    fread(&IO_VDC_02_VWR,  sizeof(pair), 1, f);
    fread(&IO_VDC_03_vdc3, sizeof(pair), 1, f);
    fread(&IO_VDC_04_vdc4, sizeof(pair), 1, f);
    fread(&IO_VDC_05_CR,   sizeof(pair), 1, f);
    fread(&IO_VDC_06_RCR,  sizeof(pair), 1, f);
    fread(&IO_VDC_07_BXR,  sizeof(pair), 1, f);
    fread(&IO_VDC_08_BYR,  sizeof(pair), 1, f);
    fread(&IO_VDC_09_MWR,  sizeof(pair), 1, f);
    fread(&IO_VDC_0A_HSR,  sizeof(pair), 1, f);
    fread(&IO_VDC_0B_HDR,  sizeof(pair), 1, f);
    fread(&IO_VDC_0C_VPR,  sizeof(pair), 1, f);
    fread(&IO_VDC_0D_VDW,  sizeof(pair), 1, f);
    fread(&IO_VDC_0E_VCR,  sizeof(pair), 1, f);
    fread(&IO_VDC_0F_DCR,  sizeof(pair), 1, f);
    fread(&IO_VDC_10_SOUR, sizeof(pair), 1, f);
    fread(&IO_VDC_11_DISTR,sizeof(pair), 1, f);
    fread(&IO_VDC_12_LENR, sizeof(pair), 1, f);
    fread(&IO_VDC_13_SATB, sizeof(pair), 1, f);
    fread(&IO_VDC_14,      sizeof(pair), 1, f);

    /* Rebuild memory bank mapping from saved MMR */
    for (int i = 0; i < 8; i++) {
        bank_set(i, saved_mmr[i]);
    }

    /* Rebuild zp_base and sp_base from current RAM pointer */
    extern uchar *zp_base;
    extern uchar *sp_base;
    zp_base = RAM;
    sp_base = RAM + 0x100;

    /* Mark all tile/sprite caches dirty so VRAM2/VRAMS rebuild */
    extern uchar *vchange, *vchanges;
    memset(vchange, 1, 0x10000 / 32);
    memset(vchanges, 1, 0x10000 / 128);

    /* Force video mode reconfiguration so display task matches loaded screen_w */
    extern int gfx_need_video_mode_change;
    gfx_need_video_mode_change = 1;

    printf("QuickLoadState: loaded OK\n");
    return true;
}
