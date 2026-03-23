#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_ota_ops.h"

extern "C"
{
#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_sdcard.h"

#include "../components/ugui/ugui.h"
}

// Stella
#include "Console.hxx"
#include "Cart.hxx"
#include "Props.hxx"
#include "MD5.hxx"
#include "Sound.hxx"
#include "OSystem.hxx"
#include "TIA.hxx"
#include "PropsSet.hxx"
#include "Switches.hxx"
#include "SoundSDL.hxx"
#include "Control.hxx"

#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

#define ESP32_PSRAM (0x3f800000)
const char* SD_BASE_PATH = "/sd";

#define AUDIO_SAMPLE_RATE (31400)

QueueHandle_t vidQueue;

#define STELLA_WIDTH 160
#define STELLA_HEIGHT 250
// Phosphor blending: Atari 2600 games like Asteroids alternate which objects
// are drawn on odd/even frames (sprite multiplexing). We accumulate two
// consecutive frames into a phosphor buffer so both sets of objects appear
// in each displayed image, dramatically reducing perceived flicker.
static uint8_t* phosphorBuf[2] = {NULL, NULL};
static int phosphorIdx = 0;
#define PHOSPHOR_SIZE (STELLA_WIDTH * STELLA_HEIGHT)  // 40000 bytes
uint16_t pal16[256];
bool IsPal;

// Paddle potentiometer on GPIO15 (ADC2_CHANNEL_3)
// Connect a 10K potentiometer: one end to GND, other to 3.3V, wiper to GPIO15.
// The ADC reading (0-4095) maps to Stelladaptor axis (-32768..32767)
// which Stella's Paddles class uses for absolute paddle position.
// The actual ADC read is done inside odroid_input_read_raw() (core 1 gamepad task).
// We read odroid_paddle_adc_raw here.
static bool paddle_adc_enabled = false;
#define PADDLE_ADC_CHANNEL  ADC2_CHANNEL_3
#define PADDLE_DEAD_ZONE    4   // POT units — hysteresis for stationary jitter
#define PADDLE_DETECT_SPREAD 300 // max spread for detection (stable = pot present)
// ADC input range for map function: values outside are clamped.
// Using wide range (100-4095) so the pot travel is less sensitive.
#define PADDLE_ADC_LO   100
#define PADDLE_ADC_HI   4095

void videoTask(void *arg)
{
    while(1)
    {
        uint8_t* param;
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        // With TIA double buffering, param points to the just-completed frame.
        // TIA is now writing to the OTHER buffer, so we can read directly
        // without copying — no race condition.
        ili9341_write_frame_atari2600(param, pal16, IsPal);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);

    }

    odroid_display_lock_sms_display();

    // Draw hourglass
    odroid_display_show_hourglass();

    odroid_display_unlock_sms_display();

    vTaskDelete(NULL);

    while (1) {}
}

// volatile bool test = true;
// volatile uint16_t test2 = true;

UG_GUI gui;
uint16_t* fb;

static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
    fb[y * 320 + x] = color;
}

static void window1callback(UG_MESSAGE* msg)
{
}

static void UpdateDisplay()
{
    UG_Update();
    ili9341_write_frame_rectangleLE(0, 0, 320, 240, fb);
}



// A utility function to swap two elements
inline static void swap(char** a, char** b)
{
    char* t = *a;
    *a = *b;
    *b = t;
}

static int strcicmp(char const *a, char const *b)
{
    for (;; a++, b++)
    {
        int d = tolower((int)*a) - tolower((int)*b);
        if (d != 0 || !*a) return d;
    }
}

//------
/* This function takes last element as pivot, places
   the pivot element at its correct position in sorted
    array, and places all smaller (smaller than pivot)
   to left of pivot and all greater elements to right
   of pivot */
static int partition (char* arr[], int low, int high)
{
    char* pivot = arr[high];    // pivot
    int i = (low - 1);  // Index of smaller element

    for (int j = low; j <= high- 1; j++)
    {
        // If current element is smaller than or
        // equal to pivot
        if (strcicmp(arr[j], pivot) < 0) //(arr[j] <= pivot)
        {
            i++;    // increment index of smaller element
            swap(&arr[i], &arr[j]);
        }
    }
    swap(&arr[i + 1], &arr[high]);
    return (i + 1);
}

/* The main function that implements QuickSort
 arr[] --> Array to be sorted,
  low  --> Starting index,
  high  --> Ending index */
static void quickSort(char* arr[], int low, int high)
{
    if (low < high)
    {
        /* pi is partitioning index, arr[p] is now
           at right place */
        int pi = partition(arr, low, high);

        // Separately sort elements before
        // partition and after partition
        quickSort(arr, low, pi - 1);
        quickSort(arr, pi + 1, high);
    }
}

IRAM_ATTR static void bubble_sort(char** files, int count)
{
    int n = count;
    bool swapped = true;

    while (n > 0)
    {
        int newn = 0;
        for (int i = 1; i < n; ++i)
        {
            if (strcicmp(files[i - 1], files[i]) > 0)
            {
                char* temp = files[i - 1];
                files[i - 1] = files[i];
                files[i] = temp;

                newn = i;
            }
        } //end for
        n = newn;
    } //until n = 0
}

