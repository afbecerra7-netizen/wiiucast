// ============================================================================
// WiiU Cast — Spike S2: decode de archivos MP4 reales (condición C2 del GO)
//
// Pregunta que responde: ¿decodifica H264DEC un MP4 normal de teléfono
// (B-frames, framing AVCC) en MODO BUFFERED, reordenando la salida a orden
// de presentación, y a qué velocidad?
//
// Todo el homebrew existente (moonlight, magiquest) usa streams SIN B-frames
// en modo OUTPUT_PER_FRAME=1. Este spike prueba exactamente lo contrario:
//   - demux MP4 propio (mp4demux.c, sin FFmpeg)
//   - conversión AVCC -> Annex-B
//   - OUTPUT_PER_FRAME = 0 (buffered): el decoder retiene ~5 frames y debe
//     emitirlos por callback EN ORDEN DE PRESENTACIÓN con sus timestamps
//   - medición de ms por H264DECExecute
//   - sondeo de H264DECMemoryRequirement para niveles 4.1/4.2/5.0/5.1
//     (resuelve la contradicción nivel 4.2 vs 5.1 de la auditoría)
//   - volcado de frames decodificados como PGM a la SD para inspección
//
// El callback de salida corre SÍNCRONO dentro de H264DECExecute, así que ahí
// no se loguea ni se escribe a SD: solo se copian datos a memoria y se mide
// su propio coste, que el bucle descuenta de la medición del Execute. Los
// logs y los PGM salen después del bucle de decode.
//
// Entrada:  sd:/wiiucast/test.mp4   (cómo generarlo: spikes/README.md)
// Salida:   log en pantalla/UDP + sd:/wiiucast/s2-frame-*.pgm
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

#include "mp4demux.h"

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

// ---------------------------------------------------------------------------
// Estado de la salida del decoder. El callback SOLO escribe aquí (nada de
// logs ni de SD dentro de la ventana cronometrada del Execute).
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
   uint64_t cbTicksLast;            // ticks gastados por el callback en el
                                    // Execute en curso (el bucle los descuenta)
} g_out;

static void frame_callback(H264DecodeOutput *output)
{
   OSTime cb0 = OSGetSystemTime();

   for (int32_t i = 0; i < output->frameCount; i++) {
      H264DecodeResult *r = output->decodeResults[i];
      uint32_t idx = g_out.framesOut;

      if (idx == 0) g_out.firstStatus = r->status;
      if (idx < PTS_LOG_COUNT) g_out.firstOutPts[idx] = r->timestamp;

      // La prueba central del spike: en buffered mode la salida DEBE venir en
      // orden de presentación aunque la entrada vaya en orden de decode.
      if (idx > 0 && r->timestamp < g_out.lastPts) g_out.outOfOrder++;
      g_out.lastPts = r->timestamp;
      g_out.lastWidth = r->width;
      g_out.lastHeight = r->height;
      g_out.lastPitch = r->nextLine;

      // Copiar el plano Y de los frames a volcar (el PGM se escribe DESPUÉS
      // del bucle de decode; aquí solo memcpy a un buffer pre-asignado).
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
// Sondeo de niveles: ¿acepta la librería 5.0/5.1 y cuánta memoria pide?
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
// AVCC -> Annex-B: reemplaza cada prefijo de longitud por 00 00 00 01.
// Comprobaciones por resta (no suma) para que un nalLen corrupto de 4 bytes
// no desborde uint32 y esquive los límites.
// Devuelve bytes escritos en dst, o 0 si el sample está malformado.
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
      // invariantes: in <= srcLen, out <= dstCap
      if (nalLen == 0 || nalLen > srcLen - in) return 0;
      if (dstCap - out < 4 || nalLen > dstCap - out - 4) return 0;

      memcpy(dst + out, startCode, 4); out += 4;
      memcpy(dst + out, src + in, nalLen); out += nalLen;
      in += nalLen;
   }
   return (in == srcLen) ? out : 0;
}

