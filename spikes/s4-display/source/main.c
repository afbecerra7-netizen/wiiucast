// ============================================================================
// WiiU Cast — Spike S4: la ruta de presentación (la última sin probar)
//
// Pregunta: ¿se ve el vídeo en la TV? S2 demostró que el decoder produce los
// píxeles correctos (bit-exactos contra ffmpeg), pero volcaba a archivo: la
// cadena NV12 -> texturas GX2 -> shader -> pantalla nunca se ha ejecutado.
//
// Usa los módulos REALES del proyecto (src/media, src/video), no copias:
// si esto funciona, la Fase 2 está a un fetcher HTTP de distancia.
//
// Entrada: sd:/wiiucast/test.mp4   ·  Salida: vídeo en TV y GamePad
// Reproduce en bucle a la cadencia del archivo. HOME para salir.
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/sdcard.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/mp4demux.h"
#include "video/decoder.h"
#include "video/renderer.h"

// Cola de frames decodificados esperando su momento de presentación. El
// decoder va por delante del reloj: aquí se guardan hasta que toca mostrarlos.
#define QUEUE_MAX 8
typedef struct { int index; double pts; BOOL used; } QueuedFrame;
static QueuedFrame s_queue[QUEUE_MAX];
static int s_queued;

static uint32_t s_shown;

static void on_frame(void *framebuffer, double pts, int w, int h, int pitch, void *user)
{
   int index = video_renderer_index_of(framebuffer);
   if (index < 0) return;
   if (s_queued >= QUEUE_MAX) return;   // el llamador regula, no debería pasar
   for (int i = 0; i < QUEUE_MAX; i++) {
      if (!s_queue[i].used) {
         s_queue[i].index = index;
         s_queue[i].pts = pts;
         s_queue[i].used = TRUE;
         s_queued++;
         return;
      }
   }
}

// Saca de la cola el frame cuyo PTS ya venció (el más antiguo).
static int take_due_frame(double clock, double *outPts)
{
   int best = -1;
   double bestPts = 0;
   for (int i = 0; i < QUEUE_MAX; i++) {
      if (!s_queue[i].used) continue;
      if (s_queue[i].pts <= clock && (best < 0 || s_queue[i].pts < bestPts)) {
         best = i;
         bestPts = s_queue[i].pts;
      }
   }
   if (best < 0) return -1;
   s_queue[best].used = FALSE;
   s_queued--;
   *outPts = bestPts;
   return s_queue[best].index;
}

// AVCC -> Annex-B (comprobaciones por resta: inmunes a overflow)
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

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogUdpInit();

   WHBLogPrintf("== S4: presentacion GX2 (NV12 -> shader -> TV) ==");

   if (!WHBMountSdCard()) { WHBLogPrintf("FATAL: SD"); goto done; }

   char path[320];
   snprintf(path, sizeof(path), "%s/wiiucast/test.mp4", WHBGetSdCardMountPath());

   Mp4Video vid;
   char err[160];
   if (mp4_parse(path, &vid, err, sizeof(err)) != 0) {
      WHBLogPrintf("FATAL demux: %s", err);
      goto done;
   }
   WHBLogPrintf("[mp4] %dx%d %u samples %.1fs bframes=%d",
                vid.width, vid.height, vid.sampleCount, vid.duration, vid.hasBFrames);

   if (!video_renderer_init()) { WHBLogPrintf("FATAL: renderer"); goto done; }
   if (!video_renderer_set_size(vid.width, vid.height)) {
      WHBLogPrintf("FATAL: texturas");
      goto done;
   }
   if (!decoder_open(vid.profile, vid.level, vid.width, vid.height, on_frame, NULL)) {
      WHBLogPrintf("FATAL: decoder");
      goto done;
   }

   uint32_t bsCap = vid.maxSampleSize + vid.spsPpsSize + 4096;
   uint8_t *bitstream = malloc(bsCap);
   uint8_t *sampleBuf = malloc(vid.maxSampleSize);
   FILE *f = fopen(path, "rb");
   if (!bitstream || !sampleBuf || !f) { WHBLogPrintf("FATAL: buffers"); goto done; }

   uint32_t next = 0;      // siguiente sample a decodificar
   int fbIndex = 0;        // buffer de textura round-robin
   OSTime start = OSGetSystemTime();
   BOOL haveVideo = FALSE;

   WHBLogPrintf("[s4] reproduciendo en bucle; HOME para salir");

   while (WHBProcIsRunning()) {
      double clock = OSTicksToMicroseconds(OSGetSystemTime() - start) / 1e6;

      // Alimentar el decoder mientras quepan frames en la cola de salida.
      // (Los buffers son 2: no se puede ir más de un frame por delante.)
      while (s_queued < VIDEO_NUM_BUFFERS - 1 && next < vid.sampleCount) {
         Mp4Sample *s = &vid.samples[next];
         if (fseek(f, (long)s->offset, SEEK_SET) != 0 ||
             fread(sampleBuf, 1, s->size, f) != s->size) { next++; continue; }

         uint32_t len = 0;
         if (next == 0) { memcpy(bitstream, vid.spsPps, vid.spsPpsSize); len = vid.spsPpsSize; }
         uint32_t conv = avcc_to_annexb(sampleBuf, s->size, bitstream + len,
                                        bsCap - len, vid.nalLengthSize);
         if (conv == 0) { next++; continue; }

         decoder_submit(bitstream, len + conv, s->pts,
                        video_renderer_framebuffer(fbIndex));
         fbIndex = (fbIndex + 1) % VIDEO_NUM_BUFFERS;
         next++;
      }

      // Fin del archivo: vaciar el pipeline y volver a empezar
      if (next >= vid.sampleCount && s_queued == 0) {
         decoder_flush();
         if (s_queued == 0) {
            next = 0;
            start = OSGetSystemTime();
            decoder_close();
            decoder_open(vid.profile, vid.level, vid.width, vid.height, on_frame, NULL);
            WHBLogPrintf("[s4] bucle: %u frames mostrados", s_shown);
         }
      }

      // ¿Toca mostrar alguno?
      double pts;
      int due = take_due_frame(clock, &pts);
      if (due >= 0) {
         video_renderer_submit(due);
         haveVideo = TRUE;
         s_shown++;
         if (s_shown % 60 == 0) {
            WHBLogPrintf("[s4] %u frames | reloj %.1fs | pts %.1fs | cola %d",
                         s_shown, clock, pts, s_queued);
         }
      }

      video_renderer_draw(haveVideo, 0.0f, 0.0f, 0.0f);
   }

   fclose(f);
   free(bitstream);
   free(sampleBuf);
   decoder_close();
   mp4_free(&vid);

done:
   video_renderer_shutdown();
   WHBUnmountSdCard();
   WHBProcShutdown();
   return 0;
}
