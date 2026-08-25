// Presentación de vídeo por GX2: el decoder escribe NV12 directamente en la
// memoria de las texturas (zero-copy) y un shader convierte a RGB al dibujar
// un quad a pantalla completa en la TV y en el GamePad.
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Alineaciones que exige H264DEC sobre el framebuffer de salida.
#define VIDEO_FRAME_PITCH(w)  (((w) + 0xff) & ~0xff)
#define VIDEO_FRAME_HEIGHT(h) (((h) + 0xf) & ~0xf)
#define VIDEO_FRAME_SIZE(w, h) \
   ((VIDEO_FRAME_PITCH(w) * VIDEO_FRAME_HEIGHT(h) * 3) / 2)

BOOL video_renderer_init(void);
void video_renderer_shutdown(void);

// ¿Tenemos la pantalla? Al abrir el menú HOME el sistema nos manda a segundo
// plano y nos quita los scan buffers: dibujar entonces cuelga la consola.
// WHBProcIsRunning() sigue devolviendo TRUE en ese estado, así que hay que
// consultar esto por separado antes de cada frame.
BOOL video_renderer_has_foreground(void);

// Reserva las texturas para un tamaño de vídeo. Puede llamarse otra vez al
// cambiar de medio (libera las anteriores). Devuelve FALSE si no hay memoria.
BOOL video_renderer_set_size(int width, int height);

// Buffer NV12 donde el decoder debe escribir el frame `index`
// (0 <= index < VIDEO_NUM_BUFFERS). Es memoria de textura GX2.
#define VIDEO_NUM_BUFFERS 2
void *video_renderer_framebuffer(int index);

// Marca el buffer `index` como listo para mostrar.
void video_renderer_submit(int index);

// Dibuja el último frame enviado, en TV y GamePad. Llamar una vez por frame.
// Si aún no hay vídeo, pinta el fondo indicado.
void video_renderer_draw(BOOL haveVideo, float bgR, float bgG, float bgB);

int video_renderer_width(void);
int video_renderer_height(void);
