// Descarga HTTP en segundo plano hacia un buffer circular, para reproducir
// mientras se descarga.
//
// Solo lectura hacia adelante: el hilo descarga secuencialmente y el lector
// consume por detrás. Eso exige que el MP4 tenga el índice al principio
// (`-movflags +faststart`); si el moov está al final, el demuxer lo detecta
// y se avisa al usuario en vez de fallar en silencio.
#pragma once
#include <wut_types.h>
#include <stdint.h>

typedef enum {
   FETCH_IDLE = 0,
   FETCH_CONNECTING,
   FETCH_STREAMING,
   FETCH_DONE,        // descarga completa
   FETCH_ERROR,
} FetchState;

// Arranca la descarga de `url` (http:// con host o IP). No bloquea.
BOOL fetch_start(const char *url);
void fetch_stop(void);

FetchState fetch_state(void);
const char *fetch_error(void);      // mensaje legible si state == FETCH_ERROR
uint64_t fetch_total_size(void);    // 0 si el servidor no dio Content-Length
uint64_t fetch_downloaded(void);
uint32_t fetch_available(uint64_t offset);  // bytes contiguos legibles desde offset

// Lee sin bloquear. Devuelve los bytes copiados (0 si aún no han llegado, o
// -1 si `offset` ya salió de la ventana: el buffer circular lo sobrescribió).
int fetch_read(uint64_t offset, void *buf, uint32_t len);

// Libera todo lo anterior a `offset` (el lector garantiza no volver atrás).
void fetch_release_until(uint64_t offset);
