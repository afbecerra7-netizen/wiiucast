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

// Cuánto prefijo se copia para parsear el MP4. El moov de un archivo con
// faststart cabe de sobra: el de un 1080p de 11 minutos ocupa ~360 KB.
#define HEADER_PROBE  (4 * 1024 * 1024)

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

   // El medio NO se guarda entero: se lee del buffer circular de red según
   // hace falta, así que la memoria es constante sea cual sea el tamaño del
   // archivo. Solo la cabecera (moov) se copia aparte para poder parsearla.
   uint8_t *headerBuf;
   uint32_t headerLen;

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

   // Rebuffering: si la red no da para el bitrate del medio, es mucho mejor
   // parar y acumular que reproducir a trompicones. Se vuelve a arrancar con
   // REBUFFER_BYTES por delante.
   BOOL rebuffering;
   double rebufferClock;   // reloj donde se paró, para reanudar sin saltar

   // Medición de cadencia real: sin esto no hay forma de distinguir "va a
   // saltos" por decodificación lenta de "va a saltos" por sincronía.
   OSTime fpsWindow;
   uint32_t fpsFrames;
   double fpsValue;

   // Traza de los últimos fotogramas presentados: la única forma de ver
   // desde fuera si salen en orden o dando saltos.
   double lastShown[10];
   int lastShownCount;
} P;

// Margen que se acumula antes de (re)arrancar. A 8 Mbps son ~4 segundos.
#define REBUFFER_BYTES (4u * 1024 * 1024)

void player_set_display_cb(PlayerDisplayFn fn) { P.displayCb = fn; }

static void set_failed(const char *msg)
{
   // Truncar es aceptable: el mensaje es para mostrar, no para procesar.
   snprintf(P.error, sizeof(P.error), "%.*s", (int)sizeof(P.error) - 1, msg);
   P.state = PLAYER_FAILED;
   WHBLogPrintf("[player] %s", msg);
}

