#include "player.h"
#include "mp4demux.h"

#include "audio/aac_decoder.h"
#include "audio/audio_out.h"
#include "net/http_fetch.h"
#include "video/decoder.h"
#include "video/renderer.h"

#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

// Cuánto hay que tener descargado antes de intentar parsear el MP4. El moov
// de un archivo con faststart cabe de sobra en 1 MiB.
#define HEADER_BYTES  (1024 * 1024)

// Frames decodificados esperando su turno de presentación.
#define QUEUE_MAX 8

typedef struct { int index; double pts; BOOL used; } QueuedFrame;

static struct {
   PlayerState state;
   char error[160];

   Mp4Video vid;
   BOOL haveVid;

   Mp4Audio aud;
   BOOL haveAudio;         // hay pista de audio Y se pudo abrir el decoder
   uint32_t nextAudioSample;
   uint8_t *audioSampleBuf;

   uint8_t *fileBuf;      // copia local del archivo conforme llega
   uint64_t fileCap, fileLen;

   uint8_t *bitstream, *sampleBuf;
   uint32_t bsCap;

   uint32_t nextSample;
   int fbIndex;

   QueuedFrame queue[QUEUE_MAX];
   int queued;

   OSTime clockStart;
   double pausedAt;
   uint32_t framesShown;
   double lastPts;
   PlayerDisplayFn displayCb;

   // Cuando el audio se agota (final del medio) su reloj deja de avanzar,
   // así que se ancla al reloj del sistema para que el vídeo termine.
   BOOL audioDrained;
   OSTime drainAnchor;
   double drainClock;
} P;

void player_set_display_cb(PlayerDisplayFn fn) { P.displayCb = fn; }

static void set_failed(const char *msg)
{
   // Truncar es aceptable: el mensaje es para mostrar, no para procesar.
   snprintf(P.error, sizeof(P.error), "%.*s", (int)sizeof(P.error) - 1, msg);
   P.state = PLAYER_FAILED;
   WHBLogPrintf("[player] %s", msg);
}

// ---------------------------------------------------------------------------
static void on_frame(int index, double pts, int w, int h, int pitch, void *user)
{
   (void)w; (void)h; (void)pitch; (void)user;
   for (int i = 0; i < QUEUE_MAX; i++) {
      if (!P.queue[i].used) {
         P.queue[i].index = index;
         P.queue[i].pts = pts;
         P.queue[i].used = TRUE;
         P.queued++;
         return;
      }
   }
}

static int take_due_frame(double clock, double *outPts)
{
   int best = -1;
   double bestPts = 0;
   for (int i = 0; i < QUEUE_MAX; i++) {
      if (!P.queue[i].used) continue;
      if (P.queue[i].pts <= clock && (best < 0 || P.queue[i].pts < bestPts)) {
         best = i;
         bestPts = P.queue[i].pts;
      }
   }
   if (best < 0) return -1;
   P.queue[best].used = FALSE;
   P.queued--;
   *outPts = bestPts;
   return P.queue[best].index;
}

static uint32_t avcc_to_annexb(const uint8_t *src, uint32_t srcLen,
                               uint8_t *dst, uint32_t dstCap, int nalLenSize)
{
   static const uint8_t startCode[4] = { 0, 0, 0, 1 };
   uint32_t in = 0, out = 0;
   while (srcLen - in >= (uint32_t)nalLenSize && in < srcLen) {
      uint32_t nalLen = 0;
      for (int b = 0; b < nalLenSize; b++) nalLen = (nalLen << 8) | src[in + b];
      in += nalLenSize;
      if (nalLen == 0 || nalLen > srcLen - in) return 0;
      if (dstCap - out < 4 || nalLen > dstCap - out - 4) return 0;
      memcpy(dst + out, startCode, 4); out += 4;
      memcpy(dst + out, src + in, nalLen); out += nalLen;
      in += nalLen;
   }
   return (in == srcLen) ? out : 0;
}

