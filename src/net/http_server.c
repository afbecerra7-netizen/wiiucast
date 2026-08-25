#include "http_server.h"

#include <whb/log.h>

#include <coreinit/thread.h>
#include <nn/nets2/somemopt.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// La Wii U es big-endian: orden de red == orden de host, sin htons/htonl.

#define MAX_CLIENTS   6       // select() tope 32 fds; 6 clientes + listener sobra
#define REQ_CAP       4096
#define RESP_CAP      16384
#define CLIENT_TIMEOUT_S 10

typedef enum { CL_FREE = 0, CL_READING, CL_WRITING } ClientState;

typedef struct {
   int fd;
   ClientState state;
   char req[REQ_CAP];
   uint32_t reqLen;
   char resp[RESP_CAP];
   uint32_t respLen, respSent;
   OSTime lastActivity;
} Client;

static int s_listenFd = -1;
static uint16_t s_port;
static HttpHandler s_handler;
static Client s_clients[MAX_CLIENTS];
static uint32_t s_requests;

// ---------------------------------------------------------------------------
// somemopt: donar 3 MiB al stack. Sin esto, SO_RCVBUF falla y el rendimiento
// de red cae. La llamada INIT bloquea hasta el apagado de nsysnet, así que
// vive en su propio hilo y el buffer se cede de por vida (no se libera: un
// free durante el teardown sería una carrera con el runtime).
// ---------------------------------------------------------------------------
static OSThread s_memThread;
static void *s_memBuf;

static int somemopt_thread(int argc, const char **argv)
{
   somemopt(SOMEMOPT_REQUEST_INIT, s_memBuf, 0x300000, SOMEMOPT_FLAGS_NONE);
   return 0;
}

static void thread_dealloc(OSThread *t, void *stack) { free(stack); }

void net_memory_init(void)
{
   s_memBuf = memalign(0x40, 0x300000);
   if (!s_memBuf) { WHBLogPrintf("[net] sin memoria para somemopt"); return; }

   const int stackSize = 128 * 1024;
   uint8_t *stack = memalign(16, stackSize);
   if (!stack) { free(s_memBuf); s_memBuf = NULL; return; }

   if (!OSCreateThread(&s_memThread, somemopt_thread, 0, NULL,
                       stack + stackSize, stackSize, 16,
                       OS_THREAD_ATTRIB_AFFINITY_ANY | OS_THREAD_ATTRIB_DETACHED)) {
      free(stack); free(s_memBuf); s_memBuf = NULL;
      WHBLogPrintf("[net] OSCreateThread(somemopt) fallo");
      return;
   }
   OSSetThreadName(&s_memThread, "somemopt");
   OSSetThreadDeallocator(&s_memThread, thread_dealloc);
   OSResumeThread(&s_memThread);

   somemopt(SOMEMOPT_REQUEST_WAIT_FOR_INIT, NULL, 0, SOMEMOPT_FLAGS_NONE);
   WHBLogPrintf("[net] somemopt: 3 MiB donados al stack");
}

// ---------------------------------------------------------------------------
uint32_t net_local_ip(void)
{
   int fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0) return 0;

   uint32_t ip = 0;
   socklen_t len = sizeof(ip);
   if (getsockopt(fd, SOL_SOCKET, SO_MYADDR, &ip, &len) == 0 && ip != 0) {
      close(fd);
      return ip;
   }

   // Fallback: connect UDP (no envía nada, solo fuerza la selección de ruta)
   struct sockaddr_in dst;
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = 53;
   dst.sin_addr.s_addr = 0x08080808;
   ip = 0;
   if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
      struct sockaddr_in me;
      socklen_t mlen = sizeof(me);
      if (getsockname(fd, (struct sockaddr *)&me, &mlen) == 0) ip = me.sin_addr.s_addr;
   }
   close(fd);
   return ip;
}

