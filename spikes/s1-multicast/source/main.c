// ============================================================================
// WiiU Cast — Spike S1: recepción multicast (condición C1 del GO)
//
// Pregunta que responde: ¿entrega nsysnet datagramas recibidos en el grupo
// SSDP 239.255.255.250:1900 tras IP_ADD_MEMBERSHIP en hardware real?
//
// Qué hace:
//   1. Abre un socket UDP en :1900 y se une al grupo SSDP 239.255.255.250.
//   2. Abre un socket UDP en :5353 y se une al grupo mDNS 224.0.0.251
//      (grupo de control: 224.0.0.0/24 lo inundan muchos switches sin IGMP;
//      si llega mDNS pero no SSDP, el problema es IGMP/239-8, no la consola).
//   3. Envía un M-SEARCH al grupo DESDE el socket :1900 (prueba la ruta de
//      transmisión; las respuestas de otros renderers DLNA vuelven por
//      unicast al puerto de origen, o sea a este mismo socket).
//   4. Loguea todo paquete recibido con origen y primera línea.
//
// Los paquetes cuyo origen es la PROPIA consola (loopback multicast) se
// cuentan aparte y NO valen para el veredicto: solo el tráfico de otras
// máquinas demuestra recepción real por la red.
//
// Cómo probar: ver spikes/README.md (script tools/s1_send_probes.py en el PC
// + abrir BubbleUPnP / VLC en el teléfono, que emiten M-SEARCH reales).
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <nn/nets2/somemopt.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>   // close() para sockets (devoptab de wut)

#define SSDP_GROUP 0xEFFFFFFA  // 239.255.255.250
#define SSDP_PORT  1900
#define MDNS_GROUP 0xE00000FB  // 224.0.0.251
#define MDNS_PORT  5353

// La Wii U es big-endian: orden de red == orden de host, así que la
// asignación directa de puertos/direcciones es correcta sin htons/htonl.

static uint32_t g_myIp;  // para filtrar el loopback de nuestros propios envíos

// ---------------------------------------------------------------------------
// somemopt: donar 3 MiB al stack de red. Teoría bajo prueba: sin esta
// donación, la copia de datagramas UDP hacia el usuario falla con el error
// nativo 12 (EMSGSIZE) — moonlight/RetroArch/ftpd (que sí reciben UDP en
// hardware) la hacen todos. La llamada INIT bloquea hasta el apagado de
// nsysnet, por eso vive en su propio hilo y el buffer no se libera jamás.
// ---------------------------------------------------------------------------
static OSThread s_memThread;
static void *s_memBuf;

static int somemopt_thread(int argc, const char **argv)
{
   somemopt(SOMEMOPT_REQUEST_INIT, s_memBuf, 0x300000, SOMEMOPT_FLAGS_NONE);
   return 0;
}

static void thread_dealloc(OSThread *t, void *stack) { free(stack); }

static void net_memory_init(void)
{
   s_memBuf = memalign(0x40, 0x300000);
   if (!s_memBuf) {
      WHBLogPrintf("[mem] sin memoria para somemopt");
      return;
   }
   const int stackSize = 128 * 1024;
   uint8_t *stack = memalign(16, stackSize);
   if (!stack) { free(s_memBuf); s_memBuf = NULL; return; }

   if (!OSCreateThread(&s_memThread, somemopt_thread, 0, NULL,
                       stack + stackSize, stackSize, 16,
                       OS_THREAD_ATTRIB_AFFINITY_ANY | OS_THREAD_ATTRIB_DETACHED)) {
      free(stack); free(s_memBuf); s_memBuf = NULL;
      WHBLogPrintf("[mem] OSCreateThread fallo");
      return;
   }
   OSSetThreadName(&s_memThread, "somemopt");
   OSSetThreadDeallocator(&s_memThread, thread_dealloc);
   OSResumeThread(&s_memThread);

   int rc = somemopt(SOMEMOPT_REQUEST_WAIT_FOR_INIT, NULL, 0, SOMEMOPT_FLAGS_NONE);
   WHBLogPrintf("[mem] somemopt 3 MiB donados (wait rc=%d)", rc);
}

