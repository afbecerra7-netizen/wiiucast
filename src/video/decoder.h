// Decodificador H.264 sobre el hardware de la consola (H264DEC).
//
// Se usa en modo BUFFERED (OUTPUT_PER_FRAME=0): el decoder retiene ~5 frames
// y los emite por callback en ORDEN DE PRESENTACIÓN. Eso es lo que permite
// reproducir MP4 normales con B-frames — verificado en hardware, y bit-exacto
// contra ffmpeg (ver spikes/RESULTADOS.md).
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Callback de frame listo. `framebuffer` es la dirección REAL donde el
// decoder dejó el frame — la reporta él mismo en H264DecodeResult, y hay que
// usarla: en modo buffered la salida sale reordenada y no corresponde a la
// llamada a Execute en curso. Suponerlo mostraba fotogramas desordenados.
typedef void (*DecoderFrameFn)(void *framebuffer, double pts,
                               int width, int height, int pitch, void *user);

// profile/level/width/height salen del demuxer. Devuelve FALSE si el stream
// no es compatible (perfil no soportado, sin memoria...).
BOOL decoder_open(int profile, int level, int width, int height,
                  DecoderFrameFn onFrame, void *user);
void decoder_close(void);

// Entrega una unidad de decodificación en Annex-B. `frameBuffer` es donde el
// decoder puede escribir NV12 (una textura del renderer). Qué frame acaba en
// qué buffer lo dice el propio decoder en el callback, no esta llamada.
BOOL decoder_submit(const uint8_t *annexb, uint32_t len, double pts,
                    void *frameBuffer);

// Emite los frames que el decoder retiene (final del medio o antes de seek).
void decoder_flush(void);

uint32_t decoder_frames_out(void);
uint32_t decoder_errors(void);
