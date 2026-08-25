# Resultados de los spikes de Fase 0

> Rellenar tras ejecutar cada spike en hardware real. Estos resultados
> deciden el GO/NO-GO y la arquitectura de descubrimiento (ver AUDITORIA.md §6).

## Entorno

- Modelo de Wii U / firmware / versión de Aroma: Wii U con Aroma (wiiload plugin v0.2.5 añadido durante las pruebas)
- Router (modelo, banda, ¿IGMP snooping activado?): red doméstica 192.168.40.0/24, consola por Wi-Fi 2.4 GHz
- ¿Adaptador LAN disponible?: no usado en esta sesión
- Fecha: 2026-08-24

## S1 — Multicast (condición C1) — ✅ COMPLETADO

| Prueba | Resultado |
|---|---|
| `IP_ADD_MEMBERSHIP` 239.255.255.250 (rc/errno) | rc=0 ✅ |
| `IP_ADD_MEMBERSHIP` 224.0.0.251 (rc/errno) | rc=0 ✅ |
| Llega TEST-unicast | ✅ |
| Llega TEST-bcast-subred | ✅ |
| Llega TEST-bcast-255 | ✅ |
| Llega TEST-multicast | ✅ (¡SSDP 239.255.255.250 desde la red!) |
| Llega TEST-mdns | ✅ (+ tráfico mDNS ambiental de otros dispositivos) |
| Llegan M-SEARCH de apps reales (BubbleUPnP/VLC) | no probado (sin app instalada); irrelevante: las sondas son M-SEARCH reales |
| TX: sendto al grupo (rc) | 126 bytes OK ✅ (y el propio paquete vuelve por loopback → self=2) |

**Veredicto C1:** ☑ **CONFIRMADA** (SSDP viable) ☐ PARCIAL ☐ NEGATIVA

### Mañas de nsysnet descubiertas (¡oro para el producto!)

1. **`recvfrom`/`recv` UDP con longitud > ~1460 falla con error nativo 12 (EMSGSIZE)** y
   el datagrama NO se consume. Leer con `len <= 1460` funciona. Éste era el único bug:
   la entrega de paquetes funcionó siempre.
2. **`SO_NONBLOCK` rompe `recvfrom`** (err 12 constante). El patrón correcto es
   `select()` con timeout cero + recvfrom bloqueante (como mdnsniff).
3. **`MSG_DONTWAIT` tampoco es fiable** — evitar flags en recvfrom.
4. Con **somemopt** donado, `SO_RUSRBUF` y `SO_RCVBUF` pasan a devolver rc=0
   (sin somemopt, `SO_RCVBUF` daba EINVAL en algún socket).
5. El wiiload de `.rpx` por red se colgó en pantalla de carga una vez; el de `.wuhb`
   funciona fiable.

Notas: pendiente repetir con adaptador LAN y con IGMP snooping activo en otro router.

## S2 — Decode buffered (condición C2) — ✅ COMPLETADO

Archivos: testsrc2 60 s, libx264 High, `-bf 3`, yuv420p, faststart (720p L4.1 @6M; 1080p L4.2 @12M).

| Métrica | 720p30 | 1080p30 |
|---|---|---|
| B-frames detectados (ctts) | SÍ | SÍ |
| Frames entrada / salida | 1800 / 1800 | 1800 / 1800 |
| PTS fuera de orden | **0** | **0** |
| Errores de Execute | 0 | 0 |
| avg ms/frame | **9.6** | **21.0** |
| min / max ms/frame | 9.3 / 21.8 | 20.5 / 34.8 |
| Margen vs presupuesto 33.3 ms | **3.5×** | **1.6×** |
| ¿PGM correctos? | ✅ **bit-exacto** | ✅ **bit-exacto** |

El reordenado es visible en los logs `[pts]`: entrada en orden decode (desordenada),
salida perfectamente monótona. `status0=100` en ambos. El decoder reporta 1920×1088
(alto codificado) para el archivo 1080p, como se esperaba.

### Verificación visual: el decoder hardware es BIT-EXACTO vs ffmpeg

Los volcados PGM de la consola se compararon contra el plano Y extraído en el PC con
`ffmpeg -pix_fmt yuv420p -f rawvideo` (sin conversión de rango):

```
720p  frames 0/30/60: difieren 0 px de 921.600  | max delta 0
1080p frames 0/30/60: difieren 0 px de 2.073.600 | max delta 0
```

Cero desviación. El H264DEC de la Wii U produce salida idéntica al decoder por software
de ffmpeg — no hay pérdida, ni desplazamiento de pitch, ni artefactos.

⚠️ Trampa al verificar: `ffmpeg -vf format=gray` **sí** convierte de rango limitado
(16–235) a completo (0–255) y da falsos positivos (media de diferencia ~8, máx 17).
Comparar siempre contra el plano Y crudo de `yuv420p`.

**Escala lineal con los píxeles** (9.6 ms × 2.25 ≈ 21.0 ms medidos) → techo del decoder:
**720p60 sí (9.6 < 16.6), 1080p30 sí (21.0 < 33.3), 1080p60 no (21.0 > 16.6).**

Sondeo `H264DECMemoryRequirement` (1920×1088): se ejecutó pero el resultado salió de
pantalla antes de la foto — repetir en la próxima sesión (dato no bloqueante: L4.2 acepta
y basta para 1080p30).

**Veredicto C2:** ☑ **CONFIRMADA** (720p30/720p60/1080p30) ☐ FALLA

## S3 — Throughput (condición C3) — ⚠️ PARCIAL (solo Wi-Fi)

| Vía | Pasada A (default) | Pasada B (tuned: RUSRBUF+WINSCALE+NOSLOWSTART) |
|---|---|---|
| Wi-Fi (Mac también en Wi-Fi, servidor python) | **1.79 Mbps** | **1.73 Mbps** |
| LAN adapter | pendiente | pendiente |

Análisis: ~1.8 Mbps estables con tuning indiferente = límite de ventana TCP, no de radio.
Con RTT medido ~20 ms y ventana InterNiche por defecto (~4–8 KB): ventana/RTT ≈ 0.2–0.4 MB/s,
que cuadra con lo medido. Palancas pendientes para S3 v2:
1. `SO_RCVBUF` explícito grande (128–256 KB) tras somemopt (en la pasada tuned NO se seteó).
2. Descargas paralelas (2–3 conexiones con HTTP Range) — multiplican throughput limitado por ventana.
3. Adaptador LAN (RTT ~1 ms → el mismo límite de ventana daría >30 Mbps).

**Implicación provisional:** por Wi-Fi solo ~1.3 Mbps de vídeo seguro (SD/480p) sin las
mejoras de arriba; el buffering agresivo del reproductor (descarga por delante de la
reproducción) mitiga para clips cortos.
**Decisión C3:** pendiente de S3 v2 + prueba LAN.

## Decisión final Fase 0

☑ **GO — pasar a Fase 1** con descubrimiento: ☑ **SSDP** (confirmado en hardware) + web UI como complemento
☐ Replantear

Pendientes no bloqueantes que hereda la Fase 1:
1. S3 v2: `SO_RCVBUF` grande + descargas paralelas con Range. **Ojo al sesgo de la
   medida actual: la Mac también estaba en Wi-Fi, así que cada byte cruzó el aire dos
   veces.** Repetir con el PC por cable antes de sacar conclusiones. (No hay adaptador
   LAN disponible para la consola.)
2. Repetir sondeo de niveles 5.0/5.1 (salió de pantalla).
3. ~~Verificación visual de los PGM~~ ✅ hecho: bit-exacto.
4. Repetir S1 en una red con IGMP snooping activo.