static void SortFiles(char** files, int count)
{
    int n = count;
    bool swapped = true;

    if (count > 1)
    {
        //quickSort(files, 0, count - 1);
        bubble_sort(files, count - 1);
    }
}

int GetFiles(const char* path, const char* extension, char*** filesOut)
{
    //printf("GetFiles: path='%s', extension='%s'\n", path, extension);
    //OpenSDCard();

    const int MAX_FILES = 4096;

    int count = 0;
    char** result = (char**)heap_caps_malloc(MAX_FILES * sizeof(void*), MALLOC_CAP_SPIRAM);
    //char** result = (char**)malloc(MAX_FILES * sizeof(void*));
    if (!result) abort();

    //*filesOut = result;

    DIR *dir = opendir(path);
    if( dir == NULL )
    {
        printf("GetFiles: opendir('%s') failed, no ROMs directory.\n", path);
        *filesOut = result;
        return 0;
    }

    int extensionLength = strlen(extension);
    if (extensionLength < 1) abort();


    char* temp = (char*)malloc(extensionLength + 1);
    if (!temp) abort();

    memset(temp, 0, extensionLength + 1);


    // Print files
    struct dirent *entry;
    while((entry=readdir(dir)) != NULL)
    {
        //printf("File: %s\n", entry->d_name);
        size_t len = strlen(entry->d_name);

        bool skip = false;

        // ignore 'hidden' files (MAC)
        if (entry->d_name[0] == '.') skip = true;

        // ignore BIOS file(s)
        char* lowercase = (char*)malloc(len + 1);
        if (!lowercase) abort();

        lowercase[len] = 0;
        for (int i = 0; i < len; ++i)
        {
            lowercase[i] = tolower((int)entry->d_name[i]);
        }
        if (strcmp(lowercase, "bios.col") == 0) skip = true;

        free(lowercase);


        memset(temp, 0, extensionLength + 1);
        if (!skip)
        {
            for (int i = 0; i < extensionLength; ++i)
            {
                temp[i] = tolower((int)entry->d_name[len - extensionLength + i]);
            }

            if (len > extensionLength)
            {
                if (strcmp(temp, extension) == 0)
                {
                    result[count] = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
                    //result[count] = (char*)malloc(len + 1);
                    if (!result[count])
                    {
                        abort();
                    }

                    strcpy(result[count], entry->d_name);
                    ++count;

                    if (count >= MAX_FILES) break;
                }
            }
        }
    }

    closedir(dir);
    free(temp);
    //CloseSDCard();


    SortFiles(result, count);

#if 0
    for (int i = 0; i < count; ++i)
    {
        printf("GetFiles: %d='%s'\n", i, result[i]);
    }
#endif

    *filesOut = result;
    return count;
}

void FreeFiles(char** files, int count)
{
    for (int i = 0; i < count; ++i)
    {
        free(files[i]);
    }

    free(files);
}





#define MAX_OBJECTS 20
#define ITEM_COUNT  10

UG_WINDOW window1;
UG_BUTTON button1;
UG_TEXTBOX textbox[ITEM_COUNT];
UG_OBJECT objbuffwnd1[MAX_OBJECTS];

/* ---- Direct-render state (bypasses ugui for item display) ---- */
static int   lastDrawnPage  = -1;
static int   lastDrawnItem  = -1;
static UG_S16 cachedContentY   = 0;  /* Y pixel where item area starts    */
static UG_S16 cachedContentXS  = 0;  /* X left edge of content area       */
static UG_S16 cachedContentXE  = 0;  /* X right edge of content area      */
static UG_S16 cachedTextHeight = 0;  /* pixel height of one text row      */
static char   displayStrBuf[ITEM_COUNT][40]; /* cached display strings     */

static void DrawPage_ResetState()
{
    lastDrawnPage  = -1;
    lastDrawnItem  = -1;
    memset(displayStrBuf, 0, sizeof(displayStrBuf));
}

/* Render one text row directly into framebuffer (no ugui) */
static void RenderTextRow(int rowIdx, const char* text, uint16_t bg, uint16_t fg)
{
    int y0 = cachedContentY + rowIdx * cachedTextHeight;
    int h  = cachedTextHeight;
    if (y0 + h > 240) h = 240 - y0;
    if (y0 < 0 || h <= 0) return;

    int xL = cachedContentXS;
    int xR = cachedContentXE;
    int cw = xR - xL + 1;

    /* Fill background for content area of this row */
    for (int row = 0; row < h; row++) {
        uint16_t* p = &fb[(y0 + row) * 320 + xL];
        for (int x = 0; x < cw; x++) p[x] = bg;
    }

    if (!text || !*text) return;

    /* Render text using FONT_8X12 bitmap data (1BPP, 8px wide, 12px tall) */
    const UG_FONT* font = &FONT_8X12;
    int charW  = font->char_width;   /* 8  */
    int charH  = font->char_height;  /* 12 */
    int bn     = (charW + 7) >> 3;   /* bytes per row = 1 */

    int textLen = (int)strlen(text);
    int textPxW = textLen * charW;

    /* Center horizontally within content area */
    int tx = xL + (cw - textPxW) / 2;
    if (tx < xL) tx = xL;

    /* Center vertically within row */
    int ty = (h - charH) / 2;
    if (ty < 0) ty = 0;

    for (int ci = 0; ci < textLen; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        if (ch < font->start_char || ch > font->end_char) continue;
        int cx = tx + ci * charW;
        if (cx + charW > 320) break;
        if (cx < 0) continue;

        unsigned int fidx = (unsigned int)(ch - font->start_char) * charH * bn;
        for (int r = 0; r < charH && ty + r < h; r++) {
            unsigned char b = font->p[fidx + r * bn];
            uint16_t* pix = &fb[(y0 + ty + r) * 320 + cx];
            for (int bit = 0; bit < charW; bit++) {
                if (b & (1 << bit)) pix[bit] = fg;
            }
        }
    }
}

