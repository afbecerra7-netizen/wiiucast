#include "http_fetch.h"

#include <whb/log.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/mutex.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// 8 MiB de colchón: a 6 Mbps son ~11 s de vídeo por delante, suficiente para
// absorber el jitter del Wi-Fi de la consola.
#define RING_SIZE     (8 * 1024 * 1024)
#define RECV_CHUNK    (32 * 1024)
#define CONNECT_TIMEOUT_S 10
#define IDLE_TIMEOUT_S    15

static uint8_t *s_ring;
static uint64_t s_windowStart;   // offset del byte más antiguo aún en el ring
static uint64_t s_filled;        // offset del siguiente byte a escribir
static uint64_t s_totalSize;

static volatile FetchState s_state = FETCH_IDLE;
static char s_error[128];
static char s_url[512];

static OSThread s_thread;
static uint8_t *s_stack;
static volatile BOOL s_stopRequested;
// El hilo se bloquea dentro de recv() sin timeout. Para poder cerrarlo hay
// que cerrarle el socket desde fuera: eso hace que recv vuelva y el hilo
// salga. Sin esto, OSJoinThread no retorna nunca y la app se cuelga al salir
// (dejando además la consola inservible para wiiload).
static volatile int s_sock = -1;
static OSMutex s_mutex;
static BOOL s_mutexReady;

// ---------------------------------------------------------------------------
static void set_error(const char *msg)
{
   snprintf(s_error, sizeof(s_error), "%s", msg);
   s_state = FETCH_ERROR;
   WHBLogPrintf("[fetch] error: %s", msg);
}

// http://host[:puerto]/ruta — acepta IP literal o nombre.
static BOOL parse_url(const char *url, char *host, size_t hostCap,
                      uint16_t *port, char *path, size_t pathCap)
{
   if (strncmp(url, "http://", 7) != 0) return FALSE;
   const char *p = url + 7;

   const char *slash = strchr(p, '/');
   const char *hostEnd = slash ? slash : p + strlen(p);

   const char *colon = memchr(p, ':', (size_t)(hostEnd - p));
   size_t hostLen = (size_t)((colon ? colon : hostEnd) - p);
   if (hostLen == 0 || hostLen >= hostCap) return FALSE;
   memcpy(host, p, hostLen);
   host[hostLen] = '\0';

   *port = 80;
   if (colon) {
      unsigned v = 0;
      for (const char *c = colon + 1; c < hostEnd; c++) {
         if (*c < '0' || *c > '9') return FALSE;
         v = v * 10 + (unsigned)(*c - '0');
      }
      if (v == 0 || v > 65535) return FALSE;
      *port = (uint16_t)v;
   }

   snprintf(path, pathCap, "%s", slash ? slash : "/");
   return TRUE;
}

static uint32_t resolve_host(const char *host)
{
   // ¿IP literal?
   unsigned a, b, c, d;
   if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
       a < 256 && b < 256 && c < 256 && d < 256) {
      return (a << 24) | (b << 16) | (c << 8) | d;
   }
   struct hostent *he = gethostbyname(host);
   if (he && he->h_addr_list && he->h_addr_list[0] && he->h_length == 4) {
      uint32_t ip;
      memcpy(&ip, he->h_addr_list[0], 4);
      return ip;   // big-endian == orden de host en esta consola
   }
   return 0;
}

static int connect_timeout(int fd, uint32_t ip, uint16_t port)
{
   struct sockaddr_in dst;
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = port;
   dst.sin_addr.s_addr = ip;
   // El hilo puede bloquearse aquí sin afectar a la UI; el timeout lo pone
   // el propio stack. (SO_NONBLOCK no se usa: rompe la recepción en nsysnet.)
   return connect(fd, (struct sockaddr *)&dst, sizeof(dst));
}

// Espera a que haya sitio en el ring o a que nos pidan parar.
static BOOL wait_for_space(uint32_t need)
{
   while (!s_stopRequested) {
      OSLockMutex(&s_mutex);
      uint64_t used = s_filled - s_windowStart;
      OSUnlockMutex(&s_mutex);
      if (RING_SIZE - used >= need) return TRUE;
      OSSleepTicks(OSMillisecondsToTicks(20));
   }
   return FALSE;
}