// ---------------------------------------------------------------------------
static void on_frame(void *framebuffer, double pts, int w, int h, int pitch, void *user)
{
   (void)w; (void)h; (void)pitch; (void)user;
   int index = video_renderer_index_of(framebuffer);
   if (index < 0) return;   // frame en memoria que no es nuestra: descartar
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

// Copia el prefijo del archivo (donde vive el moov) a un buffer propio, para
// poder parsearlo aunque la ventana de red ya haya avanzado.
static void drain_header(void)
{
   if (!P.headerBuf || P.headerLen >= HEADER_PROBE) return;
   uint32_t avail = fetch_available(P.headerLen);
   if (avail == 0) return;
   if (P.headerLen + avail > HEADER_PROBE) avail = HEADER_PROBE - P.headerLen;
   int got = fetch_read(P.headerLen, P.headerBuf + P.headerLen, avail);
   if (got > 0) P.headerLen += (uint32_t)got;
}

// Libera del buffer circular todo lo anterior al primer byte que aún haga
// falta. Vídeo y audio van entrelazados, así que manda el que va más atrás.
static void release_consumed(void)
{
   uint64_t keep = UINT64_MAX;
   if (P.haveVid && P.nextSample < P.vid.sampleCount) {
      keep = P.vid.samples[P.nextSample].offset;
   }
   if (P.haveAudio && P.nextAudioSample < P.aud.sampleCount) {
      uint64_t a = P.aud.samples[P.nextAudioSample].offset;
      if (a < keep) keep = a;
   }
   if (keep != UINT64_MAX) fetch_release_until(keep);
}

// ¿Está este sample entero dentro de la ventana descargada?
static BOOL sample_available(const Mp4Sample *s)
{
   return fetch_available(s->offset) >= s->size;
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
   free(P.headerBuf);  P.headerBuf = NULL;
   P.headerLen = 0;
   P.nextSample = 0;
   P.queued = 0;
   memset(P.queue, 0, sizeof(P.queue));
}

static void player_stop_internal(BOOL notifyDisplay)
{
   fetch_stop();
   free_playback();
   if (notifyDisplay && P.displayCb) P.displayCb(FALSE);
   P.state = PLAYER_IDLE;
   P.framesShown = 0;
   P.lastPts = 0;
}

void player_stop(void)
{
   player_stop_internal(TRUE);   // devolver la pantalla a la UI
}

void player_shutdown(void)
{
   // Al cerrar la app no se avisa a la pantalla: reasignar texturas mientras
   // el sistema nos está quitando el foreground es justo lo que cuelga.
   player_stop_internal(FALSE);
}

void player_release_hardware(void)
{
   if (P.state == PLAYER_IDLE) return;
   WHBLogPrintf("[player] soltando hardware (perdimos el foreground)");

   // SOLO el hardware, y nada más. Este callback lo llama ProcUI y tiene que
   // volver deprisa: liberar memoria, parar hilos o esperar a la GPU aquí
   // deja la consola colgada. La memoria se recupera al reproducir de nuevo
   // o al cerrar la app.
   decoder_close();        // devuelve el decodificador H264
   aac_decoder_close();    // software, inmediato
   audio_out_shutdown();   // devuelve las voces AX
   P.haveAudio = FALSE;

   // Detener la reproducción sin desmontar nada: el vídeo no puede continuar
   // sin decodificador.
   P.state = PLAYER_ENDED;
   P.queued = 0;
   memset(P.queue, 0, sizeof(P.queue));
}

BOOL player_play_url(const char *url)
{
   player_stop();

   // Solo se reserva sitio para la cabecera: el resto se lee de la ventana
   // de red conforme avanza la reproducción, así que no hay límite de tamaño.
   P.headerBuf = malloc(HEADER_PROBE);
   if (!P.headerBuf) {
      set_failed("sin memoria para la cabecera del video");
      return FALSE;
   }
   P.headerLen = 0;

   if (!fetch_start(url)) {
      set_failed(fetch_error()[0] ? fetch_error() : "no se pudo iniciar la descarga");
      return FALSE;
   }

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
   if (mp4_parse_memory_av(P.headerBuf, P.headerLen, &P.vid, &P.aud,
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
   P.rebuffering = FALSE;
   P.clockStart = OSGetSystemTime();
   P.state = PLAYER_PLAYING;
   WHBLogPrintf("[player] reproduciendo%s", P.haveAudio ? " con audio" : " (mudo)");
   return TRUE;
}

// Decodifica y encola audio mientras quepa en el anillo de salida.
static void feed_audio(void)
{
   if (!P.haveAudio) return;

   while (P.nextAudioSample < P.aud.sampleCount) {
      if (!sample_available(&P.aud.samples[P.nextAudioSample])) break;  // esperando red

      // No decodificar por delante de lo que cabe: un frame AAC son 1024
      // muestras, así que con ese hueco basta.
      if (audio_out_space() < 2048) break;

      Mp4Sample *s = &P.aud.samples[P.nextAudioSample];
      if (fetch_read(s->offset, P.audioSampleBuf, s->size) != (int)s->size) break;

      uint32_t frames = 0;
      const int16_t *pcm = aac_decoder_decode(P.audioSampleBuf, s->size, &frames);
      P.nextAudioSample++;

      if (pcm && frames > 0) audio_out_write(pcm, frames);
   }
}

BOOL player_update(void)
{
   if (P.state == PLAYER_BUFFERING || P.state == PLAYER_PLAYING) {
      if (fetch_state() == FETCH_ERROR) { set_failed(fetch_error()); return FALSE; }
   }

   if (P.state == PLAYER_BUFFERING) {
      drain_header();
      // Se intenta abrir en cuanto haya prefijo suficiente. Si el moov aún no
      // ha llegado entero, mp4_parse_memory_av falla sin ruido y se reintenta
      // en el siguiente frame.
      if (P.headerLen >= 256 * 1024 || fetch_state() == FETCH_DONE) {
         try_open_media();
      }
      return FALSE;
   }

   if (P.state == PLAYER_PAUSED) return P.framesShown > 0;
   if (P.state != PLAYER_PLAYING) return FALSE;

   // Si estamos rellenando, no se reproduce hasta tener margen suficiente.
   if (P.rebuffering) {
      uint64_t needFrom = (P.nextSample < P.vid.sampleCount)
                             ? P.vid.samples[P.nextSample].offset : 0;
      BOOL enough = fetch_available(needFrom) >= REBUFFER_BYTES ||
                    fetch_state() == FETCH_DONE;
      if (!enough) return P.framesShown > 0;   // sigue congelado el último frame

      // Reanudar donde se paró: el reloj de audio no avanzó mientras tanto.
      P.rebuffering = FALSE;
      P.audioDrained = FALSE;
      P.clockStart = OSGetSystemTime() -
                     (OSTime)(P.rebufferClock * (double)OSTimerClockSpeed);
      audio_out_pause(FALSE);
      WHBLogPrintf("[player] reanudando en %.1fs", P.rebufferClock);
   }

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
   // Profundidad de tubería: con margen el decodificado absorbe los picos
   // (fotogramas clave grandes) sin que la presentación se quede seca.
   while (P.queued < 3 && P.nextSample < P.vid.sampleCount) {
      if (!sample_available(&P.vid.samples[P.nextSample])) break;  // esperando red

      Mp4Sample *s = &P.vid.samples[P.nextSample];
      if (fetch_read(s->offset, P.sampleBuf, s->size) != (int)s->size) break;

      uint32_t len = 0;
      if (P.nextSample == 0) {
         memcpy(P.bitstream, P.vid.spsPps, P.vid.spsPpsSize);
         len = P.vid.spsPpsSize;
      }
      uint32_t conv = avcc_to_annexb(P.sampleBuf, s->size, P.bitstream + len,
                                     P.bsCap - len, P.vid.nalLengthSize);
      if (conv == 0) { P.nextSample++; continue; }

      decoder_submit(P.bitstream, len + conv, s->pts,
                     video_renderer_framebuffer(P.fbIndex));
      P.fbIndex = (P.fbIndex + 1) % VIDEO_NUM_BUFFERS;
      P.nextSample++;
   }

   // Fin del medio (el vídeo manda: si el audio dura un pelín más, da igual)
   if (P.nextSample >= P.vid.sampleCount && P.queued == 0) {
      decoder_flush();
      if (P.queued == 0) {
         // Las voces AX reproducen el anillo en bucle: hay que pararlas o
         // seguirían soltando el último trozo de audio indefinidamente.
         audio_out_stop();
         P.state = PLAYER_ENDED;
         WHBLogPrintf("[player] fin: %u frames mostrados", P.framesShown);
         return P.framesShown > 0;
      }
   }

   release_consumed();

   // ¿Nos hemos quedado sin datos? Parar y acumular en vez de dar tirones.
   if (!P.rebuffering && P.nextSample < P.vid.sampleCount &&
       fetch_state() != FETCH_DONE &&
       !sample_available(&P.vid.samples[P.nextSample]) && P.queued == 0) {
      P.rebuffering = TRUE;
      P.rebufferClock = clock;
      audio_out_pause(TRUE);
      WHBLogPrintf("[player] rebuffering en %.1fs (la red no da para el bitrate)", clock);
      return P.framesShown > 0;
   }

   double pts;
   int due = take_due_frame(clock, &pts);
   if (due >= 0) {
      video_renderer_submit(due);
      P.framesShown++;
      P.fpsFrames++;
      P.lastPts = pts;

      for (int i = 9; i > 0; i--) P.lastShown[i] = P.lastShown[i - 1];
      P.lastShown[0] = pts;
      if (P.lastShownCount < 10) P.lastShownCount++;
   }

   // Ventana de 1 s para la cadencia observada
   if (P.fpsWindow == 0) P.fpsWindow = OSGetSystemTime();
   {
      double win = OSTicksToMicroseconds(OSGetSystemTime() - P.fpsWindow) / 1e6;
      if (win >= 1.0) {
         P.fpsValue = P.fpsFrames / win;
         P.fpsFrames = 0;
         P.fpsWindow = OSGetSystemTime();
      }
   }

   return P.framesShown > 0;
}

PlayerState player_state(void)      { return P.state; }
const char *player_error(void)      { return P.error; }
double player_position(void)        { return P.lastPts; }
double player_duration(void)        { return P.haveVid ? P.vid.duration : 0.0; }
uint32_t player_frames_shown(void)  { return P.framesShown; }

double player_mbps(void) { return fetch_mbps(); }
BOOL player_is_rebuffering(void) { return P.rebuffering; }
double player_fps(void) { return P.fpsValue; }

// Rellena `out` con los PTS de los últimos fotogramas mostrados, del más
// reciente al más antiguo. Devuelve cuántos.
int player_recent_pts(double *out, int max)
{
   int n = P.lastShownCount < max ? P.lastShownCount : max;
   for (int i = 0; i < n; i++) out[i] = P.lastShown[i];
   return n;
}

int player_progress_pct(void)
{
   uint64_t total = fetch_total_size();
   if (total == 0) return -1;
   return (int)((fetch_downloaded() * 100) / total);
}
