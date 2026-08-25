// ============================================================================
// WiiU Cast — receptor de medios local para Wii U (Aroma)
//
// Fase 1: esqueleto. La consola levanta un servidor HTTP, sirve una web UI al
// teléfono y acepta URLs de medios. La reproducción llega en la Fase 2 (el
// pipeline ya está validado en spikes/s2-decode).
//
// Estado del arte de sockets en esta consola: ver spikes/RESULTADOS.md — las
// reglas de nsysnet que hacen que esto funcione no son las de BSD.
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <stdio.h>
#include <string.h>

#include "net/http_server.h"
#include "ui/screen.h"
#include "web/index.h"

#define HTTP_PORT 8080
#define BG        0x101820FF   // fondo de la UI de consola (RGBX8 big-endian)

// ---------------------------------------------------------------------------
// Estado de la aplicación
// ---------------------------------------------------------------------------
typedef enum {
   APP_IDLE = 0,      // esperando que alguien castee
   APP_RECEIVED,      // URL recibida (en Fase 2: reproduciendo)
   APP_PAUSED,
} AppState;

static struct {
   AppState state;
   char url[512];
   uint32_t casts;
   OSTime lastEvent;
} g_app;

static const char *state_name(AppState s)
{
   switch (s) {
      case APP_RECEIVED: return "Recibido";
      case APP_PAUSED:   return "En pausa";
      default:           return "Esperando";
   }
}

// ---------------------------------------------------------------------------
// Rutas HTTP
// ---------------------------------------------------------------------------

// Escapa lo mínimo para meter una cadena dentro de un string JSON.
static void json_escape(const char *in, char *out, uint32_t cap)
{
   uint32_t o = 0;
   for (uint32_t i = 0; in[i] && o + 7 < cap; i++) {
      unsigned char ch = (unsigned char)in[i];
      if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = ch; }
      else if (ch < 0x20)          { o += snprintf(out + o, cap - o, "\\u%04x", ch); }
      else                          { out[o++] = ch; }
   }
   out[o] = '\0';
}

static int handle_request(const char *method, const char *path, const char *query,
                          const char *body, char *out, uint32_t outCap,
                          const char **contentType)
{
   // --- Página principal
   if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
      *contentType = "text/html; charset=utf-8";
      snprintf(out, outCap, "%s", WEBUI_HTML);
      return 200;
   }

   // --- Estado (lo consulta la web UI cada 2 s)
   if (strcmp(path, "/status") == 0) {
      *contentType = "application/json";
      char escaped[600];
      json_escape(g_app.url, escaped, sizeof(escaped));
      snprintf(out, outCap,
               "{\"state\":\"%s\",\"url\":\"%s\",\"casts\":%u}",
               state_name(g_app.state), escaped, g_app.casts);
      return 200;
   }

   // --- Recibir una URL para reproducir
   if (strcmp(path, "/cast") == 0) {
      *contentType = "text/plain";

      if (strcmp(method, "POST") != 0 || body[0] == '\0') {
         snprintf(out, outCap, "Falta la URL.");
         return 400;
      }
      // Validación temprana: mejor un mensaje claro en el teléfono que un
      // fallo silencioso cuando llegue el reproductor.
      if (strncmp(body, "https://", 8) == 0) {
         snprintf(out, outCap,
                  "HTTPS todavia no: la consola no hace TLS. Usa http://");
         return 400;
      }
      if (strncmp(body, "http://", 7) != 0) {
         snprintf(out, outCap, "La URL tiene que empezar por http://");
         return 400;
      }
      if (strlen(body) >= sizeof(g_app.url)) {
         snprintf(out, outCap, "URL demasiado larga.");
         return 400;
      }
      snprintf(g_app.url, sizeof(g_app.url), "%s", body);
      g_app.state = APP_RECEIVED;
      g_app.casts++;
      g_app.lastEvent = OSGetSystemTime();
      WHBLogPrintf("[cast] %s", g_app.url);
      snprintf(out, outCap, "ok");
      return 200;
   }

   // --- Comandos de reproducción
   if (strcmp(path, "/cmd") == 0) {
      const char *a = strstr(query, "a=");
      *contentType = "text/plain";
      if (!a) { snprintf(out, outCap, "falta ?a="); return 400; }
      a += 2;

      if (strncmp(a, "pause", 5) == 0) {
         if (g_app.state == APP_RECEIVED)    g_app.state = APP_PAUSED;
         else if (g_app.state == APP_PAUSED) g_app.state = APP_RECEIVED;
      } else if (strncmp(a, "stop", 4) == 0) {
         g_app.state = APP_IDLE;
         g_app.url[0] = '\0';
      } else {
         snprintf(out, outCap, "comando desconocido");
         return 400;
      }
      g_app.lastEvent = OSGetSystemTime();
      snprintf(out, outCap, "ok");
      return 200;
   }

   *contentType = "text/plain";
   snprintf(out, outCap, "no encontrado");
   return 404;
}

