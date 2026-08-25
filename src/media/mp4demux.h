// Demuxer MP4 (ISO BMFF) mínimo para el spike S2.
// Extrae de la primera pista de vídeo H.264 (avc1/avc3):
//   - SPS/PPS del avcC ya convertidos a Annex-B
//   - tabla plana de samples con offset/tamaño/DTS/PTS (stts+ctts)
//   - flag de B-frames (algún ctts != 0), que es lo que el spike quiere probar
//
// Limitaciones deliberadas (es un spike): un solo trak de vídeo, sin edit
// lists (elst), sin fragmentos (moof), archivos < 2 GB.

#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
   uint64_t offset;     // offset absoluto en el archivo
   uint32_t size;       // bytes del sample (NALs con prefijo de longitud AVCC)
   double dts;          // segundos
   double pts;          // segundos (dts + ctts)
   int keyframe;        // según stss (o 1 si no hay stss)
} Mp4Sample;

typedef struct {
   int width, height;         // del sample entry
   int profile, level;        // AVCProfileIndication / AVCLevelIndication (avcC)
   int nalLengthSize;         // 1, 2 o 4 (lengthSizeMinusOne + 1)

   uint8_t spsPps[1024];      // SPS+PPS en Annex-B (00 00 00 01 ...)
   uint32_t spsPpsSize;

   Mp4Sample *samples;        // malloc'd; liberar con mp4_free
   uint32_t sampleCount;
   uint32_t maxSampleSize;    // para dimensionar el buffer de bitstream
   double duration;           // segundos
   int hasBFrames;            // 1 si algún ctts offset != 0
} Mp4Video;

// --- Pista de audio ---------------------------------------------------------
typedef enum { MP4_AUDIO_NONE = 0, MP4_AUDIO_AAC } Mp4AudioCodec;

typedef struct {
   Mp4AudioCodec codec;
   int sampleRate;            // del sample entry (el ASC puede corregirlo)
   int channels;

   uint8_t asc[64];           // AudioSpecificConfig sacado del esds
   uint32_t ascSize;          // 0 si no se encontró

   Mp4Sample *samples;        // malloc'd; liberar con mp4_free_audio
   uint32_t sampleCount;
   uint32_t maxSampleSize;
   double duration;
} Mp4Audio;

// 0 = ok; <0 = error (mensaje en errbuf)
int mp4_parse(const char *path, Mp4Video *out, char *errbuf, size_t errlen);

// Igual pero sobre un prefijo del archivo ya en memoria (reproducción durante
// la descarga). Exige que el moov esté al principio: `-movflags +faststart`.
int mp4_parse_memory(const uint8_t *data, uint32_t len, Mp4Video *out,
                     char *errbuf, size_t errlen);

// Variante que además extrae la pista de audio. `audio` puede ser NULL; si no
// hay pista de audio compatible, `audio->codec` queda en MP4_AUDIO_NONE (el
// vídeo se reproduce igual, solo que mudo).
int mp4_parse_memory_av(const uint8_t *data, uint32_t len,
                        Mp4Video *video, Mp4Audio *audio,
                        char *errbuf, size_t errlen);

void mp4_free(Mp4Video *v);
void mp4_free_audio(Mp4Audio *a);