/* Prepare a display string from a filename (strip extension, truncate) */
static void PrepareDisplayStr(const char* fileName, char* dst, size_t maxLen)
{
    const char* dot = strrchr(fileName, '.');
    size_t len = dot ? (size_t)(dot - fileName) : strlen(fileName);
    if (len > maxLen) len = maxLen;
    memcpy(dst, fileName, len);
    dst[len] = 0;
}

/* Highlight color: green for partial, yellow for full-redraw */
#define HL_COLOR_FULL    0xFFE0  /* yellow */
#define HL_COLOR_PARTIAL 0x07E0  /* bright green */

void DrawPage(char** files, int fileCount, int currentItem)
{
    static const size_t MAX_DISPLAY_LENGTH = 38;

    int page = currentItem / ITEM_COUNT;
    page *= ITEM_COUNT;

    bool pageChanged = (page != lastDrawnPage);

    /* ====== FULL PAGE REDRAW (page change or first draw) ====== */
    if (pageChanged || lastDrawnItem < 0)
    {
        printf("DrawPage FULL: page=%d item=%d (lastPage=%d lastItem=%d)\n",
               page, currentItem, lastDrawnPage, lastDrawnItem);

        if (fileCount < 1)
        {
            for (int line = 0; line < ITEM_COUNT; line++) {
                displayStrBuf[line][0] = 0;
                bool isMid = (line == ITEM_COUNT / 2);
                RenderTextRow(line, isMid ? "(empty)" : NULL, 0xFFFF, 0x0000);
            }
        }
        else
        {
            for (int line = 0; line < ITEM_COUNT; line++) {
                if (page + line < fileCount) {
                    PrepareDisplayStr(files[page + line],
                                      displayStrBuf[line], MAX_DISPLAY_LENGTH);
                    bool selected = (page + line == currentItem);
                    RenderTextRow(line, displayStrBuf[line],
                                  selected ? HL_COLOR_FULL : 0xFFFF, 0x0000);
                } else {
                    displayStrBuf[line][0] = 0;
                    RenderTextRow(line, NULL, 0xFFFF, 0x0000);
                }
            }
        }

        /* Send entire content area in one SPI transfer */
        int y0   = cachedContentY;
        int yEnd = cachedContentY + ITEM_COUNT * cachedTextHeight;
        if (yEnd > 240) yEnd = 240;
        int totalH = yEnd - y0;
        if (totalH > 0)
            ili9341_write_frame_rectangleLE(0, y0, 320, totalH, &fb[y0 * 320]);

        lastDrawnPage = page;
        lastDrawnItem = currentItem;
    }
    /* ====== PARTIAL UPDATE (same page, just highlight change) ====== */
    else if (currentItem != lastDrawnItem)
    {
        int prevLine = lastDrawnItem - page;
        int currLine = currentItem  - page;

        printf("DrawPage PARTIAL: item %d->%d (lines %d->%d)\n",
               lastDrawnItem, currentItem, prevLine, currLine);

        /* Re-render only the two changed rows */
        RenderTextRow(prevLine, displayStrBuf[prevLine], 0xFFFF, 0x0000);
        RenderTextRow(currLine, displayStrBuf[currLine], HL_COLOR_PARTIAL, 0x0000);

        /* Send ONE contiguous SPI transfer covering both rows */
        int topLine = (prevLine < currLine) ? prevLine : currLine;
        int botLine = (prevLine < currLine) ? currLine : prevLine;
        int y0      = cachedContentY + topLine * cachedTextHeight;
        int yEnd    = cachedContentY + (botLine + 1) * cachedTextHeight;
        if (yEnd > 240) yEnd = 240;
        int totalH  = yEnd - y0;
        if (totalH > 0)
            ili9341_write_frame_rectangleLE(0, y0, 320, totalH, &fb[y0 * 320]);

        lastDrawnItem = currentItem;
    }
}

