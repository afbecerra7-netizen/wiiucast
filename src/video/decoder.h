// Decodificador H.264 sobre el hardware de la consola (H264DEC).
//
// Se usa en modo BUFFERED (OUTPUT_PER_FRAME=0): el decoder retiene ~5 frames
// y los emite por callback en ORDEN DE PRESENTACIÓN. Eso es lo que permite
// reproducir MP4 normales con B-frames — verificado en hardware, y bit-exacto
// contra ffmpeg (ver spikes/RESULTADOS.md).
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Callback de frame listo. `index` es el buffer que se pasó a decoder_submit,
// `pts` el timestamp de presentación en segundos.
typedef void (*DecoderFrameFn)(int index, double pts, int width, int height,
                               int pitch, void *user);

// profile/level/width/height salen del demuxer. Devuelve FALSE si el stream
// no es compatible (perfil no soportado, sin memoria...).
BOOL decoder_open(int profile, int level, int width, int height,
                  DecoderFrameFn onFrame, void *user);
void decoder_close(void);

// Entrega una unidad de decodificación en Annex-B. `frameBuffer` es donde el
// decoder escribirá NV12 (una textura del renderer), e `index` se pasa tal
// cual al callback. Devuelve FALSE si el decoder rechazó el bitstream.
BOOL decoder_submit(const uint8_t *annexb, uint32_t len, double pts,
                    void *frameBuffer, int index);

// Emite los frames que el decoder retiene (final del medio o antes de seek).
void decoder_flush(void);

uint32_t decoder_frames_out(void);
uint32_t decoder_errors(void);
