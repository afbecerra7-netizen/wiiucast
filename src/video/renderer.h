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

// Buffers NV12 donde el decoder escribe. Tienen que ser MÁS que los frames
// que el decoder retiene en modo buffered (~5), o reutiliza uno que todavía
// está en la cola de presentación y se ven fotogramas desordenados.
// Cuenta: el decoder retiene ~5 fotogramas en modo buffered y la cola de
// presentación admite 3, o sea 8 vivos a la vez. Con 12 hay margen de sobra
// para que ninguno se reutilice antes de mostrarse.
#define VIDEO_NUM_BUFFERS 12
void *video_renderer_framebuffer(int index);

// Índice del buffer cuya memoria empieza en `ptr`, o -1 si no es ninguno.
// Hace falta porque el decoder identifica sus frames por dirección.
int video_renderer_index_of(const void *ptr);

// Marca el buffer `index` como listo para mostrar.
void video_renderer_submit(int index);

// Dibuja el último frame enviado, en TV y GamePad. Llamar una vez por frame.
// Si aún no hay vídeo, pinta el fondo indicado.
void video_renderer_draw(BOOL haveVideo, float bgR, float bgG, float bgB);

int video_renderer_width(void);
int video_renderer_height(void);