static const char* ChooseFile()
{
    const char* result = NULL;

    /* Reset partial-update state from any previous invocation */
    DrawPage_ResetState();

    fb = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!fb) abort();
    //fb = (uint16_t*)ESP32_PSRAM;

    UG_Init(&gui, pset, 320, 240);

    UG_WindowCreate(&window1, objbuffwnd1, MAX_OBJECTS, window1callback);

    UG_WindowSetTitleText(&window1, "CHOOSE ROM v5");
    UG_WindowSetTitleTextFont(&window1, &FONT_10X16);
    UG_WindowSetTitleTextAlignment(&window1, ALIGN_CENTER);


    UG_S16 innerWidth = UG_WindowGetInnerWidth(&window1);
    UG_S16 innerHeight = UG_WindowGetInnerHeight(&window1);
    UG_S16 titleHeight = UG_WindowGetTitleHeight(&window1);
    UG_S16 textHeight = (innerHeight) / ITEM_COUNT;

    /* Cache layout metrics for direct-render path */
    {
        UG_AREA area;
        UG_WindowGetArea(&window1, &area);
        cachedContentY   = area.ys;
        cachedContentXS  = area.xs;
        cachedContentXE  = area.xe;
        cachedTextHeight = textHeight;
        printf("DrawPage layout: contentY=%d, xS=%d, xE=%d, textH=%d\n",
               cachedContentY, cachedContentXS, cachedContentXE, cachedTextHeight);
    }


    for (int i = 0; i < ITEM_COUNT; ++i)
    {
        uint16_t id = TXB_ID_0 + i;
        UG_S16 top = i * textHeight;
        UG_TextboxCreate(&window1, &textbox[i], id, 0, top, innerWidth, top + textHeight - 1);
        UG_TextboxSetFont(&window1, id, &FONT_8X12);
        UG_TextboxSetForeColor(&window1, id, C_BLACK);
        UG_TextboxSetAlignment(&window1, id, ALIGN_CENTER);
        //UG_TextboxSetText(&window1, id, "ABCDEFGHabcdefg");
    }

    UG_WindowShow(&window1);
    UpdateDisplay();


    const char* path = "/sd/roms/a26";
    char** files;

    // Create the ROMs directory if it doesn't exist
    mkdir(path, 0755);

    // Search for both .a26 and .bin files
    char** files_a26;
    int count_a26 = GetFiles(path, ".a26", &files_a26);

    char** files_bin;
    int count_bin = GetFiles(path, ".bin", &files_bin);

    // Merge both lists
    int fileCount = count_a26 + count_bin;
    files = (char**)heap_caps_malloc((fileCount + 1) * sizeof(void*), MALLOC_CAP_SPIRAM);
    if (!files) abort();
    int idx = 0;
    for (int i = 0; i < count_a26; ++i) files[idx++] = files_a26[i];
    for (int i = 0; i < count_bin; ++i) files[idx++] = files_bin[i];
    // Free the containers (not the strings - they're now owned by files[])
    free(files_a26);
    free(files_bin);
    // Sort the merged list
    if (fileCount > 0) SortFiles(files, fileCount);
    printf("ChooseFile: Found %d ROMs (%d .a26, %d .bin)\n", fileCount, count_a26, count_bin);


