// ============================================================================
// WiiU Cast — Spike S2: decode de archivos MP4 reales (condición C2 del GO)
//
// v2: decodifica en secuencia sd:/wiiucast/test.mp4 (720p) y, si existe,
// sd:/wiiucast/test-1080.mp4 — mismo pipeline, dos veredictos.
//
// Prueba lo que ningún homebrew había probado: H264DEC en MODO BUFFERED
// (OUTPUT_PER_FRAME=0) con MP4s normales de teléfono — B-frames, framing
// AVCC — midiendo ms por Execute y verificando que la salida llega en orden
// de presentación. Demux propio (mp4demux.c), sin FFmpeg.
//
// RESULTADO EN HARDWARE (2026-08-24, build v1, 720p30 con -bf 3):
//   1800/1800 frames, 0 fuera de orden, 0 errores, avg 9.6 ms/frame
//   -> C2 CONFIRMADA a 720p30 con margen 3.5x. Esta v2 mide el techo 1080p.
//
// El callback corre SÍNCRONO dentro de H264DECExecute: ahí no se loguea ni
// se escribe a SD; solo copias a memoria, y su coste se mide y descuenta.
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/sdcard.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <h264/decode.h>
#include <h264/stream.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/mp4demux.h"

#define H264_MEM_ALIGNMENT   0x400
#define FRAME_PITCH(w)       (((w) + 0xff) & ~0xff)
#define FRAME_HEIGHT(h)      (((h) + 0xf) & ~0xf)
#define FRAME_SIZE(w, h)     ((FRAME_PITCH(w) * FRAME_HEIGHT(h) * 3) / 2)

// El modo buffered retiene ~5 frames dentro del decoder; 8 buffers en
// round-robin dan margen suficiente.
#define NUM_FRAMEBUFFERS 8

#define PTS_LOG_COUNT 12   // primeros PTS de entrada y salida a comparar
#define NUM_DUMPS     3    // frames a volcar como PGM
static const uint32_t DUMP_FRAMES[NUM_DUMPS] = { 0, 30, 60 };

static char g_sdBase[256];
static const char *g_label = "";

// ---------------------------------------------------------------------------
// Estado de la salida del decoder. El callback SOLO escribe aquí.
// ---------------------------------------------------------------------------
typedef struct {
   uint8_t *y;              // copia del plano Y (empaquetado width*height)
   size_t cap;              // bytes asignados en y
   int32_t width, height;
   double pts;
   uint32_t frameIndex;
   int valid;
} DumpSlot;

static struct {
   uint32_t framesOut;
   uint32_t outOfOrder;             // PTS de salida no monótono => reordenado MAL
   double lastPts;
   double firstOutPts[PTS_LOG_COUNT];
   int32_t firstStatus;
   int32_t lastWidth, lastHeight, lastPitch;
   DumpSlot dumps[NUM_DUMPS];
   uint64_t cbTicksLast;            // ticks del callback en el Execute en curso
} g_out;

static void frame_callback(H264DecodeOutput *output)
{
   OSTime cb0 = OSGetSystemTime();

   for (int32_t i = 0; i < output->frameCount; i++) {
      H264DecodeResult *r = output->decodeResults[i];
      uint32_t idx = g_out.framesOut;

      if (idx == 0) g_out.firstStatus = r->status;
      if (idx < PTS_LOG_COUNT) g_out.firstOutPts[idx] = r->timestamp;

      if (idx > 0 && r->timestamp < g_out.lastPts) g_out.outOfOrder++;
      g_out.lastPts = r->timestamp;
      g_out.lastWidth = r->width;
      g_out.lastHeight = r->height;
      g_out.lastPitch = r->nextLine;

      for (int d = 0; d < NUM_DUMPS; d++) {
         DumpSlot *s = &g_out.dumps[d];
         if (!s->valid && s->y && idx == DUMP_FRAMES[d] && r->framebuffer &&
             r->width > 0 && r->height > 0 &&
             (size_t)r->width * (size_t)r->height <= s->cap) {
            const uint8_t *src = (const uint8_t *)r->framebuffer;
            for (int32_t row = 0; row < r->height; row++) {
               memcpy(s->y + (size_t)row * r->width,
                      src + (size_t)row * r->nextLine, r->width);
            }
            s->width = r->width;
            s->height = r->height;
            s->pts = r->timestamp;
            s->frameIndex = idx;
            s->valid = 1;
         }
      }

      g_out.framesOut++;
   }

   g_out.cbTicksLast += (uint64_t)(OSGetSystemTime() - cb0);
}

