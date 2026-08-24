// ============================================================================
// WiiU Cast — Spike S3: throughput HTTP real (condición C3 del GO)
//
// Pregunta que responde: ¿cuántos Mbps sostenidos da un GET HTTP en hardware
// real, por Wi-Fi y por adaptador LAN, con y sin tuning de sockets?
// El resultado fija el techo de bitrate de vídeo anunciable del producto.
//
// Qué hace:
//   1. Dona 3 MiB al stack de red vía somemopt (patrón moonlight/RetroArch).
//   2. Lee la URL de sd:/wiiucast/s3-url.txt (solo http:// con IP literal).
//   3. Pasada A: GET con opciones por defecto.
//      Pasada B: GET con SO_RUSRBUF + SO_WINSCALE + SO_NOSLOWSTART.
//   4. Reporta Mbps instantáneos cada segundo y la media de cada pasada.
//
// Todos los sockets van en modo NO bloqueante: un servidor colgado o una IP
// muerta jamás debe dejar la consola sin responder al botón HOME.
//
// Servidor en el PC:  python3 -m http.server 8000   (ver spikes/README.md)
// Ejecutar el spike dos veces: consola por Wi-Fi y consola por LAN adapter.
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/sdcard.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <nn/nets2/somemopt.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>   // close() para sockets (devoptab de wut)

#define SOMEMOPT_SIZE     0x300000   // máximo documentado
#define RECV_CHUNK        (64 * 1024)
#define MAX_BYTES         (64u * 1024 * 1024)  // parar a los 64 MB...
#define MAX_SECONDS       25                   // ...o 25 s, lo que llegue antes
#define CONNECT_TIMEOUT_S 10
#define IDLE_TIMEOUT_S    8   // sin datos durante 8 s -> abortar la pasada

// La Wii U es big-endian: orden de red == orden de host (sin htons/htonl).

// ---------------------------------------------------------------------------
// somemopt: donar memoria al stack. La llamada INIT bloquea hasta que nsysnet
// se apaga, así que vive en su propio hilo. El buffer se asigna aquí (hilo
// principal) y se cede de por vida: NO se libera nunca — INIT solo retorna
// durante el teardown del proceso y un free ahí es una carrera con el runtime
// (por eso moonlight tampoco lo libera).
// ---------------------------------------------------------------------------
static OSThread s_memThread;
static void *s_memBuf;

static int somemopt_thread(int argc, const char **argv)
{
   somemopt(SOMEMOPT_REQUEST_INIT, s_memBuf, SOMEMOPT_SIZE, SOMEMOPT_FLAGS_NONE);
   return 0;
}

static void thread_dealloc(OSThread *t, void *stack) { free(stack); }

static void net_memory_init(void)
{
   s_memBuf = memalign(0x40, SOMEMOPT_SIZE);
   if (!s_memBuf) {
      WHBLogPrintf("[mem] sin memoria para somemopt; sigo sin tuning de buffers");
      return;
   }

   const int stackSize = 128 * 1024;
   uint8_t *stack = memalign(16, stackSize);
   if (!stack) {
      free(s_memBuf);
      s_memBuf = NULL;
      return;
   }

   if (!OSCreateThread(&s_memThread, somemopt_thread, 0, NULL,
                       stack + stackSize, stackSize, 16,
                       OS_THREAD_ATTRIB_AFFINITY_ANY | OS_THREAD_ATTRIB_DETACHED)) {
      free(stack);
      free(s_memBuf);
      s_memBuf = NULL;
      WHBLogPrintf("[mem] OSCreateThread fallo; sigo sin somemopt");
      return;
   }
   OSSetThreadName(&s_memThread, "somemopt");
   OSSetThreadDeallocator(&s_memThread, thread_dealloc);
   OSResumeThread(&s_memThread);

   int rc = somemopt(SOMEMOPT_REQUEST_WAIT_FOR_INIT, NULL, 0, SOMEMOPT_FLAGS_NONE);
   WHBLogPrintf("[mem] somemopt 3 MiB donados al stack (wait rc=%d)", rc);
}