// Selection
    int currentItem = 0;
    DrawPage(files, fileCount, currentItem);

    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    /* ---- Auto-repeat state ---- */
    #define REPEAT_INITIAL_MS  250   /* ms before first repeat   */
    #define REPEAT_INTERVAL_MS  60   /* ms between repeats        */
    int  heldDir        = 0;         /* 0=none, 1=down, 2=up, 3=right, 4=left */
    int  holdCounter    = 0;         /* ticks held (each tick ~10ms)           */
    const int initialTicks  = REPEAT_INITIAL_MS / 10;
    const int repeatTicks   = REPEAT_INTERVAL_MS / 10;

    while (true)
    {
		odroid_gamepad_state state;
		odroid_input_gamepad_read(&state);

        int page = currentItem / 10;
        page *= 10;

        /* Determine which d-pad direction is currently pressed */
        int curDir = 0;
        if      (state.values[ODROID_INPUT_DOWN])  curDir = 1;
        else if (state.values[ODROID_INPUT_UP])    curDir = 2;
        else if (state.values[ODROID_INPUT_RIGHT]) curDir = 3;
        else if (state.values[ODROID_INPUT_LEFT])  curDir = 4;

        /* Track hold duration */
        if (curDir != 0 && curDir == heldDir) {
            holdCounter++;
        } else {
            heldDir = curDir;
            holdCounter = 0;
        }

        /* Decide if we should act this tick:
         *  - Fresh press (edge): always act
         *  - Held past initial delay: act every repeatTicks */
        bool freshDown  = !previousState.values[ODROID_INPUT_DOWN]  && state.values[ODROID_INPUT_DOWN];
        bool freshUp    = !previousState.values[ODROID_INPUT_UP]    && state.values[ODROID_INPUT_UP];
        bool freshRight = !previousState.values[ODROID_INPUT_RIGHT] && state.values[ODROID_INPUT_RIGHT];
        bool freshLeft  = !previousState.values[ODROID_INPUT_LEFT]  && state.values[ODROID_INPUT_LEFT];

        bool repeatNow  = (holdCounter >= initialTicks) &&
                          ((holdCounter - initialTicks) % repeatTicks == 0);

        bool actDown  = freshDown  || (curDir == 1 && repeatNow);
        bool actUp    = freshUp    || (curDir == 2 && repeatNow);
        bool actRight = freshRight || (curDir == 3 && repeatNow);
        bool actLeft  = freshLeft  || (curDir == 4 && repeatNow);

		if (fileCount > 0)
		{
	        if(actDown)
	        {
				if (currentItem + 1 < fileCount)
	            {
	                ++currentItem;
	                DrawPage(files, fileCount, currentItem);
	            }
				else
				{
					currentItem = 0;
	                DrawPage(files, fileCount, currentItem);
				}
	        }
	        else if(actUp)
	        {
				if (currentItem > 0)
	            {
	                --currentItem;
	                DrawPage(files, fileCount, currentItem);
	            }
				else
				{
					currentItem = fileCount - 1;
					DrawPage(files, fileCount, currentItem);
				}
	        }
	        else if(actRight)
	        {
				if (page + 10 < fileCount)
	            {
	                currentItem = page + 10;
	                DrawPage(files, fileCount, currentItem);
	            }
				else
				{
					currentItem = 0;
					DrawPage(files, fileCount, currentItem);
				}
	        }
	        else if(actLeft)
	        {
				if (page - 10 >= 0)
	            {
	                currentItem = page - 10;
	                DrawPage(files, fileCount, currentItem);
	            }
				else
				{
					currentItem = page;
					while (currentItem + 10 < fileCount)
					{
						currentItem += 10;
					}

	                DrawPage(files, fileCount, currentItem);
				}
	        }
	        else if(!previousState.values[ODROID_INPUT_A] && state.values[ODROID_INPUT_A])
	        {
	            size_t fullPathLength = strlen(path) + 1 + strlen(files[currentItem]) + 1;

	            //char* fullPath = (char*)heap_caps_malloc(fullPathLength, MALLOC_CAP_SPIRAM);
                char* fullPath = (char*)malloc(fullPathLength);
	            if (!fullPath) abort();

	            strcpy(fullPath, path);
	            strcat(fullPath, "/");
	            strcat(fullPath, files[currentItem]);

	            result = fullPath;
                break;
	        }
		}

        previousState = state;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    FreeFiles(files, fileCount);

    /* Release persistent display strings */
    DrawPage_ResetState();

    //free(fb);
    return result;
}

static Console *console = 0;
static Cartridge *cartridge = 0;
static Settings *settings = 0;
static OSystem* osystem;
static uint32_t tiaSamplesPerFrame;

