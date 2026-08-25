# Auditoría técnica — WiiU Cast

**Fecha:** 2026-08-24 · **Método:** 12 agentes de investigación (6 investigadores paralelos, 5 verificadores adversariales, 1 crítico de completitud), ~400 consultas sobre fuentes primarias (código fuente, docs de wut, GitHub API, foros).

---

## Veredicto: VIABLE con correcciones — GO condicional

**No se descarta.** El pipeline central (red → decode H.264 por hardware → NV12 → GX2 en TV) está **probado end-to-end por Moonlight Wii U** a 720p60 con adaptador LAN. El nicho está **completamente vacío**: no existe ningún receptor DLNA/AirPlay/Chromecast homebrew en ninguna consola Nintendo — WiiU Cast sería el primero. La escena está viva en 2026 (wut v1.9.1, Aroma beta-27, devkitPPC r49.1, hb-appstore activo).

**Las condiciones:** dos incógnitas que ninguna investigación puede resolver desde el escritorio y que **deben probarse en hardware real antes de escribir el producto** (Fase 0), más una restricción física de red que fuerza decisiones de producto.

> **ACTUALIZACIÓN 2026-08-24 — Fase 0 EJECUTADA en hardware real.** Resultados completos en [spikes/RESULTADOS.md](spikes/RESULTADOS.md). El GO condicional pasa a **GO**.

| # | Condición | Estado tras Fase 0 |
|---|---|---|
| 1 | Recepción multicast SSDP (`239.255.255.250:1900`) en hardware real | ✅ **CONFIRMADA** — la consola recibe SSDP multicast, mDNS, broadcast y unicast por Wi-Fi. (Maña crítica descubierta: leer UDP con `len ≤ 1460` o nsysnet falla con EMSGSIZE; sin `SO_NONBLOCK`, patrón select+recvfrom) |
| 2 | Decode de archivos MP4 *normales* (B-frames, AVCC) con H264DEC en modo buffered | ✅ **CONFIRMADA** — reordenado perfecto (0 PTS fuera de orden), 0 errores; avg 9.6 ms/frame a 720p30 (3.5×) y 21.0 ms a 1080p30 (1.6×). Techo: 720p60 y 1080p30 sí; 1080p60 no |
| 3 | Wi-Fi de la consola: 2.4 GHz b/g/n vs. vídeos de teléfono de 10–40 Mbps | ⚠️ **PARCIAL** — medidos 1.8 Mbps sostenidos por Wi-Fi (límite de ventana TCP, no de radio). Palancas pendientes: `SO_RCVBUF` grande, descargas paralelas con Range, y adaptador LAN |

---

## 1. Verificación adversarial de las afirmaciones críticas

Cinco fact-checkers independientes intentaron refutar las afirmaciones que sostenían el análisis previo:

