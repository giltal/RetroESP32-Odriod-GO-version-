#ifndef ODROID_DISPLAY_EMU_IMPL
void ili9341_write_frame_pcengine_mode0(uint8_t* buffer, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_w224(uint8_t* buffer, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_w256(uint8_t* buffer, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_w320(uint8_t* buffer, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_w336(uint8_t* buffer, uint16_t* pal);

void ili9341_write_frame_pcengine_mode0_scanlines(struct my_scanline* scan, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_scanlines_w224(struct my_scanline* scan, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_scanlines_w256(struct my_scanline* scan, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_scanlines_w320(struct my_scanline* scan, uint16_t* pal);
void ili9341_write_frame_pcengine_mode0_scanlines_w336(struct my_scanline* scan, uint16_t* pal);

#else

#include "../huexpress/includes/cleantypes.h"

#define PCENGINE_GAME_WIDTH  352
#define PCENGINE_GAME_HEIGHT 240
#define PCENGINE_REMOVE_X 16
#define XBUF_WIDTH  (536 + 32 + 32)

extern uchar *Pal;
extern uchar *SPM;

/*
 * 32-bit optimized palette conversion: reads 4 source pixels per iteration
 * via uint32 access, clears framebuffer and SPM with word writes, packs
 * 2 RGB565 palette outputs per uint32 write to line buffer.
 * Reduces SPIRAM cache line fetches and instruction count vs byte-at-a-time.
 * All PCE widths (224/256/320/336) are divisible by 4.
 */
#define PAL_CONVERT_LINE_4X(framePtr, sPtr, line_buffer_ptr, pal, pal0w, WIDTH) \
    do { \
        uint32_t *_fp32 = (uint32_t *)(framePtr); \
        uint32_t *_sp32 = (uint32_t *)(sPtr); \
        uint32_t *_lp32 = (uint32_t *)(line_buffer_ptr); \
        for (short _i = 0; _i < (WIDTH) / 4; _i++) { \
            uint32_t _pix = _fp32[_i]; \
            _fp32[_i] = (pal0w); \
            _sp32[_i] = 0; \
            uint16_t _p0 = (pal)[_pix & 0xFF]; \
            uint16_t _p1 = (pal)[(_pix >> 8) & 0xFF]; \
            uint16_t _p2 = (pal)[(_pix >> 16) & 0xFF]; \
            uint16_t _p3 = (pal)[(_pix >> 24) & 0xFF]; \
            _lp32[_i * 2]     = _p0 | ((uint32_t)_p1 << 16); \
            _lp32[_i * 2 + 1] = _p2 | ((uint32_t)_p3 << 16); \
        } \
        (framePtr) += (WIDTH); (sPtr) += (WIDTH); (line_buffer_ptr) += (WIDTH); \
    } while (0)

void ili9341_write_frame_pcengine_mode0(uint8_t* buffer, uint16_t* pal)
{
    // ili9341_write_frame_rectangleLE(0,0,300,240, buffer -32);
#if 0    
    uint8_t* framePtr = buffer + PCENGINE_REMOVE_X;
    uint8_t *sPtr = SPM;
    short x, y;
    uchar pal0 = Pal[0];
    send_reset_drawing(0, 0, 320, 240);
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)
    {
      uint16_t* line_buffer = line_buffer_get();
      uint16_t* line_buffer_ptr = line_buffer; 
      for (short i = 0; i < 4; ++i) // LINE_COUNT
      {
          //int index = i * displayWidth;
          for (x = 0; x < 320; ++x)
          {
            uint8_t source=*framePtr;
            *framePtr = pal0;
            framePtr++;
            uint16_t value1 = pal[source];
            //line_buffer[index++] = value1;
            *line_buffer_ptr = value1;
            line_buffer_ptr++;
            *sPtr = 0;
            sPtr++;
          }
          framePtr+=280;
      }
      send_continue_line(line_buffer, 320, 4);
    }
    //memset(buffer, Pal[0], 240 * XBUF_WIDTH);
    //memset(SPM, 0, 240 * XBUF_WIDTH);
#endif

    uint8_t* framePtr = buffer + PCENGINE_REMOVE_X;
    uint8_t *sPtr = SPM;
    short x, y;
    uchar pal0 = Pal[0];
    uint32_t pal0w = pal0 * 0x01010101u;
    send_reset_drawing(0, 0, 320, 240);
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)
    {
      uint16_t* line_buffer = line_buffer_get();
      uint16_t* line_buffer_ptr = line_buffer; 
      for (short i = 0; i < 4; ++i) // LINE_COUNT
      {
          PAL_CONVERT_LINE_4X(framePtr, sPtr, line_buffer_ptr, pal, pal0w, 320);
          framePtr+=280;
          sPtr+=280;
      }
      send_continue_line(line_buffer, 320, 4);
    }
    //memset(buffer, Pal[0], 240 * XBUF_WIDTH);
    //memset(SPM, 0, 240 * XBUF_WIDTH);
    
    //#define MISSING ( 240 * XBUF_WIDTH - PCENGINE_GAME_HEIGHT*320)
    //memset(sPtr, 0, MISSING);
}

#define ODROID_DISPLAY_FRAME_RES(FUNC_NAME, WIDTH)           \
void FUNC_NAME(uint8_t* buffer, uint16_t* pal)               \
{                                                            \
    uint8_t* framePtr = buffer ;                             \
    uint8_t *sPtr = SPM;                                     \
    short x, y;                                              \
    uchar pal0 = Pal[0];                                     \
    uint32_t pal0w = pal0 * 0x01010101u;                     \
    send_reset_drawing((320-WIDTH)/2, 0, WIDTH, 240);        \
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)            \
    {                                                        \
      uint16_t* line_buffer = line_buffer_get();             \
      uint16_t* line_buffer_ptr = line_buffer;               \
      for (short i = 0; i < 4; ++i)                          \
      {                                                      \
          PAL_CONVERT_LINE_4X(framePtr, sPtr,                \
              line_buffer_ptr, pal, pal0w, WIDTH);           \
          framePtr+=280 + (320-WIDTH);                       \
          sPtr+=280 + (320-WIDTH);                           \
      }                                                      \
      send_continue_line(line_buffer, WIDTH, 4);             \
    }                                                        \
}

ODROID_DISPLAY_FRAME_RES(ili9341_write_frame_pcengine_mode0_w224, 224)





void ili9341_write_frame_pcengine_mode0_w256(uint8_t* buffer, uint16_t* pal)
{
    uint8_t* framePtr = buffer ;
    uint8_t *sPtr = SPM;
    short x, y;
    uchar pal0 = Pal[0];
    uint32_t pal0w = pal0 * 0x01010101u;
    send_reset_drawing(32, 0, 256, 240);
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)
    {
      uint16_t* line_buffer = line_buffer_get();
      uint16_t* line_buffer_ptr = line_buffer; 
      for (short i = 0; i < 4; ++i) // LINE_COUNT
      {
          PAL_CONVERT_LINE_4X(framePtr, sPtr, line_buffer_ptr, pal, pal0w, 256);
          framePtr+=280 + 64;
          sPtr+=280 + 64;
      }
      send_continue_line(line_buffer, 256, 4);
    }
}

void ili9341_write_frame_pcengine_mode0_w320(uint8_t* buffer, uint16_t* pal)
{
    uint8_t* framePtr = buffer ;
    uint8_t *sPtr = SPM;
    short x, y;
    uchar pal0 = Pal[0];
    uint32_t pal0w = pal0 * 0x01010101u;
    send_reset_drawing(0, 0, 320, 240);
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)
    {
      uint16_t* line_buffer = line_buffer_get();
      uint16_t* line_buffer_ptr = line_buffer; 
      for (short i = 0; i < 4; ++i) // LINE_COUNT
      {
          PAL_CONVERT_LINE_4X(framePtr, sPtr, line_buffer_ptr, pal, pal0w, 320);
          framePtr+=280;
          sPtr+=280;
      }
      send_continue_line(line_buffer, 320, 4);
    }
}

void ili9341_write_frame_pcengine_mode0_w336(uint8_t* buffer, uint16_t* pal)
{
    uint8_t* framePtr = buffer + 8;
    uint8_t *sPtr = SPM;
    short x, y;
    uchar pal0 = Pal[0];
    uint32_t pal0w = pal0 * 0x01010101u;
    send_reset_drawing(0, 0, 320, 240);
    for (y = 0; y < PCENGINE_GAME_HEIGHT; y += 4)
    {
      uint16_t* line_buffer = line_buffer_get();
      uint16_t* line_buffer_ptr = line_buffer; 
      for (short i = 0; i < 4; ++i) // LINE_COUNT
      {
          PAL_CONVERT_LINE_4X(framePtr, sPtr, line_buffer_ptr, pal, pal0w, 320);
          framePtr+=280;
          sPtr+=280;
      }
      send_continue_line(line_buffer, 320, 4);
    }
}

#define ODROID_DISPLAY_FRAME_SCANLINE_RES(FUNC_NAME, WIDTH)        



#define ODROID_DISPLAY_FRAME_SCANLINE_RES2(FUNC_NAME, WIDTH)                    \
void FUNC_NAME(struct my_scanline* scan, uint16_t* pal)                         \
{                                                                                       \
    /* printf("Scanline: %3d-%3d\n", scan->YY1, scan->YY2); */                          \
    uint8_t* framePtr = scan->buffer + scan->YY1 * XBUF_WIDTH + (WIDTH-320)/2;          \
    uint8_t *sPtr = SPM + (XBUF_WIDTH) * (scan->YY1);                                   \
    short x, y;                                                                         \
    uchar pal0 = Pal[0];                                                                \
    send_reset_drawing(0, scan->YY1, 320, scan->YY2);                                   \
    for (y = scan->YY1; y < scan->YY2 - 3; y += 4)                                      \
    {                                                                                   \
      uint16_t* line_buffer = line_buffer_get();                                        \
      uint16_t* line_buffer_ptr = line_buffer;                                          \
      for (short i = 0; i < 4; ++i)                                                     \
      {                                                                                 \
          for (x = 0; x < 320; ++x)                                                     \
          {                                                                             \
            uint8_t source=*framePtr;                                                   \
            *framePtr = pal0;                                                           \
            framePtr++;                                                                 \
            uint16_t value1 = pal[source];                                              \
            *line_buffer_ptr = value1;                                                  \
            line_buffer_ptr++;                                                          \
            *sPtr = 0;                                                                  \
            sPtr++;                                                                     \
          }                                                                             \
          framePtr+=280;                                                                \
          sPtr+=280;                                                                    \
      }                                                                                 \
      send_continue_line(line_buffer, 320, 4);                                          \
    }                                                                                   \
    int last = scan->YY2 - y;                                                           \
    if (last>0)                                                                         \
    {                                                                                   \
    uint16_t* line_buffer = line_buffer_get();                                          \
      uint16_t* line_buffer_ptr = line_buffer;                                          \
      for (short i = 0; i < last; ++i)                                                  \
      {                                                                                 \
          for (x = 0; x < 320; ++x)                                                     \
          {                                                                             \
            uint8_t source=*framePtr;                                                   \
            *framePtr = pal0;                                                           \
            framePtr++;                                                                 \
            uint16_t value1 = pal[source];                                              \
            *line_buffer_ptr = value1;                                                  \
            line_buffer_ptr++;                                                          \
            *sPtr = 0;                                                                  \
            sPtr++;                                                                     \
          }                                                                             \
          framePtr+=280;                                                                \
          sPtr+=280;                                                                    \
      }                                                                                 \
      send_continue_line(line_buffer, 320, last);                                       \
    }                                                                                   \
}

ODROID_DISPLAY_FRAME_SCANLINE_RES2(ili9341_write_frame_pcengine_mode0_scanlines, 352)
ODROID_DISPLAY_FRAME_SCANLINE_RES2(ili9341_write_frame_pcengine_mode0_scanlines_w320, 320)
ODROID_DISPLAY_FRAME_SCANLINE_RES2(ili9341_write_frame_pcengine_mode0_scanlines_w336, 336)

ODROID_DISPLAY_FRAME_SCANLINE_RES2(ili9341_write_frame_pcengine_mode0_scanlines_w224, 224)
ODROID_DISPLAY_FRAME_SCANLINE_RES2(ili9341_write_frame_pcengine_mode0_scanlines_w256, 256)

#endif
