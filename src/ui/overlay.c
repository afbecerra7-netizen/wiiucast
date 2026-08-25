#include "overlay.h"
#include "font_atlas.h"

#include "video/renderer.h"   // VIDEO_FRAME_PITCH / VIDEO_FRAME_HEIGHT

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int overlay_line_height(int scale) { return FONT_CELL_H * scale; }

int overlay_text_width(const char *text, int scale)
{
   return (int)strlen(text) * FONT_CELL_W * scale;
}

void overlay_clear(uint8_t *nv12, int w, int h, int pitch, uint8_t luma)
{
   int alignedH = VIDEO_FRAME_HEIGHT(h);

   // Plano Y
   for (int row = 0; row < alignedH; row++) {
      memset(nv12 + (size_t)row * pitch, luma, pitch);
   }
   // Plano UV justo detrás: 128 = sin color (gris)
   uint8_t *uv = nv12 + (size_t)pitch * alignedH;
   memset(uv, 128, (size_t)pitch * (alignedH / 2));
}

// Dibuja un glifo copiando su celda del atlas, con el máximo entre el píxel
// existente y el del glifo (así el texto se ve sobre cualquier fondo claro).
static void draw_glyph(uint8_t *nv12, int w, int h, int pitch,
                       int x, int y, int scale, unsigned char ch)
{
   if (ch < 32 || ch > 127) ch = '?';
   int idx = ch - 32;
   int sx = (idx % FONT_COLS) * FONT_CELL_W;
   int sy = (idx / FONT_COLS) * FONT_CELL_H;

   for (int gy = 0; gy < FONT_CELL_H; gy++) {
      for (int sub = 0; sub < scale; sub++) {
         int dy = y + gy * scale + sub;
         if (dy < 0 || dy >= h) continue;
         uint8_t *dstRow = nv12 + (size_t)dy * pitch;
         const uint8_t *srcRow = &FONT_ATLAS[(size_t)(sy + gy) * FONT_ATLAS_W + sx];

         for (int gx = 0; gx < FONT_CELL_W; gx++) {
            uint8_t v = srcRow[gx];
            if (v == 0) continue;
            for (int subx = 0; subx < scale; subx++) {
               int dx = x + gx * scale + subx;
               if (dx < 0 || dx >= w) continue;
               if (v > dstRow[dx]) dstRow[dx] = v;
            }
         }
      }
   }
}

void overlay_text(uint8_t *nv12, int w, int h, int pitch,
                  int x, int y, int scale, const char *text)
{
   if (scale < 1) scale = 1;
   int cx = x;
   for (const char *p = text; *p; p++) {
      if (*p == '\n') { cx = x; y += FONT_CELL_H * scale; continue; }
      draw_glyph(nv12, w, h, pitch, cx, y, scale, (unsigned char)*p);
      cx += FONT_CELL_W * scale;
      if (cx >= w) break;   // no envolver: la línea se corta
   }
}

void overlay_textf(uint8_t *nv12, int w, int h, int pitch,
                   int x, int y, int scale, const char *fmt, ...)
{
   char line[256];
   va_list args;
   va_start(args, fmt);
   vsnprintf(line, sizeof(line), fmt, args);
   va_end(args);
   overlay_text(nv12, w, h, pitch, x, y, scale, line);
}