| Afirmación | Veredicto | Matiz importante |
|---|---|---|
| Decode H.264 por software no alcanza HD en el Espresso (3× PPC750 @1.24 GHz, sin AltiVec) | ✅ **CONFIRMADO** | Consenso unánime de quienes han publicado vídeo homebrew en Wii U. Techo real por software: ~480p. El hardware se usa a *todas* las resoluciones, no solo ≥720p. |
| H264DEC existe en wut, emite NV12, y homebrew real lo usa | ✅ **CONFIRMADO** (con corrección) | El límite **no es Level 4.1**: la librería acepta hasta **Level 5.1 y 2800×1408** (validación idéntica en decaf y Cemu, ambos ingeniería inversa de la librería real). Moonlight asigna memoria para L4.2 (~40 MB); L5.1 exigiría ~189 MB. Solo perfiles baseline/main/high, solo 4:2:0. 1080p está dentro del sobre; 720p60 está *probado*. |
| nsysnet soporta multicast (`IP_ADD_MEMBERSHIP`) para SSDP | ⚠️ **INCIERTO** | La API existe y está activada por defecto desde **wut v1.4.0** (PR #331, oct 2023: `set_multicast_state(TRUE)` automático). Evidencia indirecta fuerte de recepción mDNS real (ftpiiu v0.4.4 corrió el responder mDNS en consolas). **Pero nadie ha probado nunca el grupo 239.255.255.250** (mDNS usa 224.0.0.251, que muchos switches inundan sin IGMP; SSDP requiere reportes IGMP reales). Spike obligatorio. |
| El port SDL2 permite render independiente TV/GamePad | ✅ **CONFIRMADO** | Flags `SDL_WINDOW_WIIU_TV_ONLY` / `_GAMEPAD_ONLY` / `_PREVENT_SWAP`; **Uxplore lo demuestra en código real** (dos ventanas, dos renderers). Pero el renderer SDL2 **no tiene formatos YUV** → el vídeo debe ir por GX2 crudo con shader NV12 (el de Moonlight). |
| FFmpeg se compila para Wii U y hay homebrew que lo usa | ✅ **CONFIRMADO** | No hay portlib — se compila desde fuente con el `configure-wiiu` de GaryOderNichts (`--arch=ppc --cpu=750`). CaféMP lo hace hoy. El wrapper `h264_wiiu` (H264DEC dentro de FFmpeg) vive en un fork de FFmpeg 4.3 **estancado desde dic 2020**; portarlo a FFmpeg moderno es ~4 KB de código. |

## 2. Errores del plan original

1. **`manifest.aip` no existe.** REFUTADO con búsqueda de código: 0 resultados junto a wiiu/wuhb/aroma (.aip es un formato de Advanced Installer). Los metadatos van como flags de `wuhbtool`: `--name --short-name --author --icon(128×128) --tv-image(1280×720) --drc-image(854×480)`.
2. **"FFmpeg (o reproducción nativa)" como motor de decodificación.** No son alternativas: FFmpeg sirve para **demux + audio**; el vídeo H.264 va **siempre** por H264DEC hardware. No hay plan B por software para HD.
3. **"Reproducir a 60 fps en 1080p"** como hito de Fase 3: 720p30–60 es lo demostrado; 1080p30 es plausible pero sin medición real de throughput del decoder. Ningún proyecto activo garantiza siquiera 720p30 con archivos arbitrarios.
4. **CEMU "con soporte de red habilitado" como entorno de pruebas.** Cemu implementa bind/listen/accept TCP (sirve para probar el servidor HTTP), pero **multicast está en stub "todo"** y SO_BROADCAST no tiene handler → el descubrimiento DLNA solo se prueba en hardware real. Cemu sí emula H264DEC (desde 1.15.4).
5. **raylib** como opción de UI: no hay port mantenido; el stack real es SDL2 (rama oficial devkitPro, activa feb 2026) + GX2 crudo para el vídeo.
6. **DLNA en Fase 2, reproductor en Fase 3.** Orden invertido: el riesgo está en el reproductor; DLNA es trabajo tedioso pero sin incógnitas (protocolo documentado, superficie mínima conocida).
7. **Faltaban en el plan:** sincronía A/V, seek, presupuesto de memoria, HTTPS, HLS, matriz de licencias, comportamiento en background, y la realidad del UX del emisor.

## 3. Estado del arte — qué existe y qué aprovechar

**El hallazgo principal: el nicho de receptor está vacío, pero todas las piezas existen por separado.**

| Repo | Qué es | Qué aprovechar | Licencia | Estado |
|---|---|---|---|---|
| [moonlight-wiiu](https://github.com/GaryOderNichts/moonlight-wiiu) | Cliente de game streaming | **La pieza clave.** `decode.c`: H264DEC → NV12 directo a texturas GX2 (Y=R8, UV=R8G8, zero-copy), shader YUV→RGB, doble pasada TV/DRC, `somemopt` a 3 MiB. Prueba el stack SDL2+curl+mbedtls | GPL-3.0 | Activo (feb 2026) |
| [cafemp (CaféMP)](https://github.com/whateveritwas/cafemp) | Reproductor de medios nativo (SD) | Arquitectura de player completa: demux FFmpeg, decoder `h264_wiiu` con fallback software, sync A/V por PTS con reloj maestro de audio, 4 hilos, shaders NV12/YUV420P, ImGui/GX2. Su roadmap lista DLNA… sin código aún (sería *cliente*, no receptor) | ⚠️ **Sin licencia** | Muy activo (ago 2026) |
| [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu) | Fork FFmpeg 4.3 con decoder `h264_wiiu` | El wrapper H264DEC↔libavcodec y el `configure-wiiu` (flags de cross-compile exactos) | LGPL/GPL | Estancado (2020) — CaféMP prueba que rebasear funciona |
| [Ristretto](https://github.com/wiiu-smarthome/Ristretto) | Plugin Aroma: servidor HTTP (:8572) para domótica | *(El plan lo creía un reproductor — corregido.)* Precedente de servidor HTTP vivo en Aroma (tinyhttp modificado) + **gotcha crítico documentado: el servidor muere cuando se abre el menú Home o un applet** | Sin licencia | Activo (jun 2026) |
| [gmrender-resurrect](https://github.com/hzeller/gmrender-resurrect) | MediaRenderer UPnP para Linux | La capa UPnP está **limpiamente separada** de GStreamer (`upnp_transport/control/connmgr`, `output.h` como interfaz de player) — mapa de qué implementar | GPL-2.0 | Activo (feb 2026) |
| [pupnp](https://github.com/pupnp/pupnp) | SDK UPnP portable en C (SSDP+SOAP+GENA) | Candidato a portar; requiere pthreads (devkitPPC r49 los añadió) y tuning (pool de 12 hilos, y ojo: **select en Wii U está capado a 32 fds**) | BSD-3 | Activo (ago 2026) |
| [mdnsniff](https://github.com/AorsiniYT/mdnsniff) | Sniffer mDNS para Wii U | Código de referencia de `IP_ADD_MEMBERSHIP` sobre nsysnet (join 224.0.0.251, SO_REUSEADDR, drain no bloqueante) | — | may 2026 |
| [Uxplore](https://github.com/Kuruyia/Uxplore) | Explorador de archivos | **Demuestra en código las dos ventanas SDL2** (TV_ONLY + GAMEPAD_ONLY, dos renderers) | — | Referencia |
| [ma-provider-dlna-receiver](https://github.com/trudenboy/ma-provider-dlna-receiver) | Receptor DLNA de Music Assistant | La **superficie mínima verificada en campo** que un renderer debe implementar (ver §5) | — | Referencia |
| [ctr-streaming-server](https://github.com/yellows8/ctr-streaming-server) | Receptor de medios 3DS (TCP :8334) | El precedente de receptor en consola más cercano… sin discovery, incompleto — ilustra el listón bajo | — | Histórico |

**Descartados por la investigación:** Kodi/VLC nunca se portaron (hilos de GBAtemp concluyen inviable); WiiMC es solo Wii/vWii y ni siquiera era cliente UPnP; VLCu está "not yet usable"; los experimentos de decode por software (2018) murieron donde se predijo.

## 4. Riesgos rankeados

| Riesgo | Estado | Mitigación |
|---|---|---|
| Pipeline red→H264DEC→GX2 | ✅ Viable (Moonlight lo prueba) | Reusar patrón de decode.c |
| Decode de archivos normales (B-frames, AVCC, modo buffered ~5 frames de delay) | ❓ **Desconocido — riesgo nº 1** | Spike Fase 0: si falla, el caso de uso principal muere |
| Recepción SSDP multicast | ❓ Desconocido (API sí, entrega de paquetes sin probar; IGMP/power-save Wi-Fi sin probar) | Spike Fase 0; fallback: web UI + QR / app compañera escaneando subred |
| Throughput Wi-Fi (2.4 GHz solo; ~1 MB/s típico homebrew; LAN adapter ~3.5 MB/s) | ⚠️ Bloqueador parcial | Producto 720p-class sobre Wi-Fi; recomendar LAN adapter; transcodificación en el emisor como opción |
| Sender apps reales (filtro de `protocolInfo` MIME, GENA, fragilidad de discovery — hasta Kodi "a veces no aparece") | ❓ Desconocido | protocolInfo generosa; matriz de pruebas con captura de paquetes (BubbleUPnP, Web Video Caster, LocalCast) |
| HTTPS (muchas apps entregan URLs https; mbedtls es software puro en el PPC) | ❓ Sin investigar | v1: archivos locales por HTTP plano; medir coste TLS en Fase 3 |
| Sync A/V con jitter de red + delay del modo buffered | ❓ Sin diseño probado | Patrón CaféMP (reloj maestro audio) + buffer de red generoso; fase propia |
| Servidor muere en background (menú Home/applets) + auto-apagado (APD) | ⚠️ Restricción confirmada | v1: app foreground con pantalla "esperando cast"; suprimir APD; split plugin+app para v2 |
| Memoria (H264DEC L4.2 ~40 MB, somemopt 3 MiB, buffers FFmpeg, scan buffers GX2) | ⚠️ Sin presupuesto consolidado | Presupuestar en Fase 1; L5.1 (~189 MB) probablemente inviable |
| Licencias (moonlight GPL-3, gmrender GPL-2, CaféMP sin licencia) | ⚠️ Decisión pendiente | Proyecto GPL-3.0; no copiar de CaféMP sin permiso (contactar autor); UPnP propio o de pupnp (BSD-3) |
| Demanda / UX del emisor (los botones nativos de cast del móvil son Chromecast/AirPlay — el usuario necesita una app tercera DLNA) | ⚠️ Asumir | Documentarlo desde el README; la web UI propia mitiga (no requiere app) |

## 5. La superficie DLNA mínima (verificada en campo)

Para que BubbleUPnP/VLC/Web Video Caster listen y casteen (referencia: Music Assistant dlna-receiver; VLC rechaza renderers sin `SetAVTransportURI`):

- **SSDP:** respuestas a M-SEARCH + NOTIFY alive/byebye periódicos (CACHE-CONTROL, LOCATION, SERVER, ST, USN; respetar ventana MX; USN estable o aparecen renderers fantasma)
- **HTTP:** device description XML + service descriptions
- **AVTransport:** SetAVTransportURI, Play, Pause, Stop, Seek, GetTransportInfo, GetPositionInfo, GetMediaInfo
- **RenderingControl:** Get/SetVolume, Get/SetMute
- **ConnectionManager:** GetProtocolInfo (¡lista MIME generosa — BubbleUPnP descarta lo que no anuncies!), GetCurrentConnectionIDs/Info
- **GENA:** SUBSCRIBE/NOTIFY con LastChange (las barras de progreso de los senders dependen de esto)

## 6. Plan corregido

### Fase 0 — Spikes de riesgo (1–2 fines de semana, código desechable, hardware real)

| Spike | Qué | Criterio de salida |
|---|---|---|
| **S1 — Multicast** | ~50 líneas: bind `0.0.0.0:1900`, `IP_ADD_MEMBERSHIP` 239.255.255.250, loguear M-SEARCH enviados desde el teléfono. Probar también con IGMP snooping activado en el router y por Wi-Fi vs LAN | Llegan paquetes → SSDP viable. No llegan → discovery por web UI/QR + escaneo (el producto sobrevive con premisa debilitada) |
| **S2 — Decode real** | MP4 típico de teléfono (con B-frames) desde SD: libavformat + `h264_mp4toannexb` → H264DEC **modo buffered** → TV. Medir ms/frame a 720p y 1080p; probar `H264DECMemoryRequirement` con L5.0/5.1 | Reproduce fluido → GO. Falla/tartamudea → replantear el producto entero |
| **S3 — Throughput** | HTTP GET sostenido (curl) por Wi-Fi y LAN adapter, medir Mbps reales | Fija el techo de bitrate anunciable |

### Fase 1 — Esqueleto
wut + CMake + `.wuhb` (wuhbtool, sin manifest.aip) · ProcUI (foreground/background correcto) · UI OSScreen en ambas pantallas · servidor HTTP (patrón tinyhttp/Ristretto, sockets no bloqueantes `SO_NBIO`, `somemopt` 3 MiB) · despliegue con wiiload (:4299).
**Hito:** `http://[IP]:8080` desde el teléfono responde y la consola lo muestra.

### Fase 2 — Reproductor end-to-end (primer producto usable)
Web UI servida al teléfono (URL + botones) → streaming HTTP → demux FFmpeg → H264DEC → shader NV12 GX2 → TV; audio FFmpeg→SDL2 (AX 48 kHz); sync A/V con reloj maestro de audio.
**Hito:** pegas una URL de MP4 en el teléfono y se reproduce con audio sincronizado.

### Fase 3 — Robustez
Buffering de red contra jitter · seek completo (HTTP Range → `H264DECFlush` → `H264DECFindIdrpoint`) · manejo de caída de red (lección Ristretto: sin fugas al desconectar) · presupuesto de memoria cerrado · suprimir auto-apagado · medir coste HTTPS.

### Fase 4 — MediaRenderer DLNA
La superficie de §5, hand-rolled o pupnp adaptado (cap de 32 fds) · matriz de pruebas con captura de paquetes: BubbleUPnP, Web Video Caster, LocalCast, VLC móvil.
**Hito:** la consola aparece en "dispositivos disponibles" y reproduce lo que le mandan.

### Fase 5 — Pulido
UI GamePad independiente (SDL2 dos ventanas o GX2) · apagar pantalla GamePad (`VPADSetLcdMode`, patrón Padcon) · imágenes y audio-solo · tema oscuro · publicación en hb-appstore (submit.fortheusers.org).

### Fuera de alcance (v1, explícito en el README)
HEVC/VP9/AV1 (sin decoder HW) · H.264 4:2:2/4:4:4 · HLS · servicios DRM (Netflix/Disney+: Widevine, imposible) · AirPlay/Chromecast (protocolos cerrados) · 1080p garantizado sobre Wi-Fi · vWii.

## 7. Decisión de arquitectura pendiente (única bloqueante antes de Fase 1)

**¿App `.wuhb` monolítica o plugin (server) + app (player)?** El server muere en background en ambos casos cuando hay applets abiertos, así que para v1 la app foreground con pantalla de espera es más simple y suficiente. El split plugin+app (estilo Ristretto) queda para v2 si se quiere recepción con el menú abierto. Recomendación: **monolítica v1**.

---

## Fuentes principales

wut ([H264DEC](https://github.com/devkitPro/wut/blob/master/include/h264/decode.h), [netinet/in.h](https://github.com/devkitPro/wut/blob/master/include/netinet/in.h), [PR #331](https://github.com/devkitPro/wut/pull/331), [somemopt](https://github.com/devkitPro/wut/blob/master/include/nn/nets2/somemopt.h), [vpad LCD](https://github.com/devkitPro/wut/blob/master/include/vpad/input.h)) · [decaf h264](https://github.com/decaf-emu/decaf-emu/blob/master/src/libdecaf/src/cafe/libraries/h264/h264_decode.cpp) y [Cemu H264Dec](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/libs/h264_avc/H264Dec.cpp) (límites del decoder, ingeniería inversa) · [Cemu nsysnet](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/libs/nsysnet/nsysnet.cpp) (multicast stub) · [SDL2 wiiu-sdl2-2.32](https://github.com/devkitPro/SDL/tree/wiiu-sdl2-2.32) · [wut-packages](https://github.com/devkitPro/wut-packages) (portlibs) · [wuhbtool](https://github.com/devkitPro/wut-tools/blob/master/src/wuhbtool/main.cpp) · [Nintendo: Wi-Fi 2.4 GHz](https://en-americas-support.nintendo.com/app/answers/detail/a_id/1666) · [ftpiiu #76](https://github.com/wiiu-env/ftpiiu_plugin/issues/76) (evidencia mDNS) · repos de la tabla §3.

*Dossier completo de investigación (claims, evidencia y preguntas abiertas por tema): [docs/investigacion.txt](docs/investigacion.txt).*