// ---------------------------------------------------------------------------
static void probe_levels(int profile)
{
   static const int levels[] = { 41, 42, 50, 51 };
   WHBLogPrintf("[mem] H264DECMemoryRequirement(profile=%d, 1920x1088):", profile);
   for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
      uint32_t req = 0;
      H264Error err = H264DECMemoryRequirement(profile, levels[i], 1920, 1088, &req);
      WHBLogPrintf("[mem]   nivel %d.%d -> err=0x%X req=%u bytes (%.1f MB)",
                   levels[i] / 10, levels[i] % 10, (unsigned)err,
                   req, req / (1024.0 * 1024.0));
   }
}

// ---------------------------------------------------------------------------
// AVCC -> Annex-B (comprobaciones por resta: inmune a overflow de uint32)
// ---------------------------------------------------------------------------
static uint32_t avcc_to_annexb(const uint8_t *src, uint32_t srcLen,
                               uint8_t *dst, uint32_t dstCap, int nalLenSize)
{
   static const uint8_t startCode[4] = { 0, 0, 0, 1 };
   uint32_t in = 0, out = 0;

   while (srcLen - in >= (uint32_t)nalLenSize && in < srcLen) {
      uint32_t nalLen = 0;
      for (int b = 0; b < nalLenSize; b++) {
         nalLen = (nalLen << 8) | src[in + b];
      }
      in += nalLenSize;
      if (nalLen == 0 || nalLen > srcLen - in) return 0;
      if (dstCap - out < 4 || nalLen > dstCap - out - 4) return 0;

      memcpy(dst + out, startCode, 4); out += 4;
      memcpy(dst + out, src + in, nalLen); out += nalLen;
      in += nalLen;
   }
   return (in == srcLen) ? out : 0;
}

static void write_pgm_dumps(void)
{
   for (int d = 0; d < NUM_DUMPS; d++) {
      DumpSlot *s = &g_out.dumps[d];
      if (!s->valid) continue;

      char path[360];
      snprintf(path, sizeof(path), "%s/wiiucast/s2-%s-frame-%u.pgm",
               g_sdBase, g_label, s->frameIndex);
      FILE *f = fopen(path, "wb");
      if (!f) {
         WHBLogPrintf("[pgm] no pude crear %s", path);
         continue;
      }
      fprintf(f, "P5\n%d %d\n255\n", (int)s->width, (int)s->height);
      fwrite(s->y, 1, (size_t)s->width * s->height, f);
      fclose(f);
      WHBLogPrintf("[pgm] frame %u -> s2-%s-frame-%u.pgm (%dx%d)",
                   s->frameIndex, g_label, s->frameIndex, (int)s->width, (int)s->height);
   }
}

