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
#include <whb/crash.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <stdio.h>
#include <string.h>

#include "media/player.h"
#include "net/http_server.h"
#include "ui/screen.h"
#include "video/renderer.h"
#include "web/index.h"

#define HTTP_PORT 8080
#define BG        0x101820FF   // fondo de la UI de consola (RGBX8 big-endian)

// OSScreen (la UI de texto) y GX2 (el vídeo) no pueden poseer la pantalla a
// la vez: se alterna entre los dos según haya reproducción o no.
typedef enum { DISPLAY_UI = 0, DISPLAY_VIDEO } DisplayMode;

static struct {
   char url[512];
   uint32_t casts;
   DisplayMode display;
   char notice[160];      // último error del reproductor, para la web UI
   OSTime lastEvent;
} g_app;

static const char *state_name(void)
{
   switch (player_state()) {
      case PLAYER_BUFFERING: return "Cargando";
      case PLAYER_PLAYING:   return "Reproduciendo";
      case PLAYER_PAUSED:    return "En pausa";
      case PLAYER_ENDED:     return "Terminado";
      case PLAYER_FAILED:    return "Error";
      default:               return "Esperando";
   }
}

// Cambia entre la UI de texto (OSScreen) y el vídeo (GX2). Cada API reclama
// los scan buffers, así que hay que apagar una antes de encender la otra.
// Lo llama el reproductor (via player_set_display_cb) justo cuando necesita
// la salida de vídeo lista, no el bucle principal: si se hiciera al revés,
// las texturas se crearían antes de que GX2 existiera.
static BOOL set_display(BOOL wantVideo)
{
   DisplayMode mode = wantVideo ? DISPLAY_VIDEO : DISPLAY_UI;
   if (g_app.display == mode) return TRUE;

   WHBLogPrintf("[display] cambiando a %s", wantVideo ? "VIDEO" : "UI");

   if (mode == DISPLAY_VIDEO) {
      screen_shutdown();
      if (!video_renderer_init()) {
         WHBLogPrintf("[display] GX2 fallo; vuelvo a la UI");
         screen_init();
         return FALSE;
      }
   } else {
      video_renderer_shutdown();
      screen_init();
   }
   g_app.display = mode;
   return TRUE;
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
      char escUrl[600], escNote[300];
      json_escape(g_app.url, escUrl, sizeof(escUrl));
      json_escape(player_state() == PLAYER_FAILED ? player_error() : g_app.notice,
                  escNote, sizeof(escNote));
      snprintf(out, outCap,
               "{\"state\":\"%s\",\"url\":\"%s\",\"casts\":%u,"
               "\"pos\":%.1f,\"dur\":%.1f,\"dl\":%d,\"note\":\"%s\"}",
               state_name(), escUrl, g_app.casts,
               player_position(), player_duration(),
               player_progress_pct(), escNote);
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
      g_app.casts++;
      g_app.lastEvent = OSGetSystemTime();
      g_app.notice[0] = '\0';
      WHBLogPrintf("[cast] %s", g_app.url);

      if (!player_play_url(g_app.url)) {
         snprintf(out, outCap, "%s", player_error());
         return 400;
      }
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
         player_toggle_pause();
      } else if (strncmp(a, "stop", 4) == 0) {
         player_stop();
         g_app.url[0] = '\0';
         g_app.notice[0] = '\0';
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

   screen_textf(SCREEN_TARGET_BOTH, 2, 6, "Estado:    %s", state_name());
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

   if (player_state() == PLAYER_FAILED) {
      screen_textf(SCREEN_TARGET_BOTH, 2, 9, "Fallo: %s", player_error());
   } else if (player_state() == PLAYER_BUFFERING) {
      int pct = player_progress_pct();
      if (pct >= 0) screen_textf(SCREEN_TARGET_BOTH, 2, 9, "Descargado: %d%%", pct);
   }

   screen_textf(SCREEN_TARGET_BOTH, 2, 11, "Peticiones: %u   Casts: %u",
                http_server_requests(), g_app.casts);

   screen_text(SCREEN_TARGET_BOTH, 2, 13, "HOME para salir");

   screen_present();
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogUdpInit();
   WHBInitCrashHandler();

   screen_init();

   // Traza de arranque EN PANTALLA: el log por UDP no llega fuera de la
   // consola en esta red, así que cada paso se dibuja y se presenta. Si algo
   // se cuelga, el último renglón visible dice exactamente dónde.
   #define BOOT_STEP(row, txt)                          \
      do {                                              \
         screen_begin(BG);                              \
         screen_text(SCREEN_TARGET_BOTH, 2, 1, "WiiU Cast — arrancando");  \
         for (int _r = 0; _r <= (row); _r++)            \
            screen_text(SCREEN_TARGET_BOTH, 2, 3 + _r, s_bootLog[_r]);     \
         screen_present();                              \
         WHBLogPrintf("[boot] %s", txt);                \
      } while (0)

   static const char *s_bootLog[8];
   s_bootLog[0] = "1/6 proc + screen  OK";
   BOOT_STEP(0, "proc+screen");

   net_memory_init();
   s_bootLog[1] = "2/6 somemopt       OK";
   BOOT_STEP(1, "somemopt");

   uint32_t ip = net_local_ip();
   s_bootLog[2] = "3/6 ip local       OK";
   BOOT_STEP(2, "ip");

   if (!http_server_start(HTTP_PORT, handle_request)) {
      s_bootLog[3] = "4/6 http server    FALLO";
   } else {
      s_bootLog[3] = "4/6 http server    OK";
   }
   BOOT_STEP(3, "http");

   player_init();
   player_set_display_cb(set_display);   // después de init: init limpia el estado
   s_bootLog[4] = "5/6 player         OK";
   BOOT_STEP(4, "player");

   s_bootLog[5] = "6/6 entrando al bucle";
   BOOT_STEP(5, "loop");
   g_app.display = DISPLAY_UI;
   g_app.lastEvent = OSGetSystemTime();

   while (WHBProcIsRunning()) {
      http_server_poll();

      player_update();

      // Si la reproducción murió, devolver la pantalla a la UI de texto.
      PlayerState ps = player_state();
      if (g_app.display == DISPLAY_VIDEO && ps == PLAYER_FAILED) {
         set_display(FALSE);
      }

      if (g_app.display == DISPLAY_VIDEO) {
         video_renderer_draw(TRUE, 0.0f, 0.0f, 0.0f);
         // GX2 ya sincroniza con el vsync: no hace falta dormir aquí.
      } else {
         render(ip);
         OSSleepTicks(OSMillisecondsToTicks(16));
      }
   }

   player_shutdown();
   http_server_stop();
   if (g_app.display == DISPLAY_VIDEO) video_renderer_shutdown();
   else screen_shutdown();
   WHBProcShutdown();
   return 0;
}
