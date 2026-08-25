// Salida de audio por AX (sndcore2), a la TV y al altavoz del GamePad.
//
// Modelo: una voz AX por canal, cada una reproduciendo en bucle un anillo de
// PCM 16-bit. El hardware avanza su cursor de lectura solo; nosotros vamos
// escribiendo por delante. La posición de ese cursor es además el reloj más
// fiable que tenemos, así que de ahí sale la sincronía del vídeo.
#pragma once
#include <wut_types.h>
#include <stdint.h>

#define AUDIO_MAX_CHANNELS 2

// El hardware renderiza a 48 kHz; a otras frecuencias se ajusta el ratio del
// resampler de la propia voz.
BOOL audio_out_init(int sampleRate, int channels);
void audio_out_shutdown(void);
BOOL audio_out_ready(void);

// Cuántos frames (muestras por canal) caben ahora mismo sin pisar lo que aún
// no ha sonado.
uint32_t audio_out_space(void);

// Encola PCM entrelazado de 16 bits con signo. Devuelve los frames escritos.
uint32_t audio_out_write(const int16_t *interleaved, uint32_t frames);

// Frames ya reproducidos desde el arranque: el reloj maestro.
uint64_t audio_out_played_frames(void);
double audio_out_clock(void);      // segundos reproducidos

void audio_out_pause(BOOL paused);
void audio_out_reset(void);        // vaciar el anillo (stop / seek)
