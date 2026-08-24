# Resultados de los spikes de Fase 0

> Rellenar tras ejecutar cada spike en hardware real. Estos resultados
> deciden el GO/NO-GO y la arquitectura de descubrimiento (ver AUDITORIA.md §6).

## Entorno

- Modelo de Wii U / firmware / versión de Aroma:
- Router (modelo, banda, ¿IGMP snooping activado?):
- ¿Adaptador LAN disponible?:
- Fecha:

## S1 — Multicast (condición C1)

| Prueba | Resultado |
|---|---|
| `IP_ADD_MEMBERSHIP` 239.255.255.250 (rc/errno) | |
| `IP_ADD_MEMBERSHIP` 224.0.0.251 (rc/errno) | |
| Llega TEST-unicast | |
| Llega TEST-bcast-subred | |
| Llega TEST-bcast-255 | |
| Llega TEST-multicast | |
| Llega TEST-mdns | |
| Llegan M-SEARCH de apps reales (BubbleUPnP/VLC) | |
| TX: sendto al grupo (rc) | |

**Veredicto C1:** ☐ CONFIRMADA (SSDP viable) ☐ PARCIAL (problema IGMP) ☐ NEGATIVA (fallback web UI + QR)

Notas (¿cambia con Wi-Fi vs LAN? ¿con otro router?):

## S2 — Decode buffered (condición C2)

Archivo de prueba usado (resolución, fps, bitrate, `-bf`):

| Métrica | 720p30 | 1080p30 |
|---|---|---|
| B-frames detectados (ctts) | | |
| Frames entrada / salida | | |
| PTS fuera de orden | | |
| Errores de Execute | | |
| avg ms/frame | | |
| max ms/frame | | |
| ¿PGM se ven bien? | | |

Sondeo `H264DECMemoryRequirement` (1920×1088):

| Nivel | err | bytes |
|---|---|---|
| 4.1 | | |
| 4.2 | | |
| 5.0 | | |
| 5.1 | | |

**Veredicto C2:** ☐ CONFIRMADA ☐ FALLA (detalle):

## S3 — Throughput (condición C3)

| Vía | Pasada A (default) | Pasada B (tuned) |
|---|---|---|
| Wi-Fi | Mbps | Mbps |
| LAN adapter | Mbps | Mbps |

**Techo de bitrate de vídeo elegido para el producto:** ___ Mbps
**Decisión C3:** ☐ 720p sobre Wi-Fi ☐ exigir LAN para 1080p ☐ transcodificar en emisor

## Decisión final Fase 0

☐ GO — pasar a Fase 1 con descubrimiento: ☐ SSDP ☐ web UI + QR ☐ ambos
☐ Replantear (motivo):
