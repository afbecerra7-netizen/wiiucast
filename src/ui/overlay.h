// UI de texto dibujada DENTRO de un frame NV12, para que salga por el mismo
// shader que el vídeo. Así GX2 es dueño único de la pantalla: no hay que
// alternar con OSScreen (que se peleaba por los scan buffers y el frame heap
// de MEM1, y dejaba la TV en negro al volver del cambio).
//
// El texto va al plano Y; el plano UV se deja en 128 (gris neutro), así que
// sale en blanco y negro sin tocar el shader.
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Prepara un frame NV12 de `w`x`h` con fondo `luma` y el plano UV neutro.
void overlay_clear(uint8_t *nv12, int w, int h, int pitch, uint8_t luma);

// Escribe texto en la posición dada, en píxeles, con `scale` entero (1 = 12x24).
void overlay_text(uint8_t *nv12, int w, int h, int pitch,
                  int x, int y, int scale, const char *text);

// Como overlay_text pero con formato.
void overlay_textf(uint8_t *nv12, int w, int h, int pitch,
                   int x, int y, int scale, const char *fmt, ...);

// Ancho en píxeles que ocuparía `text` a esa escala (para centrar).
int overlay_text_width(const char *text, int scale);
int overlay_line_height(int scale);