void stella_init(const char* filename)
{
    printf("%s: HEAP:0x%x (%#08x)\n",
      __func__,
      esp_get_free_heap_size(),
      heap_caps_get_free_size(MALLOC_CAP_DMA));

    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        printf("stella_init: ERROR - cannot open '%s'\n", filename);
        ili9341_clear(0xf800); // red screen
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    printf("stella_init: ROM size = %u bytes\n", size);

    void* data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    //void* data = malloc(size);
    if (!data) {
        printf("stella_init: ERROR - cannot allocate %u bytes for ROM\n", size);
        fclose(fp);
        ili9341_clear(0xf800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    size_t count = fread(data, 1, size, fp);
    if (count != size) {
        printf("stella_init: ERROR - read %u of %u bytes\n", count, size);
        fclose(fp);
        free(data);
        ili9341_clear(0xf800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    fclose(fp); fp = NULL;

    printf("stella_init: Computing MD5...\n");
    string cartMD5 = MD5((uInt8*)data, (uInt32)size);
    printf("stella_init: MD5 = %s\n", cartMD5.c_str());

    osystem = new OSystem();
    Properties props;
    osystem->propSet().getMD5(cartMD5, props);

    // Load the cart
    string cartType = props.get(Cartridge_Type);
    string cartId;//, romType("AUTO-DETECT");

    printf("%s: HEAP:0x%x (%#08x)\n",
      __func__,
      esp_get_free_heap_size(),
      heap_caps_get_free_size(MALLOC_CAP_DMA));
    settings = new Settings(osystem);
    settings->setValue("romloadcount", false);
    printf("stella_init: Creating cartridge (type='%s')...\n", cartType.c_str());
    try {
        cartridge = Cartridge::create((const uInt8*)data, (uInt32)size, cartMD5, cartType, cartId, *osystem, *settings);
    } catch(const string& error) {
        printf("stella_init: Cart creation exception: %s\n", error.c_str());
        cartridge = 0;
    } catch(...) {
        printf("stella_init: Cart creation unknown exception\n");
        cartridge = 0;
    }

    if(cartridge == 0)
    {
        printf("Stella: Failed to load cartridge.\n");
        ili9341_clear(0xf800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    printf("stella_init: Creating console...\n");
    // Create the console
    try {
        console = new Console(osystem, cartridge, props);
    } catch(const string& error) {
        printf("stella_init: Console creation exception: %s\n", error.c_str());
        ili9341_clear(0xf800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } catch(...) {
        printf("stella_init: Console creation unknown exception\n");
        ili9341_clear(0xf800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }
    osystem->myConsole = console;

    // Init sound and video
    printf("stella_init: Initializing video and audio...\n");
    console->initializeVideo();
    console->initializeAudio();

    // Get the ROM's width and height
    TIA& tia = console->tia();
    int videoWidth = tia.width();
    int videoHeight = tia.height();

    IsPal = (videoHeight > 210);

    printf("videoWidth = %d, videoHeight = %d\n", videoWidth, videoHeight);
    //framebuffer = (uint16_t*)malloc(videoWidth * videoHeight * 2);
    //if (!framebuffer) abort();

    const uint32_t *palette = console->getPalette(0);
    for (int i = 0; i < 256; ++i)
    {
        uint32_t color = palette[i];

        uint16_t r = (color >> 16) & 0xff;
        uint16_t g = (color >> 8) & 0xff;
        uint16_t b = (color >> 0) & 0xff;


        //rrrr rggg gggb bbbb
        uint16_t rgb565 = ((r << 8) & 0xf800) | ((g << 3) & 0x07e0) | (b >> 3);
        //rgb565 = (rgb565 >> 8) | (rgb565 << 8);
        pal16[i] = rgb565;
    }

    tiaSamplesPerFrame = (uint32_t)(31400.0f/console->getFramerate());
}

static int32_t* sampleBuffer;
void stella_step(odroid_gamepad_state* gamepad)
{
    // Process input
    Event &ev = osystem->eventHandler().event();

    ev.set(Event::Type(Event::JoystickZeroUp), gamepad->values[ODROID_INPUT_UP]);
    ev.set(Event::Type(Event::JoystickZeroDown), gamepad->values[ODROID_INPUT_DOWN]);
    ev.set(Event::Type(Event::JoystickZeroLeft), gamepad->values[ODROID_INPUT_LEFT]);
    ev.set(Event::Type(Event::JoystickZeroRight), gamepad->values[ODROID_INPUT_RIGHT]);
    ev.set(Event::Type(Event::JoystickZeroFire), gamepad->values[ODROID_INPUT_A]);
    ev.set(Event::Type(Event::ConsoleSelect), gamepad->values[ODROID_INPUT_SELECT]);
    ev.set(Event::Type(Event::ConsoleReset), gamepad->values[ODROID_INPUT_START]);

    // Read paddle potentiometer on GPIO15 (ADC2) and set Stelladaptor axis.
    // ADC2 read done here (once per frame) to avoid SAR2/DAC contention
    // that occurs when reading ADC2 in the 100 Hz input task on core 1.
    // EMA smoothing + dead zone prevents jitter from triggering the analog path.
    // ONLY inject SALeftAxis0Value when the game uses Paddles controller —
    // otherwise Joystick::update() interprets the axis as directional input,
    // causing the pot to move the character in joystick games.
    if (paddle_adc_enabled &&
        console->controller(Controller::Left).type() == Controller::Paddles) {
        static int ema_adc = -1;
        static int last_axis = -99999;

        /* DIAGNOSTIC: ADC2 read disabled to test if SAR2/DAC contention
         * is the cause of the ~25% slowdown.  Paddle won't move but
         * emulation speed should return to normal if this is the issue. */
        #if 0
        {
            int raw = 0;
            if (adc2_get_raw(PADDLE_ADC_CHANNEL, ADC_WIDTH_12Bit, &raw) == ESP_OK) {
                odroid_paddle_adc_raw = raw;
            }
        }
        #endif

        int raw_adc = odroid_paddle_adc_raw;

        // EMA smoothing: alpha ~0.2 (51/256)
        if (ema_adc < 0) {
            ema_adc = raw_adc;
        } else {
            ema_adc = (51 * raw_adc + 205 * ema_adc + 128) >> 8;
        }

        // map(value, fromLo, fromHi, toLo, toHi) — Arduino-style linear map
        // ADC_LO (100) → axis +32767 (right), ADC_HI (4095) → axis -32767 (left)
        // Clamp input to ADC range first, then scale linearly.
        int clamped = ema_adc;
        if (clamped < PADDLE_ADC_LO) clamped = PADDLE_ADC_LO;
        if (clamped > PADDLE_ADC_HI) clamped = PADDLE_ADC_HI;
        int paddle_axis = (int)((long)(PADDLE_ADC_HI - clamped) * 65534 / (PADDLE_ADC_HI - PADDLE_ADC_LO)) - 32767;

        if (paddle_axis < -32767) paddle_axis = -32767;
        if (paddle_axis >  32767) paddle_axis =  32767;

        // Dead zone: only inject event when axis moves enough.
        // Paddles::update() checks abs(lastAxis - axis) > 10;
        // our dead zone prevents noise from crossing that threshold.
        int diff = paddle_axis - last_axis;
        if (diff < 0) diff = -diff;
        if (last_axis == -99999 || diff > 200) {
            last_axis = paddle_axis;
            ev.set(Event::Type(Event::SALeftAxis0Value), paddle_axis);
        }
    }

    //Tell all input devices to read their state from the event structure
    console->controller(Controller::Left).update();
    console->controller(Controller::Right).update();
    console->switches().update();


    // Emulate
    TIA& tia = console->tia();
    tia.update();

    // Audio
    if (sampleBuffer == NULL)
    {
        sampleBuffer = (int32_t*)malloc(tiaSamplesPerFrame * sizeof(int32_t));
        if (!sampleBuffer) abort();
    }

    SoundSDL *sound = (SoundSDL*)&osystem->sound();
    sound->processFragment((int16_t*)sampleBuffer, tiaSamplesPerFrame);

    odroid_audio_submit((int16_t*)sampleBuffer, tiaSamplesPerFrame);
}


// In-game menu: returns 0=Continue, 1=Restart, 2=Quit
static int show_game_menu()
{
    uint16_t* menu_fb = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!menu_fb) return 0;

    // We'll draw directly to menu_fb via the existing pset/gui
    uint16_t* saved_fb = fb;
    fb = menu_fb;
    UG_Init(&gui, pset, 320, 240);

    const char* options[] = { "Continue", "Restart Game", "Quit to Menu" };
    int count = 3;
    int selected = 0;

    odroid_gamepad_state prev;
    odroid_input_gamepad_read(&prev);

    while (true)
    {
        // Dark background
        for (int i = 0; i < 320 * 240; i++) menu_fb[i] = 0x0000;

        // Draw border box
        UG_FillFrame(60, 40, 260, 180, C_NAVY);
        UG_DrawFrame(60, 40, 260, 180, C_WHITE);
        UG_DrawFrame(61, 41, 259, 179, C_WHITE);

        // Title
        UG_FontSelect(&FONT_10X16);
        UG_SetForecolor(C_WHITE);
        UG_SetBackcolor(C_NAVY);
        UG_PutString(105, 50, (char*)"GAME MENU");

        // Draw separator line
        UG_FillFrame(70, 70, 250, 71, C_WHITE);

        // Options
        UG_FontSelect(&FONT_8X12);
        for (int i = 0; i < count; i++)
        {
            int y_pos = 85 + i * 28;
            if (i == selected) {
                UG_FillFrame(70, y_pos - 2, 250, y_pos + 14, C_YELLOW);
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
            } else {
                UG_SetForecolor(C_WHITE);
                UG_SetBackcolor(C_NAVY);
            }
            UG_PutString(90, y_pos, (char*)options[i]);
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

    // Wait for key release with timeout to prevent hang
    odroid_gamepad_state rel;
    int release_timeout = 100; // ~2 seconds at 20ms per loop
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        odroid_input_gamepad_read(&rel);
        if (--release_timeout <= 0) break;
    } while (rel.values[ODROID_INPUT_A] || rel.values[ODROID_INPUT_B] || rel.values[ODROID_INPUT_MENU]);

    fb = saved_fb;
    free(menu_fb);
    return selected;
}


bool RenderFlag;
extern "C" void app_main()
{
    printf("stella-go started.\n");

    printf("HEAP:0x%x (%#08x)\n",
      esp_get_free_heap_size(),
      heap_caps_get_free_size(MALLOC_CAP_DMA));


    nvs_flash_init();

    odroid_system_init();
    odroid_input_gamepad_init();
    odroid_input_battery_level_init();
    odroid_input_battery_monitor_enabled_set(0);  // disable — shares core 1 with input task

    /* Paddle potentiometer detection on GPIO15 (ADC2_CH3).
     * A real pot gives stable readings. A floating pin reads erratically.
     * Take 4 samples and check max spread. */
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
                vals[i] = -1;
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

    ili9341_prepare();

    ili9341_init();

    // --- PROGRESS: BLUE = display init done ---
    ili9341_clear(0x001F);
    printf("stella-go: Display initialized.\n");
    vTaskDelay(200 / portTICK_PERIOD_MS);

    // Open SD card
    printf("stella-go: Opening SD card...\n");
    esp_err_t r = odroid_sdcard_open(SD_BASE_PATH);
    if (r != ESP_OK)
    {
        printf("stella-go: SD card open failed (%d)\n", r);
        // --- PROGRESS: RED = SD card failed ---
        ili9341_clear(0xF800);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    // --- PROGRESS: GREEN = SD card OK ---
    ili9341_clear(0x07E0);
    printf("stella-go: SD card opened OK.\n");
    vTaskDelay(200 / portTICK_PERIOD_MS);

    // Try to get ROM path from NVS (set by launcher)
    const char* romfile = odroid_settings_RomFilePath_get();
    if (!romfile || strlen(romfile) < 4)
    {
        // Fallback to file browser if no ROM path in NVS
        ili9341_clear(0x0000);
        printf("stella-go: No ROM path in NVS, entering file browser...\n");
        romfile = ChooseFile();
    }
    printf("%s: filename='%s'\n", __func__, romfile);

    // --- PROGRESS: MAGENTA = ROM selected, loading ---
    ili9341_clear(0xF81F);
    printf("Loading ROM: %s\n", romfile);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    stella_init(romfile);
    printf("stella_init complete, starting emulation.\n");

    odroid_audio_init(AUDIO_SAMPLE_RATE);

    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024 * 4, NULL, 5, NULL, 1);

    // Allocate phosphor blend buffers in SPIRAM (two for double-buffering)
    phosphorBuf[0] = (uint8_t*)heap_caps_malloc(PHOSPHOR_SIZE, MALLOC_CAP_SPIRAM);
    phosphorBuf[1] = (uint8_t*)heap_caps_malloc(PHOSPHOR_SIZE, MALLOC_CAP_SPIRAM);
    if (phosphorBuf[0]) memset(phosphorBuf[0], 0, PHOSPHOR_SIZE);
    if (phosphorBuf[1]) memset(phosphorBuf[1], 0, PHOSPHOR_SIZE);
    printf("Phosphor buffers: %p, %p\n", phosphorBuf[0], phosphorBuf[1]);


    uint startTime;
    uint stopTime;
    uint totalElapsedTime = 0;
    int frame = 0;
    int renderFrames = 0;
    uint16_t muteFrameCount = 0;
    uint16_t powerFrameCount = 0;

    odroid_gamepad_state last_gamepad;
    odroid_input_gamepad_read(&last_gamepad);

    static const bool renderTable[8] = {
        false, true,
        false, true,
        false, true,
        false, true };

    while(1)
    {
        startTime = xthal_get_ccount();


        odroid_gamepad_state gamepad;
        odroid_input_gamepad_read(&gamepad);

        if (last_gamepad.values[ODROID_INPUT_MENU] &&
            !gamepad.values[ODROID_INPUT_MENU])
        {
            // Drain the video queue so the videoTask finishes its
            // current SPI writes and blocks on xQueuePeek.
            uint8_t* dummy;
            while (xQueueReceive(vidQueue, &dummy, 0) == pdTRUE) { }
            // Give videoTask time to finish any in-progress SPI transfer
            vTaskDelay(50 / portTICK_PERIOD_MS);

            int choice = show_game_menu();
            if (choice == 1) {
                // Restart game
                esp_restart();
            } else if (choice == 2) {
                // Quit to launcher
                odroid_system_application_set(0);
                esp_restart();
            }
            // choice 0 = Continue, do nothing
        }

        if (!last_gamepad.values[ODROID_INPUT_VOLUME] &&
            gamepad.values[ODROID_INPUT_VOLUME])
        {
            odroid_audio_volume_change();
            printf("%s: Volume=%d\n", __func__, odroid_audio_volume_get());
        }


        // Always render TIA pixels so the framebuffer has fresh data every frame.
        // Phosphor blending needs every frame's pixels; RenderFlag=false would
        // cause TIA::updateFrame() to skip rendering entirely.
        RenderFlag = true;
        stella_step(&gamepad);
        //printf("stepped.\n");

        // Phosphor blending: accumulate non-black pixels from every frame
        // into the active phosphor buffer. Every 2nd frame, display the
        // accumulated buffer (showing objects from both odd and even frames),
        // then swap to the other buffer and clear it for the next cycle.
        bool pushThisFrame = (frame & 1) == 1;
        {
            TIA& tia = console->tia();
            uint8_t* fb = tia.currentFrameBuffer();
            uint8_t* pbuf = phosphorBuf[phosphorIdx];

            if (pbuf && fb) {
                int visibleSize = STELLA_WIDTH * (IsPal ? 250 : 210);
                for (int i = 0; i < visibleSize; i++) {
                    if (fb[i]) pbuf[i] = fb[i];
                }
            }

            if (pushThisFrame)
            {
                if (pbuf) {
                    xQueueSend(vidQueue, &pbuf, portMAX_DELAY);
                } else {
                    // Fallback: no phosphor buffer, send TIA frame directly
                    xQueueSend(vidQueue, &fb, portMAX_DELAY);
                }
                phosphorIdx ^= 1;
                if (phosphorBuf[phosphorIdx]) memset(phosphorBuf[phosphorIdx], 0, PHOSPHOR_SIZE);

                ++renderFrames;
            }
        }

        last_gamepad = gamepad;


        // end of frame
        stopTime = xthal_get_ccount();


        odroid_battery_state battery;
        odroid_input_battery_level_read(&battery);


        int elapsedTime;
        if (stopTime > startTime)
          elapsedTime = (stopTime - startTime);
        else
          elapsedTime = ((uint64_t)stopTime + (uint64_t)0xffffffff) - (startTime);

        totalElapsedTime += elapsedTime;
        ++frame;

        if (frame == 60)
        {
          float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f);
          float fps = frame / seconds;
          float renderFps = renderFrames / seconds;

          printf("HEAP:0x%x (%#08x), SIM:%f, REN:%f, BATTERY:%d [%d]\n",
            esp_get_free_heap_size(),
            heap_caps_get_free_size(MALLOC_CAP_DMA),
            fps,
            renderFps,
            battery.millivolts,
            battery.percentage);

          frame = 0;
          renderFrames = 0;
          totalElapsedTime = 0;
        }
    }
}