// ---------------------------------------------------------------------------
// Parseo trivial de http://a.b.c.d[:puerto]/ruta  (IP literal, sin DNS)
// ---------------------------------------------------------------------------
static int parse_url(const char *url, uint32_t *ip, uint16_t *port, char *path, size_t pathLen)
{
   unsigned a, b, c, d, p = 80;
   char rest[256] = "/";

   // %254s: rest[0] es '/', quedan 255 bytes desde rest+1 y sscanf escribe
   // hasta width+1 (el NUL) — con 255 se saldría del array.
   if (sscanf(url, "http://%u.%u.%u.%u:%u/%254s", &a, &b, &c, &d, &p, rest + 1) >= 5 ||
       sscanf(url, "http://%u.%u.%u.%u/%254s", &a, &b, &c, &d, rest + 1) >= 4 ||
       sscanf(url, "http://%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) == 5 ||
       sscanf(url, "http://%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      if (a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) return -1;
      *ip = (a << 24) | (b << 16) | (c << 8) | d;
      *port = (uint16_t)p;
      snprintf(path, pathLen, "%s", rest);
      return 0;
   }
   return -1;
}

// connect() no bloqueante con timeout: tras EINPROGRESS se reintenta connect
// hasta EISCONN (patrón InterNiche/embedded, sin depender de select).
static int connect_with_timeout(int fd, struct sockaddr_in *dst)
{
   int rc = connect(fd, (struct sockaddr *)dst, sizeof(*dst));
   if (rc == 0) return 0;
   if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) return -1;

   OSTime start = OSGetSystemTime();
   while (WHBProcIsRunning() &&
          OSTicksToSeconds(OSGetSystemTime() - start) < CONNECT_TIMEOUT_S) {
      rc = connect(fd, (struct sockaddr *)dst, sizeof(*dst));
      if (rc == 0 || errno == EISCONN) return 0;      // conectado
      if (errno != EINPROGRESS && errno != EALREADY &&
          errno != EWOULDBLOCK && errno != EAGAIN) return -1;
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(50));
   }
   errno = ETIMEDOUT;
   return -1;
}

