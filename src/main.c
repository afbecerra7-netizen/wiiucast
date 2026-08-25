// ============================================================================
// WiiU Cast — receptor de medios local para Wii U (Aroma)
//
// La consola sirve una web UI al teléfono, recibe una URL y reproduce el
// vídeo en la TV y el GamePad.
//
// GX2 es dueño ÚNICO de la pantalla: la UI de texto se dibuja dentro de un
// frame NV12 (texto en el plano Y, UV neutro) y sale por el mismo shader que
// el vídeo. Alternar con OSScreen dejaba la TV en negro — las dos APIs se
// disputaban los scan buffers y el frame heap de MEM1.
// ============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/crash.h>

#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <proc_ui/procui.h>

#include <stdio.h>
#include <string.h>

#include "media/player.h"
#include "net/http_server.h"
#include "ui/overlay.h"
#include "video/renderer.h"
#include "web/index.h"

#define HTTP_PORT 8080

// Lienzo de la UI. 854x480 es la resolución nativa del GamePad y escala
// limpio a la TV; de paso el frame es pequeño y barato de redibujar.
#define UI_W 854
#define UI_H 480

static struct {
   char url[512];
   uint32_t casts;
   char notice[160];
   uint32_t ip;
   BOOL uiDirty;          // hay que redibujar el lienzo de la UI
   PlayerState lastState;
} g_app;

static const char *state_name(void)
{
   switch (player_state()) {
      case PLAYER_BUFFERING: return "Cargando";
      case PLAYER_PLAYING:   return player_is_rebuffering() ? "Cargando..." : "Reproduciendo";
      case PLAYER_PAUSED:    return "En pausa";
      case PLAYER_ENDED:     return "Terminado";
      case PLAYER_FAILED:    return "Error";
      default:               return "Esperando";
   }
}

// ---------------------------------------------------------------------------
// Rutas HTTP
// ---------------------------------------------------------------------------
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
   if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
      *contentType = "text/html; charset=utf-8";
      snprintf(out, outCap, "%s", WEBUI_HTML);
      return 200;
   }

   // Traza de diagnóstico: PTS de los últimos fotogramas presentados.
   if (strcmp(path, "/frames") == 0) {
      *contentType = "text/plain";
      double pts[10];
      int n = player_recent_pts(pts, 10);
      int o = 0;
      o += snprintf(out + o, outCap - o, "ultimos mostrados (nuevo -> viejo):\n");
      for (int i = 0; i < n && o < (int)outCap - 40; i++) {
         o += snprintf(out + o, outCap - o, "  %.3f\n", pts[i]);
      }
      return 200;
   }

   if (strcmp(path, "/status") == 0) {
      *contentType = "application/json";
      char escUrl[600], escNote[300];
      json_escape(g_app.url, escUrl, sizeof(escUrl));
      json_escape(player_state() == PLAYER_FAILED ? player_error() : g_app.notice,
                  escNote, sizeof(escNote));
      snprintf(out, outCap,
               "{\"state\":\"%s\",\"url\":\"%s\",\"casts\":%u,"
               "\"pos\":%.1f,\"dur\":%.1f,\"dl\":%d,\"mbps\":%.2f,\"fps\":%.1f,\"note\":\"%s\"}",
               state_name(), escUrl, g_app.casts,
               player_position(), player_duration(),
               player_progress_pct(), player_mbps(), player_fps(), escNote);
      return 200;
   }

   if (strcmp(path, "/cast") == 0) {
      *contentType = "text/plain";

      if (strcmp(method, "POST") != 0 || body[0] == '\0') {
         snprintf(out, outCap, "Falta la URL.");
         return 400;
      }
      if (strncmp(body, "https://", 8) == 0) {
         snprintf(out, outCap, "HTTPS todavia no: la consola no hace TLS. Usa http://");
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
      g_app.notice[0] = '\0';
      g_app.uiDirty = TRUE;
      WHBLogPrintf("[cast] %s", g_app.url);

      if (!player_play_url(g_app.url)) {
         snprintf(out, outCap, "%s", player_error());
         return 400;
      }
      snprintf(out, outCap, "ok");
      return 200;
   }

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
      g_app.uiDirty = TRUE;
      snprintf(out, outCap, "ok");
      return 200;
   }

   *contentType = "text/plain";
   snprintf(out, outCap, "no encontrado");
   return 404;
}