// Copia lo que haya llegado por red al buffer local del archivo.
static void drain_network(void)
{
   for (;;) {
      uint32_t avail = fetch_available(P.fileLen);
      if (avail == 0) return;
      if (P.fileLen + avail > P.fileCap) avail = (uint32_t)(P.fileCap - P.fileLen);
      if (avail == 0) return;   // buffer local lleno (archivo mayor del previsto)

      int got = fetch_read(P.fileLen, P.fileBuf + P.fileLen, avail);
      if (got <= 0) return;
      P.fileLen += (uint32_t)got;
      fetch_release_until(P.fileLen);
   }
}

// ---------------------------------------------------------------------------
BOOL player_init(void)
{
   memset(&P, 0, sizeof(P));
   P.state = PLAYER_IDLE;
   return TRUE;
}

static void free_playback(void)
{
   decoder_close();
   aac_decoder_close();
   audio_out_shutdown();
   if (P.haveVid) { mp4_free(&P.vid); P.haveVid = FALSE; }
   mp4_free_audio(&P.aud);
   P.haveAudio = FALSE;
   P.nextAudioSample = 0;
   free(P.audioSampleBuf); P.audioSampleBuf = NULL;
   free(P.bitstream);  P.bitstream = NULL;
   free(P.sampleBuf);  P.sampleBuf = NULL;
   free(P.fileBuf);    P.fileBuf = NULL;
   P.fileCap = P.fileLen = 0;
   P.nextSample = 0;
   P.queued = 0;
   memset(P.queue, 0, sizeof(P.queue));
}

void player_stop(void)
{
   fetch_stop();
   free_playback();
   if (P.displayCb) P.displayCb(FALSE);   // devolver la pantalla a la UI
   P.state = PLAYER_IDLE;
   P.framesShown = 0;
   P.lastPts = 0;
}

void player_shutdown(void)
{
   player_stop();
}

BOOL player_play_url(const char *url)
{
   player_stop();

   // Techo del buffer local. 256 MiB cubre de sobra un clip razonable; lo
   // que no quepa se corta con un mensaje claro en vez de corromper memoria.
   P.fileCap = 256u * 1024 * 1024;
   uint64_t total = 0;

   if (!fetch_start(url)) {
      set_failed(fetch_error()[0] ? fetch_error() : "no se pudo iniciar la descarga");
      return FALSE;
   }

   // El tamaño real llega con las cabeceras; se ajusta el buffer al saberlo.
   (void)total;
   P.fileBuf = NULL;
   P.fileLen = 0;
   P.state = PLAYER_BUFFERING;
   P.framesShown = 0;
   WHBLogPrintf("[player] buffering: %s", url);
   return TRUE;
}

void player_toggle_pause(void)
{
   if (P.state == PLAYER_PLAYING) {
      P.pausedAt = OSTicksToMicroseconds(OSGetSystemTime() - P.clockStart) / 1e6;
      audio_out_pause(TRUE);
      P.state = PLAYER_PAUSED;
   } else if (P.state == PLAYER_PAUSED) {
      // Reanudar: recolocar el origen del reloj para no saltar hacia adelante.
      // Con audio no hace falta: su cursor se quedó parado donde estaba.
      P.clockStart = OSGetSystemTime() -
                     (OSTime)(P.pausedAt * (double)OSTimerClockSpeed);
      audio_out_pause(FALSE);
      P.state = PLAYER_PLAYING;
   }
}