static void write_pgm_dumps(const char *sdBase)
{
   for (int d = 0; d < NUM_DUMPS; d++) {
      DumpSlot *s = &g_out.dumps[d];
      if (!s->valid) continue;

      char path[320];
      snprintf(path, sizeof(path), "%s/wiiucast/s2-frame-%u.pgm",
               sdBase, s->frameIndex);
      FILE *f = fopen(path, "wb");
      if (!f) {
         WHBLogPrintf("[pgm] no pude crear %s", path);
         continue;
      }
      fprintf(f, "P5\n%d %d\n255\n", (int)s->width, (int)s->height);
      fwrite(s->y, 1, (size_t)s->width * s->height, f);
      fclose(f);
      WHBLogPrintf("[pgm] frame %u (pts %.3f) -> %s (%dx%d)",
                   s->frameIndex, s->pts, path, (int)s->width, (int)s->height);
   }
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

   WHBLogPrintf("== WiiU Cast S2: decode MP4 real, modo buffered ==");

   char sdBase[256] = "";
   if (!WHBMountSdCard()) {
      WHBLogPrintf("FATAL: no se pudo montar la SD");
      wait_for_home();
      goto shutdown;
   }
   snprintf(sdBase, sizeof(sdBase), "%s", WHBGetSdCardMountPath());

   char mp4Path[320];
   snprintf(mp4Path, sizeof(mp4Path), "%s/wiiucast/test.mp4", sdBase);

   // ---- 1. Demux
   Mp4Video vid;
   char err[160];
   if (mp4_parse(mp4Path, &vid, err, sizeof(err)) != 0) {
      WHBLogPrintf("FATAL demux %s:", mp4Path);
      WHBLogPrintf("  %s", err);
      WHBLogPrintf("Genera el archivo segun spikes/README.md");
      wait_for_home();
      goto shutdown;
   }

   WHBLogPrintf("[mp4] %dx%d profile=%d nivel=%d.%d nalLen=%d",
                vid.width, vid.height, vid.profile,
                vid.level / 10, vid.level % 10, vid.nalLengthSize);
   WHBLogPrintf("[mp4] %u samples, %.1f s, maxSample=%u KB, B-frames: %s",
                vid.sampleCount, vid.duration, vid.maxSampleSize / 1024,
                vid.hasBFrames ? "SI (lo que queremos probar)" : "NO (regenera con -bf 3!)");
   WHBLogConsoleDraw();

   // ---- 2. Sondeo de niveles (pregunta abierta de la auditoría)
   probe_levels(vid.profile);
   WHBLogConsoleDraw();

   // ---- 3. Preparar el decoder en MODO BUFFERED
   {
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
      if (herr != H264_ERROR_OK) {
         WHBLogPrintf("FATAL: MemoryRequirement err=0x%X", (unsigned)herr);
         wait_for_home();
         goto shutdown;
      }
      WHBLogPrintf("[dec] nivel %d -> %u bytes (%.1f MB) de trabajo",
                   level, memReq, memReq / (1024.0 * 1024.0));

      void *decMem = memalign(H264_MEM_ALIGNMENT, memReq);
      if (!decMem) {
         WHBLogPrintf("FATAL: sin memoria para el decoder");
         wait_for_home();
         goto shutdown;
      }

      #define CHECK(call)                                                    \
         do {                                                                \
            H264Error e_ = (call);                                           \
            if (e_ != H264_ERROR_OK) {                                       \
               WHBLogPrintf("FATAL: %s -> 0x%X", #call, (unsigned)e_);       \
               wait_for_home();                                              \
               goto shutdown;                                                \
            }                                                                \
         } while (0)

      CHECK(H264DECCheckMemSegmentation(decMem, memReq));
      CHECK(H264DECInitParam(memReq, decMem));
      CHECK(H264DECSetParam_FPTR_OUTPUT(decMem, frame_callback));
      // ============ EL PARÁMETRO QUE ESTE SPIKE EXISTE PARA PROBAR ========
      // 0 = buffered: el decoder retiene ~5 frames y reordena a orden de
      // presentación. (moonlight usa 1 = per-frame, sin B-frames.)
      CHECK(H264DECSetParam_OUTPUT_PER_FRAME(decMem, 0));
      CHECK(H264DECOpen(decMem));
      CHECK(H264DECBegin(decMem));

      // ---- 4. Framebuffers NV12 (pitch alineado a 256, alto a 16)
      uint32_t fbSize = FRAME_SIZE(vid.width, vid.height);
      uint8_t *fbs[NUM_FRAMEBUFFERS];
      for (int i = 0; i < NUM_FRAMEBUFFERS; i++) {
         fbs[i] = memalign(H264_MEM_ALIGNMENT, fbSize);
         if (!fbs[i]) {
            WHBLogPrintf("FATAL: sin memoria para framebuffer %d (%u bytes)", i, fbSize);
            wait_for_home();
            goto shutdown;
         }
         H264Error se = H264DECCheckMemSegmentation(fbs[i], fbSize);
         if (se != H264_ERROR_OK) {
            WHBLogPrintf("[dec] aviso: segmentacion framebuffer %d -> 0x%X", i, (unsigned)se);
         }
      }
      WHBLogPrintf("[dec] %d framebuffers de %u KB", NUM_FRAMEBUFFERS, fbSize / 1024);

      // Buffers para las copias del plano Y de los frames a volcar
      // (pitch por si el decoder reporta el ancho codificado > ancho del mp4)
      for (int d = 0; d < NUM_DUMPS; d++) {
         size_t cap = (size_t)FRAME_PITCH(vid.width) * FRAME_HEIGHT(vid.height);
         g_out.dumps[d].y = malloc(cap);
         g_out.dumps[d].cap = g_out.dumps[d].y ? cap : 0;
         // si falla, simplemente no se vuelca ese frame (s->y == NULL)
      }

      // ---- 5. Bucle de decode: entrada en ORDEN DE DECODE (DTS)
      uint32_t bsCap = vid.maxSampleSize + vid.spsPpsSize + 4096;
      uint8_t *bitstream = memalign(H264_MEM_ALIGNMENT, bsCap);
      uint8_t *sampleBuf = malloc(vid.maxSampleSize);
      if (!bitstream || !sampleBuf) {
         WHBLogPrintf("FATAL: sin memoria para buffers de bitstream");
         wait_for_home();
         goto shutdown;
      }

      FILE *f = fopen(mp4Path, "rb");
      if (!f) {
         WHBLogPrintf("FATAL: reapertura de %s", mp4Path);
         wait_for_home();
         goto shutdown;
      }

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
         if (i == 0) {  // SPS/PPS delante del primer sample
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

         // Medición: el callback corre dentro del Execute; su coste (copias
         // de plano Y para los dumps) se acumula en cbTicksLast y se descuenta.
         g_out.cbTicksLast = 0;
         OSTime t0 = OSGetSystemTime();
         H264Error xe = H264DECExecute(decMem, fbs[fbIndex]);
         OSTime t1 = OSGetSystemTime();
         fbIndex = (fbIndex + 1) % NUM_FRAMEBUFFERS;

         // Convención de moonlight: el byte bajo es informativo; el resto, error.
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

         if ((i + 1) % 30 == 0) {
            WHBLogPrintf("[dec] %u/%u | avg %.1f ms | min %.1f | max %.1f | out %u",
                         i + 1, vid.sampleCount,
                         totalUs / 1000.0 / decoded,
                         minUs / 1000.0, maxUs / 1000.0, g_out.framesOut);
            WHBLogConsoleDraw();
         }
      }

      // ---- 6. Vaciar el pipeline interno (~5 frames retenidos)
      uint32_t beforeFlush = g_out.framesOut;
      H264DECFlush(decMem);
      WHBLogPrintf("[dec] Flush: %u frames retenidos emitidos",
                   g_out.framesOut - beforeFlush);
      H264DECEnd(decMem);
      H264DECClose(decMem);
      fclose(f);

      // ---- 7. Reporte diferido: PTS entrada vs salida, PGM, veredicto
      WHBLogPrintf("[pts] entrada (orden decode) vs salida (debe ser orden pres.):");
      for (uint32_t i = 0; i < PTS_LOG_COUNT && i < vid.sampleCount; i++) {
         WHBLogPrintf("[pts] %2u: in=%.3f%s  out=%.3f", i,
                      vid.samples[i].pts,
                      vid.samples[i].keyframe ? "*" : " ",
                      (i < g_out.framesOut) ? g_out.firstOutPts[i] : -1.0);
      }

      write_pgm_dumps(sdBase);

      double avgMs = decoded ? totalUs / 1000.0 / decoded : 0;
      double fps = vid.sampleCount / (vid.duration > 0 ? vid.duration : 1);
      double budgetMs = 1000.0 / (fps > 0 ? fps : 30);

      WHBLogPrintf("== RESULTADO S2 ==");
      WHBLogPrintf("entrada: %u samples (%.0f fps aprox) | B-frames: %s",
                   vid.sampleCount, fps, vid.hasBFrames ? "SI" : "NO");
      WHBLogPrintf("salida:  %u frames | PTS fuera de orden: %u | errores: %u | status0=%d",
                   g_out.framesOut, g_out.outOfOrder, errors, (int)g_out.firstStatus);
      WHBLogPrintf("decode:  avg %.1f ms | min %.1f | max %.1f | presupuesto %.1f ms/frame",
                   avgMs, minUs / 1000.0, maxUs / 1000.0, budgetMs);

      int ok = vid.hasBFrames && errors == 0 && g_out.outOfOrder == 0 &&
               g_out.framesOut >= vid.sampleCount - 1 && avgMs < budgetMs;
      if (ok) {
         WHBLogPrintf("C2 CONFIRMADA: buffered mode reordena B-frames y llega al tiempo real.");
      } else {
         WHBLogPrintf("C2 SIN CONFIRMAR: revisa arriba que fallo (orden/errores/velocidad).");
      }
      WHBLogPrintf("Frames volcados en sd:/wiiucast/s2-frame-*.pgm (abrir en el PC).");
   }

   wait_for_home();

shutdown:
   WHBUnmountSdCard();
   WHBLogConsoleFree();
   WHBProcShutdown();
   return 0;
}