static int fetch_thread(int argc, const char **argv)
{
   char host[256], path[512];
   uint16_t port;

   if (!parse_url(s_url, host, sizeof(host), &port, path, sizeof(path))) {
      set_error("URL mal formada");
      return 0;
   }

   s_state = FETCH_CONNECTING;
   uint32_t ip = resolve_host(host);
   if (!ip) { set_error("no se pudo resolver el host"); return 0; }

   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) { set_error("socket() fallo"); return 0; }
   s_sock = fd;

   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_RUSRBUF, &one, sizeof(one));
   setsockopt(fd, SOL_SOCKET, SO_WINSCALE, &one, sizeof(one));
   setsockopt(fd, SOL_SOCKET, SO_NOSLOWSTART, &one, sizeof(one));

   if (connect_timeout(fd, ip, port) != 0) {
      set_error("no se pudo conectar al servidor");
      s_sock = -1; close(fd);
      return 0;
   }

   char req[900];
   int reqLen = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: WiiUCast/0.2\r\n"
                         "Accept: */*\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         path, host);
   if (send(fd, req, reqLen, 0) != reqLen) {
      set_error("fallo al enviar la peticion");
      s_sock = -1; close(fd);
      return 0;
   }

   s_state = FETCH_STREAMING;

   uint8_t chunk[RECV_CHUNK];
   BOOL headersDone = FALSE;
   int hdrState = 0;
   char headerBuf[2048];
   uint32_t headerLen = 0;
   OSTime lastData = OSGetSystemTime();

   while (!s_stopRequested) {
      ssize_t n = recv(fd, chunk, RECV_CHUNK, 0);
      if (n == 0) break;                       // fin de la respuesta
      if (n < 0) {
         if (OSTicksToSeconds(OSGetSystemTime() - lastData) >= IDLE_TIMEOUT_S) {
            set_error("el servidor dejo de enviar datos");
            s_sock = -1; close(fd);
            return 0;
         }
         OSSleepTicks(OSMillisecondsToTicks(5));
         continue;
      }
      lastData = OSGetSystemTime();

      uint32_t dataOff = 0;
      if (!headersDone) {
         for (ssize_t i = 0; i < n; i++) {
            if (headerLen < sizeof(headerBuf) - 1) headerBuf[headerLen++] = (char)chunk[i];
            uint8_t ch = chunk[i];
            if (ch == '\r' && (hdrState == 0 || hdrState == 2)) hdrState++;
            else if (ch == '\n' && (hdrState == 1 || hdrState == 3)) hdrState++;
            else hdrState = (ch == '\r') ? 1 : 0;
            if (hdrState == 4) { headersDone = TRUE; dataOff = (uint32_t)i + 1; break; }
         }
         if (!headersDone) continue;

         headerBuf[headerLen] = '\0';

         // Estado HTTP
         int code = 0;
         sscanf(headerBuf, "HTTP/1.%*d %d", &code);
         if (code < 200 || code >= 300) {
            char msg[128];
            snprintf(msg, sizeof(msg), "el servidor respondio %d", code);
            set_error(msg);
            s_sock = -1; close(fd);
            return 0;
         }

         // Content-Length (búsqueda case-insensitive sencilla)
         for (char *p = headerBuf; *p; p++) {
            if ((p[0] == 'C' || p[0] == 'c') &&
                strncasecmp(p, "content-length:", 15) == 0) {
               s_totalSize = strtoull(p + 15, NULL, 10);
               break;
            }
         }
         WHBLogPrintf("[fetch] %s: %llu bytes", path,
                      (unsigned long long)s_totalSize);
      }

      uint32_t payload = (uint32_t)n - dataOff;
      if (payload == 0) continue;

      if (!wait_for_space(payload)) break;

      OSLockMutex(&s_mutex);
      for (uint32_t i = 0; i < payload; i++) {
         s_ring[(s_filled + i) % RING_SIZE] = chunk[dataOff + i];
      }
      s_filled += payload;
      OSUnlockMutex(&s_mutex);
   }

   s_sock = -1;
   close(fd);
   if (s_state == FETCH_STREAMING) {
      s_state = FETCH_DONE;
      WHBLogPrintf("[fetch] completado: %llu bytes", (unsigned long long)s_filled);
   }
   return 0;
}

