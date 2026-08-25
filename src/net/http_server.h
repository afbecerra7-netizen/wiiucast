// Servidor HTTP mínimo, no bloqueante, para el control desde el teléfono.
//
// Reglas de nsysnet aprendidas en la Fase 0 (ver spikes/RESULTADOS.md) que
// condicionan este diseño:
//   - NADA de SO_NONBLOCK: rompe la recepción. Se usa select() con timeout
//     cero y luego una llamada bloqueante que ya sabemos que tiene datos.
//   - select() está limitado a 32 descriptores (NSYSNET_FD_SETSIZE).
//   - El límite de ~1460 bytes por lectura aplica a datagramas UDP; en TCP
//     las lecturas grandes funcionan (verificado en el spike S3 con 64 KB).
#pragma once
#include <wut_types.h>
#include <stdint.h>

// Handler de petición. Devuelve el cuerpo a enviar por `out` (que apunta a un
// buffer de `outCap` bytes) y el content-type. Retorna el código HTTP.
// `path` viene sin query string; `query` la trae aparte (o "" si no hay).
// `body` es el cuerpo de la petición (POST), terminado en '\0'.
typedef int (*HttpHandler)(const char *method,
                           const char *path,
                           const char *query,
                           const char *body,
                           char *out,
                           uint32_t outCap,
                           const char **contentType);

BOOL http_server_start(uint16_t port, HttpHandler handler);
void http_server_stop(void);

// Llamar una vez por frame desde el bucle principal.
void http_server_poll(void);

// Estadísticas para la UI
uint32_t http_server_requests(void);
uint16_t http_server_port(void);

// IP local de la consola (0 si no se pudo determinar).
uint32_t net_local_ip(void);
void net_ip_str(uint32_t ip, char *out16);

// Dona memoria al stack de red (somemopt). Llamar una vez al arrancar.
void net_memory_init(void);
