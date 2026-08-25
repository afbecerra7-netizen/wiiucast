#include "audio_out.h"

#include <whb/log.h>

#include <coreinit/cache.h>
#include <coreinit/mutex.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sndcore2/drcvs.h>

#include <malloc.h>
#include <string.h>

// ~1.4 s de colchón a 48 kHz. Generoso a propósito: el decodificado de audio
// compite con el de vídeo por la CPU, y quedarse sin muestras se oye.
#define RING_FRAMES 65536

static int s_rate, s_channels;
static BOOL s_ready, s_paused;

// AX quiere los canales por separado, no entrelazados: un anillo por canal.
static int16_t *s_ring[AUDIO_MAX_CHANNELS];
static AXVoice *s_voice[AUDIO_MAX_CHANNELS];

static uint32_t s_writePos;        // frames escritos (módulo RING_FRAMES al usar)
static uint64_t s_written;         // total histórico escrito
static uint64_t s_playedBase;      // frames reproducidos en vueltas completas
static uint32_t s_lastHwOffset;    // para detectar el salto del bucle
static OSMutex s_mutex;
static BOOL s_mutexReady;

// Posición de lectura del hardware, en frames dentro del anillo.
static uint32_t hw_offset(void)
{
   if (!s_voice[0]) return 0;
   AXVoiceOffsets off;
   AXGetVoiceOffsets(s_voice[0], &off);
   return off.currentOffset;
}

// Frames totales reproducidos. El cursor del hardware da vueltas al anillo,
// así que se cuentan las vueltas detectando cuándo retrocede.
static uint64_t played_frames_locked(void)
{
   uint32_t cur = hw_offset();
   if (cur < s_lastHwOffset) s_playedBase += RING_FRAMES;   // dio la vuelta
   s_lastHwOffset = cur;
   return s_playedBase + cur;
}

BOOL audio_out_init(int sampleRate, int channels)
{
   audio_out_shutdown();

   if (channels < 1) channels = 1;
   if (channels > AUDIO_MAX_CHANNELS) channels = AUDIO_MAX_CHANNELS;
   s_rate = sampleRate > 0 ? sampleRate : 48000;
   s_channels = channels;

   if (!s_mutexReady) { OSInitMutex(&s_mutex); s_mutexReady = TRUE; }

   if (!AXIsInit()) {
      AXInitParams params = { .renderer = AX_INIT_RENDERER_48KHZ, .pipeline = 0 };
      AXInitWithParams(&params);
   }

   for (int c = 0; c < s_channels; c++) {
      s_ring[c] = memalign(0x40, RING_FRAMES * sizeof(int16_t));
      if (!s_ring[c]) { WHBLogPrintf("[audio] sin memoria para el anillo"); goto fail; }
      memset(s_ring[c], 0, RING_FRAMES * sizeof(int16_t));
      DCFlushRange(s_ring[c], RING_FRAMES * sizeof(int16_t));

      s_voice[c] = AXAcquireVoice(31, NULL, NULL);
      if (!s_voice[c]) { WHBLogPrintf("[audio] no hay voces AX libres"); goto fail; }

      AXVoiceBegin(s_voice[c]);
      AXSetVoiceType(s_voice[c], 0);

      // Volumen a fondo; el ajuste fino se hará por software si hace falta.
      AXVoiceVeData ve = { .volume = 0x8000, .delta = 0 };
      AXSetVoiceVe(s_voice[c], &ve);

      // Mezcla: este canal a su lado en la TV, y también al GamePad.
      AXVoiceDeviceMixData mix[6];
      memset(mix, 0, sizeof(mix));
      mix[c % 2].bus[0].volume = 0x8000;
      AXSetVoiceDeviceMix(s_voice[c], AX_DEVICE_TYPE_TV, 0, mix);
      AXSetVoiceDeviceMix(s_voice[c], AX_DEVICE_TYPE_DRC, 0, mix);

      // Resampler: el renderer va a 48 kHz; si el medio viene a otra
      // frecuencia, la voz la convierte sola con este ratio.
      AXSetVoiceSrcType(s_voice[c], AX_VOICE_SRC_TYPE_LINEAR);
      AXSetVoiceSrcRatio(s_voice[c], (float)s_rate / 48000.0f);

      AXVoiceOffsets offsets;
      memset(&offsets, 0, sizeof(offsets));
      offsets.dataType       = AX_VOICE_FORMAT_LPCM16;
      offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
      offsets.loopOffset     = 0;
      offsets.endOffset      = RING_FRAMES - 1;
      offsets.currentOffset  = 0;
      offsets.data           = s_ring[c];
      AXSetVoiceOffsets(s_voice[c], &offsets);

      AXSetVoiceState(s_voice[c], AX_VOICE_STATE_PLAYING);
      AXVoiceEnd(s_voice[c]);
   }

   s_writePos = 0;
   s_written = 0;
   s_playedBase = 0;
   s_lastHwOffset = 0;
   s_paused = FALSE;
   s_ready = TRUE;

   WHBLogPrintf("[audio] AX listo: %d Hz, %d canales", s_rate, s_channels);
   return TRUE;

fail:
   audio_out_shutdown();
   return FALSE;
}

void audio_out_shutdown(void)
{
   for (int c = 0; c < AUDIO_MAX_CHANNELS; c++) {
      if (s_voice[c]) {
         AXVoiceBegin(s_voice[c]);
         AXSetVoiceState(s_voice[c], AX_VOICE_STATE_STOPPED);
         AXVoiceEnd(s_voice[c]);
         AXFreeVoice(s_voice[c]);
         s_voice[c] = NULL;
      }
      free(s_ring[c]);
      s_ring[c] = NULL;
   }
   s_ready = FALSE;
}