static void thread_dealloc(OSThread *t, void *stack) { (void)t; (void)stack; }

BOOL fetch_start(const char *url)
{
   fetch_stop();

   if (!s_mutexReady) { OSInitMutex(&s_mutex); s_mutexReady = TRUE; }
   if (!s_ring) {
      s_ring = memalign(0x40, RING_SIZE);
      if (!s_ring) { set_error("sin memoria para el buffer de red"); return FALSE; }
   }
   if (!s_stack) {
      s_stack = memalign(16, 256 * 1024);
      if (!s_stack) { set_error("sin memoria para el hilo de red"); return FALSE; }
   }

   snprintf(s_url, sizeof(s_url), "%s", url);
   s_windowStart = s_filled = s_totalSize = 0;
   s_error[0] = '\0';
   s_stopRequested = FALSE;
   s_state = FETCH_CONNECTING;

   if (!OSCreateThread(&s_thread, fetch_thread, 0, NULL,
                       s_stack + 256 * 1024, 256 * 1024, 16,
                       OS_THREAD_ATTRIB_AFFINITY_ANY)) {
      set_error("no se pudo crear el hilo de descarga");
      return FALSE;
   }
   OSSetThreadName(&s_thread, "http_fetch");
   OSSetThreadDeallocator(&s_thread, thread_dealloc);
   OSResumeThread(&s_thread);
   return TRUE;
}

void fetch_stop(void)
{
   if (s_state == FETCH_IDLE) return;
   s_stopRequested = TRUE;

   // Desbloquear el recv() del hilo: sin esto OSJoinThread no vuelve nunca.
   int fd = s_sock;
   if (fd >= 0) {
      s_sock = -1;
      shutdown(fd, SHUT_RDWR);
      close(fd);
   }

   OSJoinThread(&s_thread, NULL);
   s_state = FETCH_IDLE;
   s_windowStart = s_filled = s_totalSize = 0;
}

FetchState fetch_state(void)     { return s_state; }
const char *fetch_error(void)    { return s_error; }
uint64_t fetch_total_size(void)  { return s_totalSize; }
uint64_t fetch_downloaded(void)  { return s_filled; }

uint32_t fetch_available(uint64_t offset)
{
   if (!s_mutexReady) return 0;
   OSLockMutex(&s_mutex);
   uint32_t avail = 0;
   if (offset >= s_windowStart && offset <= s_filled) {
      avail = (uint32_t)(s_filled - offset);
   }
   OSUnlockMutex(&s_mutex);
   return avail;
}

int fetch_read(uint64_t offset, void *buf, uint32_t len)
{
   if (!s_ring || !s_mutexReady) return 0;

   OSLockMutex(&s_mutex);
   int result;
   if (offset < s_windowStart) {
      result = -1;                     // ya sobrescrito: no se puede rebobinar
   } else if (offset >= s_filled) {
      result = 0;                      // todavía no ha llegado
   } else {
      uint32_t avail = (uint32_t)(s_filled - offset);
      uint32_t take = (len < avail) ? len : avail;
      uint8_t *dst = (uint8_t *)buf;
      for (uint32_t i = 0; i < take; i++) {
         dst[i] = s_ring[(offset + i) % RING_SIZE];
      }
      result = (int)take;
   }
   OSUnlockMutex(&s_mutex);
   return result;
}

void fetch_release_until(uint64_t offset)
{
   if (!s_mutexReady) return;
   OSLockMutex(&s_mutex);
   if (offset > s_windowStart) s_windowStart = (offset < s_filled) ? offset : s_filled;
   OSUnlockMutex(&s_mutex);
}
