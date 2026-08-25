#include "screen.h"

#include <coreinit/cache.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/screen.h>
#include <proc_ui/procui.h>

#include <stdarg.h>
#include <stdio.h>

// Tag propio para el frame heap de MEM1 (patrón de libwhb: se marca el estado
// al adquirir el foreground y se libera entero al perderlo).
#define SCREEN_HEAP_TAG 0x57434153  // 'WCAS'

// Rejilla de caracteres de OSScreen. El font es monoespaciado y las
// coordenadas de OSScreenPutFontEx van en caracteres, no en píxeles.
#define TV_COLS  80
#define DRC_COLS 53

static void *s_bufTV, *s_bufDRC;
static uint32_t s_sizeTV, s_sizeDRC;
static BOOL s_hasForeground = FALSE;
static BOOL s_initialized = FALSE;

static uint32_t on_acquired(void *ctx)
{
   MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
   MEMRecordStateForFrmHeap(heap, SCREEN_HEAP_TAG);

   if (s_sizeTV)  s_bufTV  = MEMAllocFromFrmHeapEx(heap, s_sizeTV, 4);
   if (s_sizeDRC) s_bufDRC = MEMAllocFromFrmHeapEx(heap, s_sizeDRC, 4);

   OSScreenSetBufferEx(SCREEN_TV, s_bufTV);
   OSScreenSetBufferEx(SCREEN_DRC, s_bufDRC);
   OSScreenEnableEx(SCREEN_TV, 1);
   OSScreenEnableEx(SCREEN_DRC, 1);

   s_hasForeground = TRUE;
   return 0;
}

static uint32_t on_released(void *ctx)
{
   MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
   MEMFreeByStateToFrmHeap(heap, SCREEN_HEAP_TAG);
   s_bufTV = s_bufDRC = NULL;
   s_hasForeground = FALSE;
   return 0;
}

BOOL screen_init(void)
{
   OSScreenInit();
   s_sizeTV  = OSScreenGetBufferSizeEx(SCREEN_TV);
   s_sizeDRC = OSScreenGetBufferSizeEx(SCREEN_DRC);

   on_acquired(NULL);

   ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, on_acquired, NULL, 100);
   ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, on_released, NULL, 100);

   s_initialized = TRUE;
   return TRUE;
}

void screen_shutdown(void)
{
   if (!s_initialized) return;
   if (s_hasForeground) {
      OSScreenShutdown();
      on_released(NULL);
   }
   s_initialized = FALSE;
}

BOOL screen_has_foreground(void) { return s_hasForeground; }

int screen_cols(ScreenTarget target)
{
   return (target == SCREEN_TARGET_DRC) ? DRC_COLS : TV_COLS;
}

void screen_begin(uint32_t color)
{
   if (!s_hasForeground) return;
   OSScreenClearBufferEx(SCREEN_TV, color);
   OSScreenClearBufferEx(SCREEN_DRC, color);
}

void screen_text(ScreenTarget target, int col, int row, const char *text)
{
   if (!s_hasForeground) return;
   if (target == SCREEN_TARGET_BOTH || target == SCREEN_TARGET_TV) {
      OSScreenPutFontEx(SCREEN_TV, col, row, text);
   }
   if (target == SCREEN_TARGET_BOTH || target == SCREEN_TARGET_DRC) {
      OSScreenPutFontEx(SCREEN_DRC, col, row, text);
   }
}

void screen_textf(ScreenTarget target, int col, int row, const char *fmt, ...)
{
   char line[160];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   screen_text(target, col, row, line);
}

void screen_present(void)
{
   if (!s_hasForeground) return;
   // Los buffers son memoria no cacheada por el scanout: hay que volcar la
   // caché de datos antes de intercambiar, o se ven frames a medio escribir.
   if (s_bufTV)  DCFlushRange(s_bufTV, s_sizeTV);
   if (s_bufDRC) DCFlushRange(s_bufDRC, s_sizeDRC);
   OSScreenFlipBuffersEx(SCREEN_TV);
   OSScreenFlipBuffersEx(SCREEN_DRC);
}
