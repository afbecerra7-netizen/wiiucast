// Reproductor: une descarga HTTP, demux MP4, decode H.264 y presentación GX2.
//
// No bloquea: player_update() se llama una vez por frame desde el bucle
// principal y avanza la máquina de estados según lo que haya llegado por red.
#pragma once
#include <wut_types.h>
#include <stdint.h>

typedef enum {
   PLAYER_IDLE = 0,
   PLAYER_BUFFERING,   // descargando lo suficiente para empezar
   PLAYER_PLAYING,
   PLAYER_PAUSED,
   PLAYER_ENDED,
   PLAYER_FAILED,
} PlayerState;

// El reproductor no controla la pantalla: pide al anfitrión que prepare la
// salida de vídeo (GX2) justo antes de crear las texturas, y que la suelte al
// terminar. Debe devolver TRUE si la pantalla quedó lista.
typedef BOOL (*PlayerDisplayFn)(BOOL wantVideo);
void player_set_display_cb(PlayerDisplayFn fn);

BOOL player_init(void);
void player_shutdown(void);

// Empieza a reproducir una URL http://. Devuelve FALSE si no arrancó.
BOOL player_play_url(const char *url);
void player_stop(void);
void player_toggle_pause(void);

// Llamar una vez por frame. Devuelve TRUE si hay que dibujar vídeo.
BOOL player_update(void);

PlayerState player_state(void);
const char *player_error(void);
double player_position(void);     // segundos reproducidos
double player_duration(void);     // segundos totales (0 si se desconoce)
int player_progress_pct(void);    // % descargado (-1 si no hay tamaño)
uint32_t player_frames_shown(void);
