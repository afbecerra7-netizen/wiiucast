#include "decoder.h"

#include <whb/log.h>
#include <h264/decode.h>
#include <h264/stream.h>

#include <malloc.h>
#include <string.h>

#define DEC_ALIGNMENT 0x400

static void *s_mem;
static DecoderFrameFn s_onFrame;
static void *s_user;
static uint32_t s_framesOut, s_errors;
static BOOL s_open;

// El decoder no nos dice a qué buffer corresponde cada frame emitido: en modo
// buffered la salida va desfasada respecto a la entrada. Se lleva una cola de
// los índices en el orden en que se entregaron a Execute, y se consumen en
// orden al emitir — que es el mismo orden en que el decoder los llena.
#define PENDING_MAX 16
static int s_pending[PENDING_MAX];
static int s_pendHead, s_pendTail, s_pendCount;

static void pending_push(int index)
{
   if (s_pendCount >= PENDING_MAX) return;   // no debería pasar
   s_pending[s_pendTail] = index;
   s_pendTail = (s_pendTail + 1) % PENDING_MAX;
   s_pendCount++;
}

static int pending_pop(void)
{
   if (s_pendCount == 0) return -1;
   int v = s_pending[s_pendHead];
   s_pendHead = (s_pendHead + 1) % PENDING_MAX;
   s_pendCount--;
   return v;
}

static void frame_callback(H264DecodeOutput *output)
{
   for (int32_t i = 0; i < output->frameCount; i++) {
      H264DecodeResult *r = output->decodeResults[i];
      int index = pending_pop();
      s_framesOut++;
      if (index >= 0 && s_onFrame) {
         s_onFrame(index, r->timestamp, r->width, r->height, r->nextLine, s_user);
      }
   }
}

BOOL decoder_open(int profile, int level, int width, int height,
                  DecoderFrameFn onFrame, void *user)
{
   decoder_close();

   uint32_t memReq = 0;
   H264Error err = H264DECMemoryRequirement(profile, level, width, height, &memReq);
   if (err != H264_ERROR_OK) {
      // Algunos MP4 declaran niveles que la librería no acepta; 4.2 cubre
      // hasta 1080p, que es nuestro techo real de reproducción.
      WHBLogPrintf("[dec] nivel %d rechazado (0x%X), reintento con 4.2", level, (unsigned)err);
      level = 42;
      err = H264DECMemoryRequirement(profile, level, width, height, &memReq);
   }
   if (err != H264_ERROR_OK) {
      WHBLogPrintf("[dec] MemoryRequirement fallo: 0x%X (perfil %d)", (unsigned)err, profile);
      return FALSE;
   }

   s_mem = memalign(DEC_ALIGNMENT, memReq);
   if (!s_mem) {
      WHBLogPrintf("[dec] sin memoria para el decoder (%u KB)", memReq / 1024);
      return FALSE;
   }

   #define TRY(call)                                                   \
      do {                                                             \
         H264Error e_ = (call);                                        \
         if (e_ != H264_ERROR_OK) {                                    \
            WHBLogPrintf("[dec] %s -> 0x%X", #call, (unsigned)e_);     \
            free(s_mem); s_mem = NULL;                                 \
            return FALSE;                                              \
         }                                                             \
      } while (0)

   TRY(H264DECCheckMemSegmentation(s_mem, memReq));
   TRY(H264DECInitParam(memReq, s_mem));
   TRY(H264DECSetParam_FPTR_OUTPUT(s_mem, frame_callback));
   TRY(H264DECSetParam_OUTPUT_PER_FRAME(s_mem, 0));   // buffered: reordena B-frames
   TRY(H264DECOpen(s_mem));
   TRY(H264DECBegin(s_mem));
   #undef TRY

   s_onFrame = onFrame;
   s_user = user;
   s_framesOut = s_errors = 0;
   s_pendHead = s_pendTail = s_pendCount = 0;
   s_open = TRUE;

   WHBLogPrintf("[dec] abierto: %dx%d perfil %d nivel %d (%u KB)",
                width, height, profile, level, memReq / 1024);
   return TRUE;
}

BOOL decoder_submit(const uint8_t *annexb, uint32_t len, double pts,
                    void *frameBuffer, int index)
{
   if (!s_open) return FALSE;

   H264Error be = H264DECSetBitstream(s_mem, (uint8_t *)annexb, len, pts);
   if (be != H264_ERROR_OK) {
      s_errors++;
      return FALSE;
   }

   pending_push(index);

   H264Error xe = H264DECExecute(s_mem, frameBuffer);
   // Convención heredada de moonlight: el byte bajo trae información de
   // estado; cualquier bit por encima señala error real.
   if (((uint32_t)xe & ~0xffu) != 0) {
      s_errors++;
      pending_pop();   // ese buffer no va a emitirse
      return FALSE;
   }
   return TRUE;
}

void decoder_flush(void)
{
   if (s_open) H264DECFlush(s_mem);
}

void decoder_close(void)
{
   if (!s_open) return;
   H264DECFlush(s_mem);
   H264DECEnd(s_mem);
   H264DECClose(s_mem);
   free(s_mem);
   s_mem = NULL;
   s_open = FALSE;
   s_onFrame = NULL;
}

uint32_t decoder_frames_out(void) { return s_framesOut; }
uint32_t decoder_errors(void)     { return s_errors; }