typedef struct {
   const char *name;
   int fd;
   uint32_t group;
   uint16_t port;
   int joined;        // resultado de IP_ADD_MEMBERSHIP (0 = ok)
   int joinErrno;
   uint32_t packets;  // paquetes de OTRAS máquinas
   uint32_t msearch;  // M-SEARCH de otras máquinas
   uint32_t replies;  // respuestas HTTP/1.1 200 (renderers contestando)
   uint32_t self;     // loopback de la propia consola (no cuenta)
   uint32_t rxErrLogged;  // errores de recvfrom ya logueados (cap)
} Listener;

static void ip4_str(uint32_t ip, char *out /* >= 16 */)
{
   sprintf(out, "%u.%u.%u.%u",
           (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
           (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
}

// IP local: primero la vía nativa del stack (SO_MYADDR, documentada en
// sys/socket.h de wut) y si falla, el truco UDP-connect + getsockname
// (connect en UDP no envía nada; solo fuerza la selección de ruta).
static uint32_t local_ip(void)
{
   int fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0) return 0;

   uint32_t ip = 0;
   socklen_t len = sizeof(ip);
   if (getsockopt(fd, SOL_SOCKET, SO_MYADDR, &ip, &len) == 0 && ip != 0) {
      close(fd);
      return ip;
   }

   struct sockaddr_in dst;
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = 53;
   dst.sin_addr.s_addr = 0x08080808;  // 8.8.8.8: solo para el lookup de ruta

   ip = 0;
   if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
      struct sockaddr_in me;
      socklen_t mlen = sizeof(me);
      if (getsockname(fd, (struct sockaddr *)&me, &mlen) == 0) {
         ip = me.sin_addr.s_addr;
      }
   }
   close(fd);
   return ip;
}

static int listener_open(Listener *l)
{
   l->fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (l->fd < 0) {
      WHBLogPrintf("[%s] socket() fallo: errno=%d", l->name, errno);
      return -1;
   }

   int one = 1;
   if (setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
      WHBLogPrintf("[%s] SO_REUSEADDR fallo: errno=%d (sigo)", l->name, errno);
   }

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = l->port;
   addr.sin_addr.s_addr = INADDR_ANY;

   if (bind(l->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      WHBLogPrintf("[%s] bind(:%u) fallo: errno=%d", l->name, l->port, errno);
      close(l->fd);
      l->fd = -1;
      return -1;
   }

   if (l->group) {
      struct ip_mreq mreq;
      mreq.imr_multiaddr.s_addr = l->group;
      mreq.imr_interface.s_addr = INADDR_ANY;

      // ============ LA LLAMADA QUE ESTE SPIKE EXISTE PARA PROBAR ============
      int rc = setsockopt(l->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
      l->joined = rc;
      l->joinErrno = (rc == 0) ? 0 : errno;

      char g[16];
      ip4_str(l->group, g);
      WHBLogPrintf("[%s] join %s:%u -> rc=%d errno=%d %s",
                   l->name, g, l->port, rc, l->joinErrno,
                   (rc == 0) ? "(API OK; falta ver si llegan paquetes)" : "(JOIN RECHAZADO)");
   } else {
      WHBLogPrintf("[%s] escucha :%u sin join (control unicast puro)", l->name, l->port);
   }

   // NADA de SO_NONBLOCK: en pruebas reales, recvfrom sobre un socket en ese
   // modo devolvía siempre el error nativo 12 (EMSGSIZE) en nsysnet. El patrón
   // probado (mdnsniff) es select() con timeout cero + recvfrom bloqueante.
   int rusr = 1;
   int rurc = setsockopt(l->fd, SOL_SOCKET, SO_RUSRBUF, &rusr, sizeof(rusr));
   int rcvbuf = 65536;
   int rbrc = setsockopt(l->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
   WHBLogPrintf("[%s] SO_RUSRBUF rc=%d(e%d) SO_RCVBUF rc=%d(e%d)",
                l->name, rurc, rurc ? errno : 0, rbrc, rbrc ? errno : 0);

   return 0;
}

// ¿Hay datos en cola? (select con timeout cero)
static int sock_ready(int fd)
{
   fd_set readfds;
   FD_ZERO(&readfds);
   FD_SET(fd, &readfds);
   struct timeval tv = { 0, 0 };
   return select(fd + 1, &readfds, NULL, NULL, &tv) > 0;
}

static void listener_poll(Listener *l)
{
   if (l->fd < 0) return;

   // Buffer alineado a 64 (0x40): el IPC de nsysnet hacia IOSU es sensible a
   // la alineación del buffer de recepción (patrón tomado de mdnsniff).
   static __attribute__((aligned(64))) char buf[2048];
   struct sockaddr_in src;

   // MATRIZ DE LONGITUDES: los tres métodos (recvfrom±addr, recv) fallan
   // igual con err 12 y el datagrama NO se consume. Teoría: InterNiche
   // devuelve EMSGSIZE si la longitud pedida excede el buffer de recepción
   // del socket. Se prueba en cascada 2047 -> 1460 -> 1024 -> 256; la
   // longitud que funcione (junto al estado de somemopt) delata la causa.
   static const int TRY_LENS[4] = { 2047, 1460, 1024, 256 };

   for (int i = 0; i < 32; i++) {
      if (!sock_ready(l->fd)) break;

      ssize_t n = -1;
      int usedLen = 0;
      int errs[4] = { 0, 0, 0, 0 };
      int consumed = 0;

      for (int k = 0; k < 4; k++) {
         if (k > 0 && !sock_ready(l->fd)) { consumed = 1; break; }
         memset(&src, 0, sizeof(src));
         socklen_t slen = sizeof(src);
         n = recvfrom(l->fd, buf, TRY_LENS[k], 0,
                      (struct sockaddr *)&src, &slen);
         if (n >= 0) { usedLen = TRY_LENS[k]; break; }
         errs[k] = errno;
      }

      if (n < 0) {
         if (l->rxErrLogged < 4) {
            l->rxErrLogged++;
            if (consumed) {
               WHBLogPrintf("[%s] !! e=%d,%d,%d,%d (datagrama consumido)",
                            l->name, errs[0], errs[1], errs[2], errs[3]);
            } else {
               WHBLogPrintf("[%s] !! len2047=%d len1460=%d len1024=%d len256=%d",
                            l->name, errs[0], errs[1], errs[2], errs[3]);
            }
         }
         break;
      }

      if (n == 0) break;

      // Primer paquete OK: decir qué longitud funcionó (esto delata la causa)
      if (l->packets + l->self == 0) {
         WHBLogPrintf("[%s] >> RX OK con len=%d (errnos previos: %d,%d,%d)",
                      l->name, usedLen, errs[0], errs[1], errs[2]);
      }

      buf[n] = '\0';

      // Loopback de nuestros propios envíos multicast: contar aparte,
      // NUNCA como prueba de recepción real.
      if (g_myIp != 0 && src.sin_addr.s_addr == g_myIp) {
         l->self++;
         continue;
      }

      l->packets++;

      int isMSearch = (strncmp(buf, "M-SEARCH", 8) == 0);
      if (isMSearch) l->msearch++;
      if (strncmp(buf, "HTTP/1.1 200", 12) == 0) l->replies++;

      // Primera línea saneada para el log
      char first[61];
      int i;
      for (i = 0; i < 60 && buf[i] && buf[i] != '\r' && buf[i] != '\n'; i++) {
         first[i] = (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.';
      }
      first[i] = '\0';

      char sip[16];
      ip4_str(src.sin_addr.s_addr, sip);
      WHBLogPrintf("[%s] RX #%u %db de %s:%u %s| %s",
                   l->name, l->packets, (int)n, sip, (unsigned)src.sin_port,
                   isMSearch ? "M-SEARCH " : "", first);
   }
}

// Prueba de TRANSMISIÓN multicast, enviada DESDE el socket ya ligado a :1900:
// así cualquier renderer DLNA de la red (TV, PC con VLC) responde por unicast
// al puerto de origen — este mismo socket — y lo veremos como "HTTP/1.1 200".
static void send_msearch(int fd)
{
   static const char msearch[] =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 2\r\n"
      "ST: ssdp:all\r\n"
      "USER-AGENT: wiiucast-spike/0.1\r\n"
      "\r\n";

   if (fd < 0) return;

   struct sockaddr_in dst;
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = SSDP_PORT;
   dst.sin_addr.s_addr = SSDP_GROUP;

   ssize_t n = sendto(fd, msearch, sizeof(msearch) - 1, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
   WHBLogPrintf("[tx] sendto M-SEARCH al grupo -> %d (errno=%d) %s",
                (int)n, (n < 0) ? errno : 0,
                (n > 0) ? "(TX multicast OK)" : "(TX multicast FALLO)");
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogConsoleInit();
   WHBLogUdpInit();  // logs tambien por UDP :4405 -> `udplogserver` en el PC

   WHBLogPrintf("== WiiU Cast S1: multicast RX (SSDP vs mDNS) ==");

   net_memory_init();

   g_myIp = local_ip();
   char ipstr[16];
   ip4_str(g_myIp, ipstr);
   WHBLogPrintf("IP de la consola: %s%s", ipstr,
                g_myIp ? "" : " (!) no detectada: mirala en Ajustes->Internet");
   WHBLogPrintf("Desde el PC: python3 tools/s1_send_probes.py %s", ipstr);
   WHBLogPrintf("Y abre BubbleUPnP / VLC en el telefono (emiten M-SEARCH).");

   Listener ssdp = { .name = "SSDP", .fd = -1, .group = SSDP_GROUP, .port = SSDP_PORT };
   Listener mdns = { .name = "mDNS", .fd = -1, .group = MDNS_GROUP, .port = MDNS_PORT };
   Listener uni  = { .name = "UNI",  .fd = -1, .group = 0,          .port = 1901 };
   listener_open(&ssdp);
   listener_open(&mdns);
   listener_open(&uni);

   send_msearch(ssdp.fd);

   OSTime lastBeat = OSGetSystemTime();

   while (WHBProcIsRunning()) {
      listener_poll(&ssdp);
      listener_poll(&mdns);
      listener_poll(&uni);

      OSTime now = OSGetSystemTime();
      if (OSTicksToSeconds(now - lastBeat) >= 5) {
         lastBeat = now;
         WHBLogPrintf("vivo | SSDP: %u (%u MS, %u resp, %u self) | mDNS: %u | UNI:1901: %u",
                      ssdp.packets, ssdp.msearch, ssdp.replies, ssdp.self,
                      mdns.packets, uni.packets);
      }

      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(50));
   }

   // Veredicto final en el log. Solo cuenta tráfico de OTRAS máquinas:
   // el loopback propio (self) no demuestra nada sobre la red.
   WHBLogPrintf("== RESULTADO S1 ==");
   WHBLogPrintf("SSDP join rc=%d | pkts=%u | M-SEARCH=%u | resp=%u | self=%u",
                ssdp.joined, ssdp.packets, ssdp.msearch, ssdp.replies, ssdp.self);
   WHBLogPrintf("mDNS join rc=%d | pkts=%u | UNI:1901 pkts=%u",
                mdns.joined, mdns.packets, uni.packets);
   if (ssdp.msearch > 0) {
      WHBLogPrintf("C1 CONFIRMADA: la consola recibe M-SEARCH del grupo SSDP.");
   } else if (ssdp.packets > 0) {
      WHBLogPrintf("C1 CONFIRMADA (via NOTIFY/otros): llegan datagramas del grupo");
      WHBLogPrintf("SSDP aunque ningun M-SEARCH cayo en la ventana de prueba.");
   } else if (mdns.packets > 0) {
      WHBLogPrintf("PARCIAL: llega mDNS (224/24) pero no SSDP (239/8) -> problema IGMP.");
   } else {
      WHBLogPrintf("C1 NEGATIVA: sin multicast RX -> fallback web UI + QR / escaneo.");
   }
   WHBLogConsoleDraw();

   if (ssdp.fd >= 0) close(ssdp.fd);
   if (mdns.fd >= 0) close(mdns.fd);
   if (uni.fd >= 0) close(uni.fd);

   WHBLogConsoleFree();
   WHBProcShutdown();
   return 0;
}