// ---------------------------------------------------------------------------
// Una pasada de descarga. tuned=1 activa SO_RUSRBUF/WINSCALE/NOSLOWSTART.
// Devuelve Mbps medios (0 si falló).
// ---------------------------------------------------------------------------
static double run_download(uint32_t ip, uint16_t port, const char *path, int tuned)
{
   WHBLogPrintf("---- pasada %s ----", tuned ? "B (tuned)" : "A (default)");

   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      WHBLogPrintf("socket() fallo: errno=%d", errno);
      return 0;
   }

   int one = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one)) != 0) {
      WHBLogPrintf("SO_NONBLOCK fallo: errno=%d (riesgo de bloqueo)", errno);
   }

   if (tuned) {
      int r1 = setsockopt(fd, SOL_SOCKET, SO_RUSRBUF, &one, sizeof(one));
      int r2 = setsockopt(fd, SOL_SOCKET, SO_WINSCALE, &one, sizeof(one));
      int r3 = setsockopt(fd, SOL_SOCKET, SO_NOSLOWSTART, &one, sizeof(one));
      WHBLogPrintf("SO_RUSRBUF=%d SO_WINSCALE=%d SO_NOSLOWSTART=%d (0=ok)", r1, r2, r3);
   }

   struct sockaddr_in dst;
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = port;
   dst.sin_addr.s_addr = ip;

   if (connect_with_timeout(fd, &dst) != 0) {
      WHBLogPrintf("connect() fallo: errno=%d (¿servidor arrancado? ¿IP bien?)", errno);
      close(fd);
      return 0;
   }

   char req[512];
   int reqLen = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %u.%u.%u.%u\r\n"
                         "Connection: close\r\n"
                         "User-Agent: wiiucast-s3/0.1\r\n"
                         "\r\n",
                         path,
                         (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
                         (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
   // el request cabe de sobra en el buffer de envío; un send parcial aquí
   // sería anómalo y se trata como fallo de la pasada
   if (send(fd, req, reqLen, 0) != reqLen) {
      WHBLogPrintf("send() fallo: errno=%d", errno);
      close(fd);
      return 0;
   }

   uint8_t *buf = malloc(RECV_CHUNK);
   if (!buf) { close(fd); return 0; }

   uint64_t body = 0;
   int headersDone = 0;
   int httpOkChecked = 0;
   int hdrState = 0;  // bytes de "\r\n\r\n" ya emparejados (sobrevive entre chunks)
   int aborted = 0;

   OSTime start = OSGetSystemTime();
   OSTime lastReport = start;
   OSTime lastData = start;
   uint64_t lastBody = 0;

   for (;;) {
      if (!WHBProcIsRunning()) { aborted = 1; break; }

      OSTime now = OSGetSystemTime();
      if (OSTicksToSeconds(now - start) >= MAX_SECONDS) break;
      if (OSTicksToSeconds(now - lastData) >= IDLE_TIMEOUT_S) {
         WHBLogPrintf("sin datos durante %d s -> aborto la pasada", IDLE_TIMEOUT_S);
         aborted = 1;
         break;
      }

      ssize_t n = recv(fd, buf, RECV_CHUNK, 0);
      if (n == 0) break;  // EOF limpio
      if (n < 0) {
         if (errno == EWOULDBLOCK || errno == EAGAIN) {
            WHBLogConsoleDraw();
            OSSleepTicks(OSMillisecondsToTicks(1));
            continue;
         }
         WHBLogPrintf("recv() fallo: errno=%d", errno);
         aborted = 1;
         break;
      }
      lastData = OSGetSystemTime();

      size_t dataOff = 0;
      if (!headersDone) {
         if (!httpOkChecked) {
            httpOkChecked = 1;
            if (n >= 12 && memcmp(buf, "HTTP/1.", 7) == 0 && buf[9] != '2') {
               buf[(n < 64) ? n : 64] = 0;
               WHBLogPrintf("respuesta no-2xx: %.32s", (char *)buf);
               aborted = 1;
               break;
            }
         }
         // Máquina de estados para \r\n\r\n: funciona aunque la secuencia
         // caiga partida entre dos chunks de recv.
         for (ssize_t i = 0; i < n; i++) {
            uint8_t ch = buf[i];
            if (ch == '\r' && (hdrState == 0 || hdrState == 2)) {
               hdrState++;
            } else if (ch == '\n' && (hdrState == 1 || hdrState == 3)) {
               hdrState++;
            } else {
               hdrState = (ch == '\r') ? 1 : 0;
            }
            if (hdrState == 4) {
               headersDone = 1;
               dataOff = (size_t)i + 1;
               break;
            }
         }
         if (!headersDone) continue;  // este chunk era todo cabeceras
         // el cronómetro de la descarga empieza donde empieza el cuerpo
         start = lastReport = lastData = OSGetSystemTime();
      }

      body += (uint64_t)n - dataOff;

      OSTime now2 = OSGetSystemTime();
      if (OSTicksToMilliseconds(now2 - lastReport) >= 1000) {
         double instMbps = (body - lastBody) * 8.0 /
                           (OSTicksToMicroseconds(now2 - lastReport) / 1e6) / 1e6;
         WHBLogPrintf("  %llu KB | %.2f Mbps", (unsigned long long)(body / 1024), instMbps);
         WHBLogConsoleDraw();
         lastReport = now2;
         lastBody = body;
      }

      if (body >= MAX_BYTES) break;
   }

   OSTime end = OSGetSystemTime();
   double secs = OSTicksToMicroseconds(end - start) / 1e6;
   double mbps = (secs > 0.5 && body > 0) ? body * 8.0 / secs / 1e6 : 0;

   WHBLogPrintf("pasada %s: %llu KB en %.1f s = %.2f Mbps (%.0f KB/s)%s",
                tuned ? "B" : "A",
                (unsigned long long)(body / 1024), secs, mbps,
                (secs > 0.1) ? body / 1024.0 / secs : 0.0,
                aborted ? " [ABORTADA]" : "");

   free(buf);
   close(fd);
   return mbps;
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogConsoleInit();
   WHBLogUdpInit();

   WHBLogPrintf("== WiiU Cast S3: throughput HTTP ==");

   net_memory_init();

   char url[300] = "";
   if (WHBMountSdCard()) {
      char cfgPath[320];
      snprintf(cfgPath, sizeof(cfgPath), "%s/wiiucast/s3-url.txt", WHBGetSdCardMountPath());
      FILE *f = fopen(cfgPath, "rb");
      if (f) {
         size_t n = fread(url, 1, sizeof(url) - 1, f);
         fclose(f);
         url[n] = 0;
         // recortar espacios/saltos finales
         while (n > 0 && (url[n-1] == '\n' || url[n-1] == '\r' || url[n-1] == ' ')) {
            url[--n] = 0;
         }
      }
   }

   uint32_t ip;
   uint16_t port;
   char path[260];
   if (!url[0] || parse_url(url, &ip, &port, path, sizeof(path)) != 0) {
      WHBLogPrintf("FATAL: crea sd:/wiiucast/s3-url.txt con una linea:");
      WHBLogPrintf("  http://IP_DEL_PC:8000/test.bin");
      WHBLogPrintf("(en el PC: python3 -m http.server 8000, ver README)");
      while (WHBProcIsRunning()) {
         WHBLogConsoleDraw();
         OSSleepTicks(OSMillisecondsToTicks(100));
      }
      goto shutdown;
   }

   WHBLogPrintf("URL: %s", url);
   WHBLogConsoleDraw();

   {
      double a = run_download(ip, port, path, 0);
      WHBLogConsoleDraw();
      double b = WHBProcIsRunning() ? run_download(ip, port, path, 1) : 0;

      WHBLogPrintf("== RESULTADO S3 ==");
      WHBLogPrintf("default: %.2f Mbps | tuned: %.2f Mbps", a, b);
      double best = (b > a) ? b : a;
      WHBLogPrintf("techo de bitrate de video seguro: ~70%% de %.1f = %.1f Mbps",
                   best, best * 0.7);
      WHBLogPrintf("Repite esta prueba en Wi-Fi Y con LAN adapter, y anota ambas");
      WHBLogPrintf("en spikes/RESULTADOS.md.");
   }

   while (WHBProcIsRunning()) {
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(100));
   }

shutdown:
   WHBUnmountSdCard();
   WHBLogConsoleFree();
   WHBProcShutdown();
   return 0;
}