// Intenta abrir el medio con lo descargado hasta ahora.
static BOOL try_open_media(void)
{
   char err[160];
   if (mp4_parse_memory_av(P.fileBuf, (uint32_t)P.fileLen, &P.vid, &P.aud,
                           err, sizeof(err)) != 0) {
      // Puede ser que aún falte el moov: seguir esperando salvo que ya esté todo
      if (fetch_state() == FETCH_DONE) set_failed(err);
      return FALSE;
   }
   P.haveVid = TRUE;

   WHBLogPrintf("[player] %dx%d %u frames %.1fs bframes=%d",
                P.vid.width, P.vid.height, P.vid.sampleCount,
                P.vid.duration, P.vid.hasBFrames);

   // La salida de vídeo tiene que estar viva ANTES de crear las texturas.
   if (P.displayCb && !P.displayCb(TRUE)) {
      set_failed("no se pudo activar la salida de video (GX2)");
      return FALSE;
   }

   if (!video_renderer_set_size(P.vid.width, P.vid.height)) {
      set_failed("sin memoria para las texturas de video");
      return FALSE;
   }
   if (!decoder_open(P.vid.profile, P.vid.level, P.vid.width, P.vid.height,
                     on_frame, NULL)) {
      set_failed("el decodificador rechazo el video (¿no es H.264 compatible?)");
      return FALSE;
   }

   P.bsCap = P.vid.maxSampleSize + P.vid.spsPpsSize + 4096;
   P.bitstream = malloc(P.bsCap);
   P.sampleBuf = malloc(P.vid.maxSampleSize);
   if (!P.bitstream || !P.sampleBuf) {
      set_failed("sin memoria para los buffers de video");
      return FALSE;
   }

   // --- Audio (opcional: si falla, el vídeo se reproduce mudo)
   if (P.aud.codec == MP4_AUDIO_AAC && P.aud.sampleCount > 0) {
      int rate = 0, channels = 0;
      if (aac_decoder_open(P.aud.asc, P.aud.ascSize, &rate, &channels) &&
          audio_out_init(rate, channels)) {
         P.audioSampleBuf = malloc(P.aud.maxSampleSize);
         if (P.audioSampleBuf) {
            P.haveAudio = TRUE;
            WHBLogPrintf("[player] audio AAC %d Hz %d ch, %u frames",
                         rate, channels, P.aud.sampleCount);
         }
      }
      if (!P.haveAudio) {
         WHBLogPrintf("[player] audio no disponible; reproduccion muda");
         aac_decoder_close();
         audio_out_shutdown();
      }
   }

   P.nextSample = 0;
   P.nextAudioSample = 0;
   P.fbIndex = 0;
   P.audioDrained = FALSE;
   P.clockStart = OSGetSystemTime();
   P.state = PLAYER_PLAYING;
   WHBLogPrintf("[player] reproduciendo%s", P.haveAudio ? " con audio" : " (mudo)");
   return TRUE;
}

// ¿Está el sample de audio `i` descargado?
static BOOL audio_sample_ready(uint32_t i)
{
   Mp4Sample *s = &P.aud.samples[i];
   return (s->offset + s->size) <= P.fileLen;
}

// Decodifica y encola audio mientras quepa en el anillo de salida.
static void feed_audio(void)
{
   if (!P.haveAudio) return;

   while (P.nextAudioSample < P.aud.sampleCount) {
      if (!audio_sample_ready(P.nextAudioSample)) break;   // esperando red

      // No decodificar por delante de lo que cabe: un frame AAC son 1024
      // muestras, así que con ese hueco basta.
      if (audio_out_space() < 2048) break;

      Mp4Sample *s = &P.aud.samples[P.nextAudioSample];
      memcpy(P.audioSampleBuf, P.fileBuf + s->offset, s->size);

      uint32_t frames = 0;
      const int16_t *pcm = aac_decoder_decode(P.audioSampleBuf, s->size, &frames);
      P.nextAudioSample++;

      if (pcm && frames > 0) audio_out_write(pcm, frames);
   }
}

// ¿Está el sample `i` completamente descargado?
static BOOL sample_ready(uint32_t i)
{
   Mp4Sample *s = &P.vid.samples[i];
   return (s->offset + s->size) <= P.fileLen;
}

