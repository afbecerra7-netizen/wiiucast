#include "mp4demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR(...) do { if (errbuf) snprintf(errbuf, errlen, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Lectores big-endian sobre buffer en memoria
// ---------------------------------------------------------------------------
static uint32_t be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
          ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
   return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint16_t be16(const uint8_t *p)
{
   return ((uint16_t)p[0] << 8) | p[1];
}

#define FOURCC(a, b, c, d) \
   (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// Busca un box hijo dentro de [p, p+len). Devuelve puntero al PAYLOAD y su
// tamaño en *outLen, o NULL. `start` permite continuar una iteración.
static const uint8_t *find_box(const uint8_t *p, size_t len, uint32_t type,
                               size_t *outLen, const uint8_t *start)
{
   const uint8_t *cur = start ? start : p;
   const uint8_t *end = p + len;

   while (cur + 8 <= end) {
      uint64_t boxSize = be32(cur);
      uint32_t boxType = be32(cur + 4);
      size_t hdr = 8;

      if (boxSize == 1) {
         if (end - cur < 16) return NULL;
         boxSize = be64(cur + 8);
         hdr = 16;
      } else if (boxSize == 0) {
         boxSize = (uint64_t)(end - cur);  // hasta el final del contenedor
      }

      // Comparar tamaños, no punteros: en un target de 32 bits un boxSize
      // corrupto (p.ej. 0xFFFFFF00) haría wrap a cur+boxSize y pasaría el
      // check de puntero.
      if (boxSize < hdr || boxSize > (uint64_t)(end - cur)) return NULL;

      if (boxType == type) {
         *outLen = (size_t)(boxSize - hdr);
         return cur + hdr;
      }
      cur += boxSize;
   }
   return NULL;
}

// Como find_box pero devolviendo también el puntero al box completo para
// poder seguir iterando (múltiples 'trak').
static const uint8_t *next_box_of(const uint8_t *p, size_t len, uint32_t type,
                                  size_t *outLen, const uint8_t **iter)
{
   const uint8_t *cur = *iter ? *iter : p;
   const uint8_t *end = p + len;

   while (cur + 8 <= end) {
      uint64_t boxSize = be32(cur);
      uint32_t boxType = be32(cur + 4);
      size_t hdr = 8;

      if (boxSize == 1) {
         if (end - cur < 16) return NULL;
         boxSize = be64(cur + 8);
         hdr = 16;
      } else if (boxSize == 0) {
         boxSize = (uint64_t)(end - cur);
      }

      // Igual que en find_box: comparar tamaños para evitar wrap de puntero.
      if (boxSize < hdr || boxSize > (uint64_t)(end - cur)) return NULL;

      const uint8_t *payload = cur + hdr;
      size_t payloadLen = (size_t)(boxSize - hdr);
      cur += boxSize;

      if (boxType == type) {
         *outLen = payloadLen;
         *iter = cur;
         return payload;
      }
   }
   return NULL;
}

// ---------------------------------------------------------------------------
// avcC -> SPS/PPS Annex-B
// ---------------------------------------------------------------------------
static int parse_avcc(const uint8_t *p, size_t len, Mp4Video *v,
                      char *errbuf, size_t errlen)
{
   if (len < 7) { ERR("avcC demasiado corto (%u)", (unsigned)len); return -1; }

   v->profile = p[1];
   v->level = p[3];
   v->nalLengthSize = (p[4] & 0x03) + 1;

   static const uint8_t startCode[4] = { 0, 0, 0, 1 };
   size_t pos = 5;
   uint32_t out = 0;

   int numSps = p[pos++] & 0x1F;
   for (int i = 0; i < numSps; i++) {
      if (pos + 2 > len) { ERR("avcC SPS truncado"); return -1; }
      uint16_t sz = be16(p + pos); pos += 2;
      if (pos + sz > len) { ERR("avcC SPS truncado"); return -1; }
      if (out + 4 + sz > sizeof(v->spsPps)) { ERR("SPS/PPS > 1 KB"); return -1; }
      memcpy(v->spsPps + out, startCode, 4); out += 4;
      memcpy(v->spsPps + out, p + pos, sz); out += sz;
      pos += sz;
   }

   if (pos >= len) { ERR("avcC sin PPS"); return -1; }
   int numPps = p[pos++];
   for (int i = 0; i < numPps; i++) {
      if (pos + 2 > len) { ERR("avcC PPS truncado"); return -1; }
      uint16_t sz = be16(p + pos); pos += 2;
      if (pos + sz > len) { ERR("avcC PPS truncado"); return -1; }
      if (out + 4 + sz > sizeof(v->spsPps)) { ERR("SPS/PPS > 1 KB"); return -1; }
      memcpy(v->spsPps + out, startCode, 4); out += 4;
      memcpy(v->spsPps + out, p + pos, sz); out += sz;
      pos += sz;
   }

   v->spsPpsSize = out;
   if (numSps == 0 || numPps == 0) { ERR("avcC sin SPS o PPS"); return -1; }
   return 0;
}

// ---------------------------------------------------------------------------
// stbl -> tabla plana de samples
// ---------------------------------------------------------------------------
static int build_samples(const uint8_t *stbl, size_t stblLen, uint32_t timescale,
                         Mp4Video *v, char *errbuf, size_t errlen)
{
   size_t sttsLen, stscLen, stszLen, stcoLen, cttsLen, stssLen;
   const uint8_t *stts = find_box(stbl, stblLen, FOURCC('s','t','t','s'), &sttsLen, NULL);
   const uint8_t *stsc = find_box(stbl, stblLen, FOURCC('s','t','s','c'), &stscLen, NULL);
   const uint8_t *stsz = find_box(stbl, stblLen, FOURCC('s','t','s','z'), &stszLen, NULL);
   const uint8_t *stco = find_box(stbl, stblLen, FOURCC('s','t','c','o'), &stcoLen, NULL);
   const uint8_t *co64 = find_box(stbl, stblLen, FOURCC('c','o','6','4'), &stcoLen, NULL);
   const uint8_t *ctts = find_box(stbl, stblLen, FOURCC('c','t','t','s'), &cttsLen, NULL);
   const uint8_t *stss = find_box(stbl, stblLen, FOURCC('s','t','s','s'), &stssLen, NULL);

   if (!stts || !stsc || !stsz || (!stco && !co64)) {
      ERR("stbl incompleto (stts=%p stsc=%p stsz=%p stco/co64=%p)",
          (void *)stts, (void *)stsc, (void *)stsz, (void *)(stco ? stco : co64));
      return -1;
   }

   // stsz: version+flags(4) sample_size(4) sample_count(4) [entries]
   // Todas las validaciones de tablas van por división, no multiplicación:
   // en size_t de 32 bits un entry_count corrupto desbordaría el producto.
   if (stszLen < 12) { ERR("stsz corto"); return -1; }
   uint32_t uniformSize = be32(stsz + 4);
   uint32_t sampleCount = be32(stsz + 8);
   if (sampleCount == 0) { ERR("0 samples"); return -1; }
   if (uniformSize == 0 && sampleCount > (stszLen - 12) / 4) {
      ERR("stsz truncado"); return -1;
   }

   v->samples = calloc(sampleCount, sizeof(Mp4Sample));
   if (!v->samples) { ERR("sin memoria para %u samples", sampleCount); return -1; }
   v->sampleCount = sampleCount;

   // --- tamaños + máximo
   for (uint32_t i = 0; i < sampleCount; i++) {
      uint32_t sz = uniformSize ? uniformSize : be32(stsz + 12 + i * 4);
      v->samples[i].size = sz;
      if (sz > v->maxSampleSize) v->maxSampleSize = sz;
   }

   // --- DTS desde stts
   {
      if (sttsLen < 8) { ERR("stts corto"); return -1; }
      uint32_t entries = be32(stts + 4);
      if (entries > (sttsLen - 8) / 8) { ERR("stts truncado"); return -1; }
      uint64_t dts = 0;
      uint32_t si = 0;
      for (uint32_t e = 0; e < entries && si < sampleCount; e++) {
         uint32_t count = be32(stts + 8 + e * 8);
         uint32_t delta = be32(stts + 8 + e * 8 + 4);
         for (uint32_t k = 0; k < count && si < sampleCount; k++, si++) {
            v->samples[si].dts = (double)dts / timescale;
            v->samples[si].pts = v->samples[si].dts;  // ctts lo ajusta después
            dts += delta;
         }
      }
      v->duration = (double)dts / timescale;
      if (si < sampleCount) { ERR("stts cubre %u/%u samples", si, sampleCount); return -1; }
   }

   // --- PTS desde ctts (aquí viven los B-frames)
   if (ctts) {
      if (cttsLen < 8) { ERR("ctts corto"); return -1; }
      uint32_t entries = be32(ctts + 4);
      if (entries > (cttsLen - 8) / 8) { ERR("ctts truncado"); return -1; }
      uint32_t si = 0;
      for (uint32_t e = 0; e < entries && si < sampleCount; e++) {
         uint32_t count = be32(ctts + 8 + e * 8);
         int32_t off = (int32_t)be32(ctts + 8 + e * 8 + 4);  // v1 firmado; v0 cabe igual
         for (uint32_t k = 0; k < count && si < sampleCount; k++, si++) {
            v->samples[si].pts = v->samples[si].dts + (double)off / timescale;
            if (off != 0) v->hasBFrames = 1;
         }
      }
   }

   // --- keyframes desde stss (si no hay stss, todos son sync samples)
   if (stss) {
      if (stssLen < 8) { ERR("stss corto"); return -1; }
      uint32_t entries = be32(stss + 4);
      if (entries > (stssLen - 8) / 4) { ERR("stss truncado"); return -1; }
      for (uint32_t e = 0; e < entries; e++) {
         uint32_t sampleNum = be32(stss + 8 + e * 4);  // 1-based
         if (sampleNum >= 1 && sampleNum <= sampleCount) {
            v->samples[sampleNum - 1].keyframe = 1;
         }
      }
   } else {
      for (uint32_t i = 0; i < sampleCount; i++) v->samples[i].keyframe = 1;
   }

   // --- offsets: stsc (samples por chunk) + stco/co64 (offset de cada chunk)
   {
      if (stscLen < 8) { ERR("stsc corto"); return -1; }
      uint32_t stscEntries = be32(stsc + 4);
      if (stscEntries == 0 || stscEntries > (stscLen - 8) / 12) { ERR("stsc truncado"); return -1; }

      if (stcoLen < 8) { ERR("stco/co64 corto"); return -1; }
      uint32_t chunkCount = be32((co64 ? co64 : stco) + 4);
      size_t entrySize = co64 ? 8 : 4;
      if (chunkCount > (stcoLen - 8) / entrySize) { ERR("stco/co64 truncado"); return -1; }

      uint32_t si = 0;
      for (uint32_t c = 1, e = 0; c <= chunkCount && si < sampleCount; c++) {
         // avanzar la run de stsc vigente para el chunk c
         while (e + 1 < stscEntries && be32(stsc + 8 + (e + 1) * 12) <= c) {
            e++;
         }
         uint32_t samplesInChunk = be32(stsc + 8 + e * 12 + 4);

         uint64_t off = co64 ? be64(co64 + 8 + (c - 1) * 8)
                             : be32(stco + 8 + (c - 1) * 4);

         for (uint32_t k = 0; k < samplesInChunk && si < sampleCount; k++, si++) {
            v->samples[si].offset = off;
            off += v->samples[si].size;
         }
      }
      if (si < sampleCount) { ERR("chunks cubren %u/%u samples", si, sampleCount); return -1; }
   }

   return 0;
}

// ---------------------------------------------------------------------------
// trak de vídeo: hdlr == 'vide' y stsd con avc1/avc3
// ---------------------------------------------------------------------------
static int parse_trak(const uint8_t *trak, size_t trakLen, Mp4Video *v,
                      char *errbuf, size_t errlen)
{
   size_t mdiaLen;
   const uint8_t *mdia = find_box(trak, trakLen, FOURCC('m','d','i','a'), &mdiaLen, NULL);
   if (!mdia) return 1;  // no es la pista buscada

   size_t hdlrLen;
   const uint8_t *hdlr = find_box(mdia, mdiaLen, FOURCC('h','d','l','r'), &hdlrLen, NULL);
   if (!hdlr || hdlrLen < 12 || be32(hdlr + 8) != FOURCC('v','i','d','e')) return 1;

   size_t mdhdLen;
   const uint8_t *mdhd = find_box(mdia, mdiaLen, FOURCC('m','d','h','d'), &mdhdLen, NULL);
   if (!mdhd || mdhdLen < 4) { ERR("sin mdhd"); return -1; }
   uint8_t mdhdVer = mdhd[0];
   uint32_t timescale;
   if (mdhdVer == 1) {
      if (mdhdLen < 28) { ERR("mdhd v1 corto"); return -1; }
      timescale = be32(mdhd + 20);
   } else {
      if (mdhdLen < 20) { ERR("mdhd v0 corto"); return -1; }
      timescale = be32(mdhd + 12);
   }
   if (timescale == 0) { ERR("timescale 0"); return -1; }

   size_t minfLen, stblLen, stsdLen;
   const uint8_t *minf = find_box(mdia, mdiaLen, FOURCC('m','i','n','f'), &minfLen, NULL);
   if (!minf) { ERR("sin minf"); return -1; }
   const uint8_t *stbl = find_box(minf, minfLen, FOURCC('s','t','b','l'), &stblLen, NULL);
   if (!stbl) { ERR("sin stbl"); return -1; }
   const uint8_t *stsd = find_box(stbl, stblLen, FOURCC('s','t','s','d'), &stsdLen, NULL);
   if (!stsd || stsdLen < 16) { ERR("sin stsd"); return -1; }

   // stsd: version+flags(4) entry_count(4), luego el primer sample entry
   const uint8_t *entry = stsd + 8;
   size_t entryAvail = stsdLen - 8;
   if (entryAvail < 8) { ERR("stsd sin entries"); return -1; }
   uint32_t entrySize = be32(entry);
   uint32_t entryType = be32(entry + 4);
   if (entrySize > entryAvail) { ERR("sample entry truncado"); return -1; }

   if (entryType != FOURCC('a','v','c','1') && entryType != FOURCC('a','v','c','3')) {
      ERR("codec no es H.264 avc1/avc3 (fourcc 0x%08x) — reencodea con libx264",
          (unsigned)entryType);
      return -1;
   }
   if (entryType == FOURCC('a','v','c','3')) {
      // avc3 lleva los parameter sets inline en el stream; el spike espera avc1.
      ERR("avc3 (parameter sets inline) no soportado por el spike; usa avc1");
      return -1;
   }

   // VisualSampleEntry: tras el header de 8 hay 78 bytes fijos antes de los hijos
   if (entrySize < 86 + 8) { ERR("visual sample entry corto"); return -1; }
   v->width = be16(entry + 8 + 24);
   v->height = be16(entry + 8 + 26);

   size_t avccLen;
   const uint8_t *avcc = find_box(entry + 86, entrySize - 86,
                                  FOURCC('a','v','c','C'), &avccLen, NULL);
   if (!avcc) { ERR("sin avcC"); return -1; }
   if (parse_avcc(avcc, avccLen, v, errbuf, errlen) != 0) return -1;

   if (build_samples(stbl, stblLen, timescale, v, errbuf, errlen) != 0) return -1;

   return 0;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
int mp4_parse(const char *path, Mp4Video *out, char *errbuf, size_t errlen)
{
   memset(out, 0, sizeof(*out));

   FILE *f = fopen(path, "rb");
   if (!f) { ERR("no se pudo abrir %s", path); return -1; }

   // Buscar el box moov a nivel raíz y cargarlo entero en memoria
   uint8_t hdr[16];
   uint8_t *moov = NULL;
   size_t moovLen = 0;
   long pos = 0;

   for (;;) {
      if (fseek(f, pos, SEEK_SET) != 0) break;
      if (fread(hdr, 1, 8, f) != 8) break;

      uint64_t boxSize = be32(hdr);
      uint32_t boxType = be32(hdr + 4);
      size_t hdrSize = 8;

      if (boxSize == 1) {
         if (fread(hdr + 8, 1, 8, f) != 8) break;
         boxSize = be64(hdr + 8);
         hdrSize = 16;
      } else if (boxSize == 0) {
         // spec: el último box puede llevar size 0 = "hasta el final del archivo"
         if (fseek(f, 0, SEEK_END) != 0) break;
         long fend = ftell(f);
         if (fend < 0 || fend <= pos + (long)hdrSize) break;
         boxSize = (uint64_t)(fend - pos);
         if (fseek(f, pos + (long)hdrSize, SEEK_SET) != 0) break;
      }
      if (boxSize < hdrSize) break;

      if (boxType == FOURCC('m','o','o','v')) {
         moovLen = (size_t)(boxSize - hdrSize);
         if (moovLen > 256u * 1024 * 1024) { ERR("moov > 256 MB"); fclose(f); return -1; }
         moov = malloc(moovLen);
         if (!moov) { ERR("sin memoria para moov (%u bytes)", (unsigned)moovLen); fclose(f); return -1; }
         if (fread(moov, 1, moovLen, f) != moovLen) {
            ERR("lectura de moov incompleta");
            free(moov); fclose(f); return -1;
         }
         break;
      }

      if (pos + (long)boxSize <= pos) break;  // overflow / archivo > 2 GB
      pos += (long)boxSize;
   }

   fclose(f);
   if (!moov) { ERR("no se encontro moov (¿MP4 fragmentado o corrupto?)"); return -1; }

   // Iterar los trak hasta dar con la pista de vídeo H.264
   const uint8_t *iter = NULL;
   size_t trakLen;
   const uint8_t *trak;
   int found = 0;
   char lastErr[128] = "sin trak de video H.264";

   while ((trak = next_box_of(moov, moovLen, FOURCC('t','r','a','k'), &trakLen, &iter)) != NULL) {
      char tErr[128] = "";
      int rc = parse_trak(trak, trakLen, out, tErr, sizeof(tErr));
      if (rc == 0) { found = 1; break; }
      if (rc < 0) {
         snprintf(lastErr, sizeof(lastErr), "%s", tErr);
         mp4_free(out);
         memset(out, 0, sizeof(*out));
      }
   }

   free(moov);

   if (!found) { ERR("%s", lastErr); return -1; }
   return 0;
}

// ---------------------------------------------------------------------------
// Pista de audio: mp4a + esds -> AudioSpecificConfig
// ---------------------------------------------------------------------------

// Los descriptores MPEG-4 llevan la longitud en base-128, 7 bits por byte.
static uint32_t read_descr_len(const uint8_t *p, size_t avail, uint32_t *outBytes)
{
   uint32_t len = 0, used = 0;
   while (used < 4 && used < avail) {
      uint8_t b = p[used++];
      len = (len << 7) | (b & 0x7F);
      if (!(b & 0x80)) break;
   }
   *outBytes = used;
   return len;
}

// Recorre el esds hasta el DecoderSpecificInfo (tag 0x05), que ES el
// AudioSpecificConfig que faad2 necesita para inicializarse.
static int parse_esds(const uint8_t *p, size_t len, Mp4Audio *a)
{
   if (len < 5) return -1;
   size_t pos = 4;   // version + flags del FullBox

   while (pos + 2 <= len) {
      uint8_t tag = p[pos++];
      uint32_t lenBytes = 0;
      uint32_t dlen = read_descr_len(p + pos, len - pos, &lenBytes);
      pos += lenBytes;
      if (pos + dlen > len) return -1;

      if (tag == 0x03) {            // ES_Descriptor: saltar ES_ID + flags
         if (pos + 3 > len) return -1;
         uint8_t flags = p[pos + 2];
         pos += 3;
         if (flags & 0x80) pos += 2;                        // dependsOn
         if (flags & 0x40) { if (pos >= len) return -1; pos += 1 + p[pos]; }  // URL
         if (flags & 0x20) pos += 2;                        // OCR
         continue;                  // seguir hacia el DecoderConfigDescriptor
      }
      if (tag == 0x04) {            // DecoderConfigDescriptor
         if (pos + 13 > len) return -1;
         // objectTypeIndication 0x40 = AAC (MPEG-4 Audio)
         if (p[pos] != 0x40) return -1;
         pos += 13;
         continue;                  // seguir hacia el DecSpecificInfo
      }
      if (tag == 0x05) {            // DecoderSpecificInfo = AudioSpecificConfig
         if (dlen == 0 || dlen > sizeof(a->asc)) return -1;
         memcpy(a->asc, p + pos, dlen);
         a->ascSize = dlen;
         return 0;
      }
      pos += dlen;                  // descriptor que no nos interesa
   }
   return -1;
}

// Devuelve 0 si este trak es la pista de audio y se parseó; 1 si no lo es;
// <0 si lo es pero está rota o no es compatible.
static int parse_audio_trak(const uint8_t *trak, size_t trakLen, Mp4Audio *a,
                            char *errbuf, size_t errlen)
{
   size_t mdiaLen;
   const uint8_t *mdia = find_box(trak, trakLen, FOURCC('m','d','i','a'), &mdiaLen, NULL);
   if (!mdia) return 1;

   size_t hdlrLen;
   const uint8_t *hdlr = find_box(mdia, mdiaLen, FOURCC('h','d','l','r'), &hdlrLen, NULL);
   if (!hdlr || hdlrLen < 12 || be32(hdlr + 8) != FOURCC('s','o','u','n')) return 1;

   size_t mdhdLen;
   const uint8_t *mdhd = find_box(mdia, mdiaLen, FOURCC('m','d','h','d'), &mdhdLen, NULL);
   if (!mdhd || mdhdLen < 4) { ERR("audio sin mdhd"); return -1; }
   uint32_t timescale;
   if (mdhd[0] == 1) {
      if (mdhdLen < 28) { ERR("audio mdhd v1 corto"); return -1; }
      timescale = be32(mdhd + 20);
   } else {
      if (mdhdLen < 20) { ERR("audio mdhd v0 corto"); return -1; }
      timescale = be32(mdhd + 12);
   }
   if (timescale == 0) { ERR("audio timescale 0"); return -1; }

   size_t minfLen, stblLen, stsdLen;
   const uint8_t *minf = find_box(mdia, mdiaLen, FOURCC('m','i','n','f'), &minfLen, NULL);
   if (!minf) { ERR("audio sin minf"); return -1; }
   const uint8_t *stbl = find_box(minf, minfLen, FOURCC('s','t','b','l'), &stblLen, NULL);
   if (!stbl) { ERR("audio sin stbl"); return -1; }
   const uint8_t *stsd = find_box(stbl, stblLen, FOURCC('s','t','s','d'), &stsdLen, NULL);
   if (!stsd || stsdLen < 16) { ERR("audio sin stsd"); return -1; }

   const uint8_t *entry = stsd + 8;
   size_t entryAvail = stsdLen - 8;
   if (entryAvail < 8) { ERR("audio stsd vacio"); return -1; }
   uint32_t entrySize = be32(entry);
   uint32_t entryType = be32(entry + 4);
   if (entrySize > entryAvail) { ERR("audio sample entry truncado"); return -1; }

   if (entryType != FOURCC('m','p','4','a')) {
      ERR("pista de audio no es AAC (fourcc 0x%08x)", (unsigned)entryType);
      return -1;
   }

   // AudioSampleEntry: 8 (header) + 8 (reserved+dataRefIndex) + 8 (version,
   // revision, vendor) + 2 canales + 2 bits/sample + 2 pre_defined +
   // 2 reserved + 4 sampleRate(16.16) = los hijos empiezan en +36
   if (entrySize < 36) { ERR("audio sample entry corto"); return -1; }
   a->channels   = be16(entry + 24);
   a->sampleRate = (int)(be32(entry + 32) >> 16);

   size_t esdsLen;
   const uint8_t *esds = find_box(entry + 36, entrySize - 36,
                                  FOURCC('e','s','d','s'), &esdsLen, NULL);
   if (!esds) { ERR("audio sin esds"); return -1; }
   if (parse_esds(esds, esdsLen, a) != 0) {
      ERR("no se pudo leer la configuracion del audio (¿no es AAC-LC?)");
      return -1;
   }

   // La tabla de samples se construye con el mismo código que la de vídeo:
   // Mp4Video y Mp4Audio comparten la forma de los campos que usa.
   Mp4Video tmp;
   memset(&tmp, 0, sizeof(tmp));
   if (build_samples(stbl, stblLen, timescale, &tmp, errbuf, errlen) != 0) {
      free(tmp.samples);
      return -1;
   }
   a->samples       = tmp.samples;
   a->sampleCount   = tmp.sampleCount;
   a->maxSampleSize = tmp.maxSampleSize;
   a->duration      = tmp.duration;
   a->codec         = MP4_AUDIO_AAC;
   return 0;
}

// ---------------------------------------------------------------------------
// Variante sobre memoria: para reproducir mientras se descarga. `data` es el
// principio del archivo y `len` lo que se lleva descargado, así que el moov
// tiene que estar dentro de ese prefijo (MP4 con `-movflags +faststart`).
// ---------------------------------------------------------------------------
int mp4_parse_memory(const uint8_t *data, uint32_t len, Mp4Video *out,
                     char *errbuf, size_t errlen)
{
   return mp4_parse_memory_av(data, len, out, NULL, errbuf, errlen);
}

int mp4_parse_memory_av(const uint8_t *data, uint32_t len,
                        Mp4Video *out, Mp4Audio *audioOut,
                        char *errbuf, size_t errlen)
{
   memset(out, 0, sizeof(*out));
   if (audioOut) memset(audioOut, 0, sizeof(*audioOut));
   if (!data || len < 16) { ERR("aun no hay datos suficientes"); return -1; }

   const uint8_t *moov = NULL;
   size_t moovLen = 0;
   uint32_t pos = 0;
   int sawTruncatedMoov = 0;

   while (pos + 8 <= len) {
      uint64_t boxSize = be32(data + pos);
      uint32_t boxType = be32(data + pos + 4);
      uint32_t hdrSize = 8;

      if (boxSize == 1) {
         if (pos + 16 > len) break;
         boxSize = be64(data + pos + 8);
         hdrSize = 16;
      } else if (boxSize == 0) {
         boxSize = len - pos;   // "hasta el final"
      }
      if (boxSize < hdrSize) break;

      if (boxType == FOURCC('m','o','o','v')) {
         if (pos + boxSize > len) { sawTruncatedMoov = 1; break; }  // aún llegando
         moov = data + pos + hdrSize;
         moovLen = (size_t)(boxSize - hdrSize);
         break;
      }

      if (boxSize > len - pos) break;   // box que se extiende más allá de lo bajado
      pos += (uint32_t)boxSize;
   }

   if (!moov) {
      if (sawTruncatedMoov) ERR("descargando el indice del video...");
      else ERR("indice no encontrado al principio: reencodea con -movflags +faststart");
      return -1;
   }

   const uint8_t *iter = NULL;
   size_t trakLen;
   const uint8_t *trak;
   int found = 0;
   char lastErr[128] = "sin trak de video H.264";

   while ((trak = next_box_of(moov, moovLen, FOURCC('t','r','a','k'), &trakLen, &iter)) != NULL) {
      char tErr[128] = "";
      int rc = parse_trak(trak, trakLen, out, tErr, sizeof(tErr));
      if (rc == 0) { found = 1; break; }
      if (rc < 0) {
         snprintf(lastErr, sizeof(lastErr), "%s", tErr);
         mp4_free(out);
         memset(out, 0, sizeof(*out));
      }
   }

   if (!found) { ERR("%s", lastErr); return -1; }

   // El audio es opcional: si falla, el vídeo se reproduce mudo en vez de
   // rechazar el archivo entero.
   if (audioOut) {
      iter = NULL;
      while ((trak = next_box_of(moov, moovLen, FOURCC('t','r','a','k'),
                                 &trakLen, &iter)) != NULL) {
         char aErr[128] = "";
         int rc = parse_audio_trak(trak, trakLen, audioOut, aErr, sizeof(aErr));
         if (rc == 0) break;
         if (rc < 0) {
            mp4_free_audio(audioOut);
            memset(audioOut, 0, sizeof(*audioOut));
         }
      }
   }
   return 0;
}

void mp4_free_audio(Mp4Audio *a)
{
   if (!a) return;
   free(a->samples);
   a->samples = NULL;
   a->sampleCount = 0;
   a->codec = MP4_AUDIO_NONE;
}

void mp4_free(Mp4Video *v)
{
   free(v->samples);
   v->samples = NULL;
   v->sampleCount = 0;
}