// ---------------------------------------------------------------------------
// UI de la consola
// ---------------------------------------------------------------------------
static void render(uint32_t ip)
{
   if (!screen_has_foreground()) return;

   char ipstr[16];
   net_ip_str(ip, ipstr);

   screen_begin(BG);

   screen_text(SCREEN_TARGET_BOTH, 2, 1, "WiiU Cast");
   screen_text(SCREEN_TARGET_BOTH, 2, 2, "Receptor de medios local");

   if (ip) {
      screen_textf(SCREEN_TARGET_BOTH, 2, 4, "Abre en el telefono:  http://%s:%u",
                   ipstr, http_server_port());
   } else {
      screen_text(SCREEN_TARGET_BOTH, 2, 4, "Sin red: revisa la conexion de la consola");
   }

   screen_textf(SCREEN_TARGET_BOTH, 2, 6, "Estado:    %s", state_name(g_app.state));
   if (g_app.url[0]) {
      // La URL puede ser más larga que la rejilla del GamePad: se corta el
      // primer tramo para ambas pantallas y el resto va solo a la TV.
      #define URL_CHUNK 40
      char chunk[URL_CHUNK + 1];
      size_t urlLen = strlen(g_app.url);
      size_t first = (urlLen < URL_CHUNK) ? urlLen : URL_CHUNK;
      memcpy(chunk, g_app.url, first);
      chunk[first] = '\0';
      screen_textf(SCREEN_TARGET_BOTH, 2, 7, "Medio:     %s", chunk);
      if (urlLen > first) {
         screen_text(SCREEN_TARGET_TV, 13, 8, g_app.url + first);
      }
      #undef URL_CHUNK
   }

   screen_textf(SCREEN_TARGET_BOTH, 2, 10, "Peticiones: %u   Casts: %u",
                http_server_requests(), g_app.casts);

   screen_text(SCREEN_TARGET_BOTH, 2, 13, "HOME para salir");

   screen_present();
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogUdpInit();
   screen_init();

   WHBLogPrintf("== WiiU Cast (Fase 1) ==");

   net_memory_init();
   uint32_t ip = net_local_ip();

   if (!http_server_start(HTTP_PORT, handle_request)) {
      WHBLogPrintf("[main] no se pudo levantar el servidor HTTP");
   }

   g_app.state = APP_IDLE;
   g_app.lastEvent = OSGetSystemTime();

   while (WHBProcIsRunning()) {
      http_server_poll();
      render(ip);

      // ~60 Hz: suficiente para la UI y deja los 3 núcleos libres. Cuando
      // entre el reproductor (Fase 2) esto pasa a estar dirigido por el vsync.
      OSSleepTicks(OSMillisecondsToTicks(16));
   }

   http_server_stop();
   screen_shutdown();
   WHBProcShutdown();
   return 0;
}