// ---------------------------------------------------------------------------
// Pantalla de estado, pintada como si fuera un frame de vídeo
// ---------------------------------------------------------------------------
static void draw_ui_frame(void)
{
   uint8_t *fb = video_renderer_framebuffer(0);
   if (!fb) return;

   int pitch = VIDEO_FRAME_PITCH(UI_W);
   overlay_clear(fb, UI_W, UI_H, pitch, 24);   // fondo gris muy oscuro

   overlay_text(fb, UI_W, UI_H, pitch, 40, 40, 2, "WiiU Cast");
   overlay_text(fb, UI_W, UI_H, pitch, 40, 92, 1, "Receptor de medios local");

   char ipstr[16];
   net_ip_str(g_app.ip, ipstr);
   if (g_app.ip) {
      overlay_text(fb, UI_W, UI_H, pitch, 40, 150, 1, "Abre en el telefono:");
      overlay_textf(fb, UI_W, UI_H, pitch, 40, 180, 1, "http://%s:%u",
                    ipstr, http_server_port());
   } else {
      overlay_text(fb, UI_W, UI_H, pitch, 40, 150, 1,
                   "Sin red: revisa la conexion de la consola");
   }

   overlay_textf(fb, UI_W, UI_H, pitch, 40, 250, 1, "Estado: %s", state_name());

   PlayerState ps = player_state();
   if (ps == PLAYER_FAILED) {
      overlay_textf(fb, UI_W, UI_H, pitch, 40, 280, 1, "%s", player_error());
   } else if (ps == PLAYER_BUFFERING) {
      int pct = player_progress_pct();
      if (pct >= 0) {
         overlay_textf(fb, UI_W, UI_H, pitch, 40, 280, 1, "Descargado: %d%%", pct);
      }
   } else if (g_app.url[0]) {
      // La URL puede no caber en una línea: se corta a lo que entra.
      overlay_textf(fb, UI_W, UI_H, pitch, 40, 280, 1, "%.55s", g_app.url);
   }

   overlay_textf(fb, UI_W, UI_H, pitch, 40, 400, 1,
                 "Peticiones: %u   Casts: %u", http_server_requests(), g_app.casts);
   overlay_text(fb, UI_W, UI_H, pitch, 40, 430, 1, "HOME para salir");

   video_renderer_submit(0);
}

static uint32_t on_release_hw(void *ctx)
{
   (void)ctx;
   player_release_hardware();
   return 0;
}

// El reproductor pide la pantalla para el vídeo: se reconfiguran las texturas
// a la resolución del medio. Al soltarla, se vuelve al lienzo de la UI.
static BOOL on_display_request(BOOL wantVideo)
{
   if (!wantVideo) {
      if (!video_renderer_set_size(UI_W, UI_H)) return FALSE;
      g_app.uiDirty = TRUE;
   }
   // Para vídeo no hace falta nada aquí: el player llama a
   // video_renderer_set_size() con la resolución real justo después.
   return TRUE;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogUdpInit();
   WHBInitCrashHandler();

   WHBLogPrintf("== WiiU Cast ==");

   if (!video_renderer_init()) {
      WHBLogPrintf("[main] GX2 no arranco; sin salida de video");
      WHBProcShutdown();
      return 1;
   }
   video_renderer_set_size(UI_W, UI_H);

   net_memory_init();
   g_app.ip = net_local_ip();

   if (!http_server_start(HTTP_PORT, handle_request)) {
      WHBLogPrintf("[main] no se pudo levantar el servidor HTTP");
   }

   player_init();
   player_set_display_cb(on_display_request);

   // Al abrir el menú HOME perdemos el foreground, y con él el derecho a usar
   // el decodificador H264, las voces de audio y la GPU. Hay que devolverlos
   // AQUÍ, que es el momento que ProcUI reserva para ello: intentar soltarlos
   // más tarde (al cerrar la app) cuelga la consola. Prioridad alta para ir
   // por delante de que WHBGfx suelte lo suyo.
   ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, on_release_hw, NULL, 1);

   g_app.uiDirty = TRUE;
   g_app.lastState = PLAYER_IDLE;

   while (WHBProcIsRunning()) {
      http_server_poll();

      BOOL hasVideo = player_update();
      PlayerState ps = player_state();

      if (ps != g_app.lastState) {
         g_app.lastState = ps;
         g_app.uiDirty = TRUE;
      }

      if (!video_renderer_has_foreground()) {
         // Sin pantalla (menú HOME abierto): no se dibuja nada, y hay que
         // dormir o el bucle giraría a tope sin la espera del vsync.
         g_app.uiDirty = TRUE;   // redibujar al recuperar la pantalla
         OSSleepTicks(OSMillisecondsToTicks(16));
         continue;
      }

      if (hasVideo && (ps == PLAYER_PLAYING || ps == PLAYER_PAUSED)) {
         // El vídeo ya está en las texturas: solo hay que presentarlo.
         video_renderer_draw(TRUE, 0.0f, 0.0f, 0.0f);
      } else {
         // Pantalla de estado. Se redibuja solo cuando algo cambia; el resto
         // de frames se limitan a re-presentar el mismo lienzo.
         if (g_app.uiDirty) {
            draw_ui_frame();
            g_app.uiDirty = FALSE;
         }
         video_renderer_draw(TRUE, 0.0f, 0.0f, 0.0f);
      }
   }

   player_shutdown();
   http_server_stop();
   video_renderer_shutdown();
   WHBProcShutdown();
   return 0;
}
