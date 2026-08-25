#include "aac_decoder.h"

#include <whb/log.h>
#include <neaacdec.h>

#include <string.h>

static NeAACDecHandle s_dec;
static int s_rate, s_channels;
static uint32_t s_errors;

BOOL aac_decoder_open(const uint8_t *asc, uint32_t ascSize,
                      int *outRate, int *outChannels)
{
   aac_decoder_close();

   if (!asc || ascSize == 0) {
      WHBLogPrintf("[aac] sin AudioSpecificConfig");
      return FALSE;
   }

   s_dec = NeAACDecOpen();
   if (!s_dec) { WHBLogPrintf("[aac] NeAACDecOpen fallo"); return FALSE; }

   NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(s_dec);
   cfg->outputFormat = FAAD_FMT_16BIT;   // el formato que quiere AX
   cfg->downMatrix   = 1;                // multicanal -> estéreo
   NeAACDecSetConfiguration(s_dec, cfg);

   unsigned long rate = 0;
   unsigned char channels = 0;
   if (NeAACDecInit2(s_dec, (unsigned char *)asc, ascSize, &rate, &channels) < 0) {
      WHBLogPrintf("[aac] NeAACDecInit2 rechazo la configuracion");
      NeAACDecClose(s_dec);
      s_dec = NULL;
      return FALSE;
   }

   s_rate = (int)rate;
   s_channels = channels;
   // downMatrix ya reduce a estéreo; si el stream trae más, lo tratamos así.
   if (s_channels > 2) s_channels = 2;
   s_errors = 0;

   if (outRate)     *outRate = s_rate;
   if (outChannels) *outChannels = s_channels;

   WHBLogPrintf("[aac] abierto: %d Hz, %d canales", s_rate, s_channels);
   return TRUE;
}

void aac_decoder_close(void)
{
   if (s_dec) { NeAACDecClose(s_dec); s_dec = NULL; }
   s_rate = s_channels = 0;
}

BOOL aac_decoder_ready(void) { return s_dec != NULL; }

const int16_t *aac_decoder_decode(const uint8_t *data, uint32_t size,
                                  uint32_t *outFrames)
{
   if (outFrames) *outFrames = 0;
   if (!s_dec || !data || size == 0) return NULL;

   NeAACDecFrameInfo info;
   memset(&info, 0, sizeof(info));
   void *pcm = NeAACDecDecode(s_dec, &info, (unsigned char *)data, size);

   if (info.error != 0 || !pcm) {
      s_errors++;
      if (s_errors <= 3) {
         WHBLogPrintf("[aac] error %u: %s", (unsigned)info.error,
                      NeAACDecGetErrorMessage(info.error));
      }
      return NULL;
   }
   if (info.samples == 0) return NULL;

   // info.samples cuenta muestras totales, no frames por canal.
   int ch = info.channels ? info.channels : s_channels;
   if (ch < 1) ch = 1;
   if (outFrames) *outFrames = (uint32_t)(info.samples / ch);

   // Si el stream cambia de formato a mitad (SBR implícito), quedarnos con lo
   // que dice el frame: el reloj y la salida dependen de estos valores.
   if ((int)info.samplerate > 0) s_rate = (int)info.samplerate;
   s_channels = ch > 2 ? 2 : ch;

   return (const int16_t *)pcm;
}

int aac_decoder_channels(void)  { return s_channels; }
int aac_decoder_rate(void)      { return s_rate; }
uint32_t aac_decoder_errors(void) { return s_errors; }