BOOL player_update(void)
{
   // Asignar el buffer local en cuanto sepamos el tamaño del archivo
   if (!P.fileBuf && (P.state == PLAYER_BUFFERING || P.state == PLAYER_PLAYING)) {
      uint64_t total = fetch_total_size();
      if (total > 0) {
         if (total > P.fileCap) {
            set_failed("archivo demasiado grande (mas de 256 MB)");
            return FALSE;
         }
         P.fileCap = total;
         P.fileBuf = malloc((size_t)total);
         if (!P.fileBuf) { set_failed("sin memoria para el archivo"); return FALSE; }
      } else if (fetch_state() == FETCH_ERROR) {
         set_failed(fetch_error());
         return FALSE;
      } else {
         return FALSE;   // aún sin cabeceras
      }
   }

   if (P.state == PLAYER_BUFFERING || P.state == PLAYER_PLAYING) {
      drain_network();
      if (fetch_state() == FETCH_ERROR) { set_failed(fetch_error()); return FALSE; }
   }

   if (P.state == PLAYER_BUFFERING) {
      if (P.fileLen >= HEADER_BYTES || fetch_state() == FETCH_DONE) {
         try_open_media();
      }
      return FALSE;
   }

   if (P.state == PLAYER_PAUSED) return P.framesShown > 0;
   if (P.state != PLAYER_PLAYING) return FALSE;

   feed_audio();

   // Reloj maestro: si hay audio, manda él. Un salto de audio se oye; un
   // frame de vídeo repetido o descartado, no. Sin audio, reloj del sistema.
   //
   // Salvedad importante: al acabarse las muestras de audio el reloj se
   // quedaría clavado y el vídeo congelado (con el anillo aún sonando), así
   // que al agotarse se pasa el testigo al reloj del sistema desde ese punto.
   double clock;
   if (!P.haveAudio) {
      clock = OSTicksToMicroseconds(OSGetSystemTime() - P.clockStart) / 1e6;
   } else if (P.audioDrained) {
      clock = P.drainClock +
              OSTicksToMicroseconds(OSGetSystemTime() - P.drainAnchor) / 1e6;
   } else {
      clock = audio_out_clock();
      if (P.nextAudioSample >= P.aud.sampleCount && audio_out_queued_frames() == 0) {
         P.audioDrained = TRUE;
         P.drainAnchor = OSGetSystemTime();
         P.drainClock = clock;
         WHBLogPrintf("[player] audio agotado en %.2fs; sigo con reloj del sistema", clock);
      }
   }

   // Alimentar el decoder con lo que ya esté descargado
   while (P.queued < VIDEO_NUM_BUFFERS - 1 && P.nextSample < P.vid.sampleCount) {
      if (!sample_ready(P.nextSample)) break;   // esperando a la red

      Mp4Sample *s = &P.vid.samples[P.nextSample];
      memcpy(P.sampleBuf, P.fileBuf + s->offset, s->size);

      uint32_t len = 0;
      if (P.nextSample == 0) {
         memcpy(P.bitstream, P.vid.spsPps, P.vid.spsPpsSize);
         len = P.vid.spsPpsSize;
      }
      uint32_t conv = avcc_to_annexb(P.sampleBuf, s->size, P.bitstream + len,
                                     P.bsCap - len, P.vid.nalLengthSize);
      if (conv == 0) { P.nextSample++; continue; }

      decoder_submit(P.bitstream, len + conv, s->pts,
                     video_renderer_framebuffer(P.fbIndex), P.fbIndex);
      P.fbIndex = (P.fbIndex + 1) % VIDEO_NUM_BUFFERS;
      P.nextSample++;
   }

   // Fin del medio (el vídeo manda: si el audio dura un pelín más, da igual)
   if (P.nextSample >= P.vid.sampleCount && P.queued == 0) {
      decoder_flush();
      if (P.queued == 0) {
         P.state = PLAYER_ENDED;
         WHBLogPrintf("[player] fin: %u frames mostrados", P.framesShown);
         return P.framesShown > 0;
      }
   }

   double pts;
   int due = take_due_frame(clock, &pts);
   if (due >= 0) {
      video_renderer_submit(due);
      P.framesShown++;
      P.lastPts = pts;
   }

   return P.framesShown > 0;
}

PlayerState player_state(void)      { return P.state; }
const char *player_error(void)      { return P.error; }
double player_position(void)        { return P.lastPts; }
double player_duration(void)        { return P.haveVid ? P.vid.duration : 0.0; }
uint32_t player_frames_shown(void)  { return P.framesShown; }

int player_progress_pct(void)
{
   uint64_t total = fetch_total_size();
   if (total == 0) return -1;
   return (int)((fetch_downloaded() * 100) / total);
}