BOOL audio_out_ready(void) { return s_ready; }

uint32_t audio_out_space(void)
{
   if (!s_ready) return 0;
   OSLockMutex(&s_mutex);
   uint64_t played = played_frames_locked();
   uint64_t queued = (s_written > played) ? (s_written - played) : 0;
   OSUnlockMutex(&s_mutex);
   // Un frame de margen para no alcanzar exactamente el cursor de lectura.
   return (queued >= RING_FRAMES - 1) ? 0 : (uint32_t)(RING_FRAMES - 1 - queued);
}

uint32_t audio_out_write(const int16_t *interleaved, uint32_t frames)
{
   if (!s_ready || frames == 0) return 0;

   uint32_t space = audio_out_space();
   if (frames > space) frames = space;
   if (frames == 0) return 0;

   OSLockMutex(&s_mutex);
   for (uint32_t i = 0; i < frames; i++) {
      uint32_t slot = (s_writePos + i) % RING_FRAMES;
      for (int c = 0; c < s_channels; c++) {
         s_ring[c][slot] = interleaved[i * s_channels + c];
      }
   }
   // El DSP lee esta memoria por DMA: hay que volcar la caché o suena a viejo.
   for (int c = 0; c < s_channels; c++) {
      uint32_t start = s_writePos % RING_FRAMES;
      uint32_t first = RING_FRAMES - start;
      if (frames <= first) {
         DCFlushRange(&s_ring[c][start], frames * sizeof(int16_t));
      } else {
         DCFlushRange(&s_ring[c][start], first * sizeof(int16_t));
         DCFlushRange(&s_ring[c][0], (frames - first) * sizeof(int16_t));
      }
   }
   s_writePos = (s_writePos + frames) % RING_FRAMES;
   s_written += frames;

   // Guarda de silencio por delante: la voz reproduce el anillo EN BUCLE, así
   // que si el decodificador se retrasa el hardware volvería a soltar audio
   // viejo. Dejando ceros justo detrás de lo escrito, un adelanto se oye como
   // silencio en vez de como un eco.
   {
      uint32_t guard = 2048;
      if (guard > RING_FRAMES / 4) guard = RING_FRAMES / 4;
      for (int c = 0; c < s_channels; c++) {
         uint32_t start = s_writePos;
         uint32_t first = RING_FRAMES - start;
         if (guard <= first) {
            memset(&s_ring[c][start], 0, guard * sizeof(int16_t));
            DCFlushRange(&s_ring[c][start], guard * sizeof(int16_t));
         } else {
            memset(&s_ring[c][start], 0, first * sizeof(int16_t));
            memset(&s_ring[c][0], 0, (guard - first) * sizeof(int16_t));
            DCFlushRange(&s_ring[c][start], first * sizeof(int16_t));
            DCFlushRange(&s_ring[c][0], (guard - first) * sizeof(int16_t));
         }
      }
   }
   OSUnlockMutex(&s_mutex);

   return frames;
}

void audio_out_stop(void)
{
   if (!s_ready) return;
   for (int c = 0; c < s_channels; c++) {
      if (!s_voice[c]) continue;
      AXVoiceBegin(s_voice[c]);
      AXSetVoiceState(s_voice[c], AX_VOICE_STATE_STOPPED);
      AXVoiceEnd(s_voice[c]);
   }
   s_paused = TRUE;
}

uint64_t audio_out_played_frames(void)
{
   if (!s_ready) return 0;
   OSLockMutex(&s_mutex);
   uint64_t p = played_frames_locked();
   OSUnlockMutex(&s_mutex);
   // Nunca reportar más de lo escrito: al arrancar el anillo está en silencio
   // y el cursor avanza igual, lo que adelantaría el reloj.
   return (p > s_written) ? s_written : p;
}

double audio_out_clock(void)
{
   if (!s_ready || s_rate <= 0) return 0.0;
   return (double)audio_out_played_frames() / (double)s_rate;
}

uint32_t audio_out_queued_frames(void)
{
   if (!s_ready) return 0;
   OSLockMutex(&s_mutex);
   uint64_t played = played_frames_locked();
   uint64_t queued = (s_written > played) ? (s_written - played) : 0;
   OSUnlockMutex(&s_mutex);
   return (uint32_t)queued;
}

void audio_out_pause(BOOL paused)
{
   if (!s_ready || paused == s_paused) return;
   for (int c = 0; c < s_channels; c++) {
      if (!s_voice[c]) continue;
      AXVoiceBegin(s_voice[c]);
      AXSetVoiceState(s_voice[c],
                      paused ? AX_VOICE_STATE_STOPPED : AX_VOICE_STATE_PLAYING);
      AXVoiceEnd(s_voice[c]);
   }
   s_paused = paused;
}

void audio_out_reset(void)
{
   if (!s_ready) return;
   OSLockMutex(&s_mutex);
   for (int c = 0; c < s_channels; c++) {
      memset(s_ring[c], 0, RING_FRAMES * sizeof(int16_t));
      DCFlushRange(s_ring[c], RING_FRAMES * sizeof(int16_t));
   }
   s_writePos = 0;
   s_written = 0;
   s_playedBase = 0;
   s_lastHwOffset = hw_offset();
   OSUnlockMutex(&s_mutex);
}