// ---------------------------------------------------------------------------
// Prueba completa de un archivo. Devuelve en cuanto algo falla (los leaks en
// rutas de error son aceptables en un spike; la ruta de éxito libera todo).
// ---------------------------------------------------------------------------
#define FATAL(...) do { WHBLogPrintf(__VA_ARGS__); WHBLogConsoleDraw(); return; } while (0)
#define CHECK(call)                                                    \
   do {                                                                \
      H264Error e_ = (call);                                           \
      if (e_ != H264_ERROR_OK) {                                       \
         FATAL("FATAL: %s -> 0x%X", #call, (unsigned)e_);              \
      }                                                                \
   } while (0)

static void decode_file(const char *mp4Path, const char *label)
{
   // ¿Existe el archivo? (el de 1080 es opcional)
   FILE *probe = fopen(mp4Path, "rb");
   if (!probe) {
      WHBLogPrintf("[%s] %s no existe — salto", label, mp4Path);
      return;
   }
   fclose(probe);

   g_label = label;
   memset(&g_out, 0, sizeof(g_out));

   WHBLogPrintf("==== prueba %s: %s ====", label, mp4Path);

   // ---- 1. Demux
   Mp4Video vid;
   char err[160];
   if (mp4_parse(mp4Path, &vid, err, sizeof(err)) != 0) {
      FATAL("FATAL demux: %s", err);
   }

   WHBLogPrintf("[mp4] %dx%d profile=%d nivel=%d.%d nalLen=%d",
                vid.width, vid.height, vid.profile,
                vid.level / 10, vid.level % 10, vid.nalLengthSize);
   WHBLogPrintf("[mp4] %u samples, %.1f s, maxSample=%u KB, B-frames: %s",
                vid.sampleCount, vid.duration, vid.maxSampleSize / 1024,
                vid.hasBFrames ? "SI" : "NO (regenera con -bf 3!)");
   WHBLogConsoleDraw();

   // ---- 2. Decoder en MODO BUFFERED
   int level = vid.level;
   uint32_t memReq = 0;
   H264Error herr = H264DECMemoryRequirement(vid.profile, level,
                                             vid.width, vid.height, &memReq);
   if (herr != H264_ERROR_OK) {
      WHBLogPrintf("[dec] MemoryRequirement(nivel %d) err=0x%X; reintento con 42",
                   level, (unsigned)herr);
      level = 42;
      herr = H264DECMemoryRequirement(vid.profile, level,
                                      vid.width, vid.height, &memReq);
   }
   if (herr != H264_ERROR_OK) FATAL("FATAL: MemoryRequirement err=0x%X", (unsigned)herr);
   WHBLogPrintf("[dec] nivel %d -> %.1f MB de trabajo", level, memReq / (1024.0 * 1024.0));

   void *decMem = memalign(H264_MEM_ALIGNMENT, memReq);
   if (!decMem) FATAL("FATAL: sin memoria para el decoder");

   CHECK(H264DECCheckMemSegmentation(decMem, memReq));
   CHECK(H264DECInitParam(memReq, decMem));
   CHECK(H264DECSetParam_FPTR_OUTPUT(decMem, frame_callback));
   CHECK(H264DECSetParam_OUTPUT_PER_FRAME(decMem, 0));  // buffered: el punto del spike
   CHECK(H264DECOpen(decMem));
   CHECK(H264DECBegin(decMem));

   // ---- 3. Framebuffers NV12
   uint32_t fbSize = FRAME_SIZE(vid.width, vid.height);
   uint8_t *fbs[NUM_FRAMEBUFFERS];
   for (int i = 0; i < NUM_FRAMEBUFFERS; i++) {
      fbs[i] = memalign(H264_MEM_ALIGNMENT, fbSize);
      if (!fbs[i]) FATAL("FATAL: sin memoria para framebuffer %d (%u bytes)", i, fbSize);
      H264Error se = H264DECCheckMemSegmentation(fbs[i], fbSize);
      if (se != H264_ERROR_OK) {
         WHBLogPrintf("[dec] aviso: segmentacion fb %d -> 0x%X", i, (unsigned)se);
      }
   }
   WHBLogPrintf("[dec] %d framebuffers de %u KB", NUM_FRAMEBUFFERS, fbSize / 1024);

   // Copias del plano Y para los volcados PGM
   for (int d = 0; d < NUM_DUMPS; d++) {
      size_t cap = (size_t)FRAME_PITCH(vid.width) * FRAME_HEIGHT(vid.height);
      g_out.dumps[d].y = malloc(cap);
      g_out.dumps[d].cap = g_out.dumps[d].y ? cap : 0;
   }

   // ---- 4. Bucle de decode: entrada en ORDEN DE DECODE (DTS)
   uint32_t bsCap = vid.maxSampleSize + vid.spsPpsSize + 4096;
   uint8_t *bitstream = memalign(H264_MEM_ALIGNMENT, bsCap);
   uint8_t *sampleBuf = malloc(vid.maxSampleSize);
   if (!bitstream || !sampleBuf) FATAL("FATAL: sin memoria para bitstream");

   FILE *f = fopen(mp4Path, "rb");
   if (!f) FATAL("FATAL: reapertura de %s", mp4Path);

   uint64_t totalUs = 0, minUs = UINT64_MAX, maxUs = 0;
   uint32_t decoded = 0, errors = 0;
   int fbIndex = 0;

   for (uint32_t i = 0; i < vid.sampleCount && WHBProcIsRunning(); i++) {
      Mp4Sample *s = &vid.samples[i];

      if (fseek(f, (long)s->offset, SEEK_SET) != 0 ||
          fread(sampleBuf, 1, s->size, f) != s->size) {
         WHBLogPrintf("[dec] lectura del sample %u fallo", i);
         errors++;
         continue;
      }

      uint32_t len = 0;
      if (i == 0) {
         memcpy(bitstream, vid.spsPps, vid.spsPpsSize);
         len = vid.spsPpsSize;
      }
      uint32_t conv = avcc_to_annexb(sampleBuf, s->size,
                                     bitstream + len, bsCap - len,
                                     vid.nalLengthSize);
      if (conv == 0) {
         WHBLogPrintf("[dec] sample %u AVCC malformado", i);
         errors++;
         continue;
      }
      len += conv;

      H264Error be = H264DECSetBitstream(decMem, bitstream, len, s->pts);
      if (be != H264_ERROR_OK) {
         WHBLogPrintf("[dec] SetBitstream sample %u -> 0x%X", i, (unsigned)be);
         errors++;
         continue;
      }

      g_out.cbTicksLast = 0;
      OSTime t0 = OSGetSystemTime();
      H264Error xe = H264DECExecute(decMem, fbs[fbIndex]);
      OSTime t1 = OSGetSystemTime();
      fbIndex = (fbIndex + 1) % NUM_FRAMEBUFFERS;

      if (((uint32_t)xe & ~0xffu) != 0) {
         WHBLogPrintf("[dec] Execute sample %u -> 0x%X", i, (unsigned)xe);
         errors++;
         continue;
      }

      uint64_t ticks = (uint64_t)(t1 - t0);
      if (ticks >= g_out.cbTicksLast) ticks -= g_out.cbTicksLast;
      uint64_t us = OSTicksToMicroseconds(ticks);
      totalUs += us;
      if (us < minUs) minUs = us;
      if (us > maxUs) maxUs = us;
      decoded++;

      if ((i + 1) % 60 == 0) {
         WHBLogPrintf("[dec] %u/%u | avg %.1f ms | min %.1f | max %.1f | out %u",
                      i + 1, vid.sampleCount,
                      totalUs / 1000.0 / decoded,
                      minUs / 1000.0, maxUs / 1000.0, g_out.framesOut);
         WHBLogConsoleDraw();
      }
   }

   // ---- 5. Vaciar el pipeline interno
   uint32_t beforeFlush = g_out.framesOut;
   H264DECFlush(decMem);
   WHBLogPrintf("[dec] Flush: %u frames retenidos emitidos", g_out.framesOut - beforeFlush);
   H264DECEnd(decMem);
   H264DECClose(decMem);
   fclose(f);

   // ---- 6. Reporte
   WHBLogPrintf("[pts] entrada (orden decode) vs salida (debe ser orden pres.):");
   for (uint32_t i = 0; i < PTS_LOG_COUNT && i < vid.sampleCount; i++) {
      WHBLogPrintf("[pts] %2u: in=%.3f%s  out=%.3f", i,
                   vid.samples[i].pts,
                   vid.samples[i].keyframe ? "*" : " ",
                   (i < g_out.framesOut) ? g_out.firstOutPts[i] : -1.0);
   }

   write_pgm_dumps();

   double avgMs = decoded ? totalUs / 1000.0 / decoded : 0;
   double fps = vid.sampleCount / (vid.duration > 0 ? vid.duration : 1);
   double budgetMs = 1000.0 / (fps > 0 ? fps : 30);

   WHBLogPrintf("== RESULTADO S2 [%s] ==", label);
   WHBLogPrintf("entrada: %u samples (%.0f fps) | B-frames: %s | salida: %u frames",
                vid.sampleCount, fps, vid.hasBFrames ? "SI" : "NO", g_out.framesOut);
   WHBLogPrintf("fuera de orden: %u | errores: %u | status0=%d",
                g_out.outOfOrder, errors, (int)g_out.firstStatus);
   WHBLogPrintf("decode: avg %.1f ms | min %.1f | max %.1f | presupuesto %.1f ms/frame",
                avgMs, minUs / 1000.0, maxUs / 1000.0, budgetMs);

   int ok = vid.hasBFrames && errors == 0 && g_out.outOfOrder == 0 &&
            g_out.framesOut >= vid.sampleCount - 1 && avgMs < budgetMs;
   WHBLogPrintf(ok ? "[%s] C2 OK: reordena B-frames y llega al tiempo real (%.1fx margen)"
                   : "[%s] C2 SIN CONFIRMAR: revisa arriba",
                label, ok ? budgetMs / (avgMs > 0 ? avgMs : 1) : 0.0);
   WHBLogConsoleDraw();

   // ---- 7. Liberar (la siguiente prueba necesita la memoria)
   for (int i = 0; i < NUM_FRAMEBUFFERS; i++) free(fbs[i]);
   for (int d = 0; d < NUM_DUMPS; d++) { free(g_out.dumps[d].y); g_out.dumps[d].y = NULL; }
   free(bitstream);
   free(sampleBuf);
   free(decMem);
   mp4_free(&vid);
}

static void wait_for_home(void)
{
   while (WHBProcIsRunning()) {
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(100));
   }
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogConsoleInit();
   WHBLogUdpInit();

   WHBLogPrintf("== WiiU Cast S2 v2: decode MP4 buffered (720p + 1080p) ==");

   if (!WHBMountSdCard()) {
      WHBLogPrintf("FATAL: no se pudo montar la SD");
      wait_for_home();
      goto shutdown;
   }
   snprintf(g_sdBase, sizeof(g_sdBase), "%s", WHBGetSdCardMountPath());

   probe_levels(100);
   WHBLogConsoleDraw();

   {
      char path[320];
      snprintf(path, sizeof(path), "%s/wiiucast/test.mp4", g_sdBase);
      decode_file(path, "720");
      snprintf(path, sizeof(path), "%s/wiiucast/test-1080.mp4", g_sdBase);
      decode_file(path, "1080");
   }

   WHBLogPrintf("== FIN: ambas pruebas arriba; HOME para salir ==");
   wait_for_home();

shutdown:
   WHBUnmountSdCard();
   WHBLogConsoleFree();
   WHBProcShutdown();
   return 0;
}