void net_ip_str(uint32_t ip, char *out16)
{
   sprintf(out16, "%u.%u.%u.%u",
           (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
           (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
}

// ---------------------------------------------------------------------------
static void client_close(Client *c)
{
   if (c->fd >= 0) close(c->fd);
   c->fd = -1;
   c->state = CL_FREE;
   c->reqLen = c->respLen = c->respSent = 0;
}

BOOL http_server_start(uint16_t port, HttpHandler handler)
{
   s_handler = handler;
   s_port = port;
   s_requests = 0;
   for (int i = 0; i < MAX_CLIENTS; i++) { s_clients[i].fd = -1; s_clients[i].state = CL_FREE; }

   s_listenFd = socket(AF_INET, SOCK_STREAM, 0);
   if (s_listenFd < 0) {
      WHBLogPrintf("[http] socket() fallo: errno=%d", errno);
      return FALSE;
   }

   int one = 1;
   setsockopt(s_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = port;
   addr.sin_addr.s_addr = INADDR_ANY;

   if (bind(s_listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      WHBLogPrintf("[http] bind(:%u) fallo: errno=%d", port, errno);
      close(s_listenFd);
      s_listenFd = -1;
      return FALSE;
   }

   if (listen(s_listenFd, MAX_CLIENTS) != 0) {
      WHBLogPrintf("[http] listen() fallo: errno=%d", errno);
      close(s_listenFd);
      s_listenFd = -1;
      return FALSE;
   }

   WHBLogPrintf("[http] escuchando en :%u", port);
   return TRUE;
}

void http_server_stop(void)
{
   for (int i = 0; i < MAX_CLIENTS; i++) client_close(&s_clients[i]);
   if (s_listenFd >= 0) close(s_listenFd);
   s_listenFd = -1;
}

// ---------------------------------------------------------------------------
// Parseo de la petición y construcción de la respuesta
// ---------------------------------------------------------------------------
static const char *status_text(int code)
{
   switch (code) {
      case 200: return "OK";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 413: return "Payload Too Large";
      default:  return "Internal Server Error";
   }
}

// ¿Están completas las cabeceras? Devuelve el offset del cuerpo, o 0.
static uint32_t headers_end(const char *buf, uint32_t len)
{
   for (uint32_t i = 0; i + 3 < len; i++) {
      if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
         return i + 4;
      }
   }
   return 0;
}

static uint32_t content_length_of(const char *buf, uint32_t hdrEnd)
{
   // Búsqueda case-insensitive de "content-length:"
   static const char *needle = "content-length:";
   for (uint32_t i = 0; i + 15 < hdrEnd; i++) {
      uint32_t k = 0;
      while (k < 15) {
         char a = buf[i + k];
         if (a >= 'A' && a <= 'Z') a += 32;
         if (a != needle[k]) break;
         k++;
      }
      if (k == 15) {
         uint32_t v = 0;
         uint32_t j = i + 15;
         while (j < hdrEnd && (buf[j] == ' ' || buf[j] == '\t')) j++;
         while (j < hdrEnd && buf[j] >= '0' && buf[j] <= '9') { v = v * 10 + (buf[j] - '0'); j++; }
         return v;
      }
   }
   return 0;
}

static void build_response(Client *c)
{
   char method[8] = "", target[512] = "";
   // Línea de petición: "METHOD /path?query HTTP/1.1"
   if (sscanf(c->req, "%7s %511s", method, target) != 2) {
      c->respLen = snprintf(c->resp, RESP_CAP,
                            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                            "Connection: close\r\n\r\n");
      c->respSent = 0;
      c->state = CL_WRITING;
      return;
   }

   char *query = strchr(target, '?');
   if (query) { *query = '\0'; query++; } else { query = target + strlen(target); }

   uint32_t hdrEnd = headers_end(c->req, c->reqLen);
   const char *body = (hdrEnd && hdrEnd <= c->reqLen) ? c->req + hdrEnd : "";

   // El handler escribe el cuerpo directamente en la zona final del buffer de
   // respuesta y luego se antepone la cabecera (evita un buffer intermedio).
   char *bodyOut = c->resp + 512;
   uint32_t bodyCap = RESP_CAP - 512 - 1;
   const char *ctype = "text/plain; charset=utf-8";

   int code = s_handler ? s_handler(method, target, query, body, bodyOut, bodyCap, &ctype)
                        : 404;
   uint32_t bodyLen = (uint32_t)strlen(bodyOut);

   char header[512];
   int hlen = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %u\r\n"
                       "Cache-Control: no-store\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       code, status_text(code), ctype, bodyLen);

   // Mover el cuerpo justo detrás de la cabecera
   memmove(c->resp + hlen, bodyOut, bodyLen);
   memcpy(c->resp, header, hlen);
   c->respLen = hlen + bodyLen;
   c->respSent = 0;
   c->state = CL_WRITING;
   s_requests++;
}

// ---------------------------------------------------------------------------
void http_server_poll(void)
{
   if (s_listenFd < 0) return;

   fd_set readfds, writefds;
   FD_ZERO(&readfds);
   FD_ZERO(&writefds);

   int maxFd = s_listenFd;
   FD_SET(s_listenFd, &readfds);

   for (int i = 0; i < MAX_CLIENTS; i++) {
      Client *c = &s_clients[i];
      if (c->state == CL_READING) FD_SET(c->fd, &readfds);
      else if (c->state == CL_WRITING) FD_SET(c->fd, &writefds);
      else continue;
      if (c->fd > maxFd) maxFd = c->fd;
   }

   struct timeval tv = { 0, 0 };
   if (select(maxFd + 1, &readfds, &writefds, NULL, &tv) <= 0) {
      // Sin actividad: aprovechar para caducar clientes colgados
      OSTime now = OSGetSystemTime();
      for (int i = 0; i < MAX_CLIENTS; i++) {
         Client *c = &s_clients[i];
         if (c->state != CL_FREE &&
             OSTicksToSeconds(now - c->lastActivity) >= CLIENT_TIMEOUT_S) {
            client_close(c);
         }
      }
      return;
   }

   // --- Nueva conexión
   if (FD_ISSET(s_listenFd, &readfds)) {
      struct sockaddr_in peer;
      socklen_t plen = sizeof(peer);
      int fd = accept(s_listenFd, (struct sockaddr *)&peer, &plen);
      if (fd >= 0) {
         Client *slot = NULL;
         for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].state == CL_FREE) { slot = &s_clients[i]; break; }
         }
         if (slot) {
            slot->fd = fd;
            slot->state = CL_READING;
            slot->reqLen = slot->respLen = slot->respSent = 0;
            slot->lastActivity = OSGetSystemTime();
         } else {
            close(fd);  // sin hueco: rechazar limpiamente
         }
      }
   }

   // --- Clientes
   for (int i = 0; i < MAX_CLIENTS; i++) {
      Client *c = &s_clients[i];

      if (c->state == CL_READING && FD_ISSET(c->fd, &readfds)) {
         c->lastActivity = OSGetSystemTime();
         ssize_t n = recv(c->fd, c->req + c->reqLen, REQ_CAP - 1 - c->reqLen, 0);
         if (n <= 0) { client_close(c); continue; }
         c->reqLen += (uint32_t)n;
         c->req[c->reqLen] = '\0';

         uint32_t hdrEnd = headers_end(c->req, c->reqLen);
         if (hdrEnd) {
            uint32_t need = hdrEnd + content_length_of(c->req, hdrEnd);
            if (c->reqLen >= need) {
               build_response(c);
            } else if (need >= REQ_CAP) {
               c->respLen = snprintf(c->resp, RESP_CAP,
                                     "HTTP/1.1 413 Payload Too Large\r\n"
                                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
               c->respSent = 0;
               c->state = CL_WRITING;
            }
         } else if (c->reqLen >= REQ_CAP - 1) {
            client_close(c);  // cabeceras absurdamente largas
         }
      }
      else if (c->state == CL_WRITING && FD_ISSET(c->fd, &writefds)) {
         c->lastActivity = OSGetSystemTime();
         ssize_t n = send(c->fd, c->resp + c->respSent, c->respLen - c->respSent, 0);
         if (n <= 0) { client_close(c); continue; }
         c->respSent += (uint32_t)n;
         if (c->respSent >= c->respLen) client_close(c);  // Connection: close
      }
   }
}

uint32_t http_server_requests(void) { return s_requests; }
uint16_t http_server_port(void) { return s_port; }
