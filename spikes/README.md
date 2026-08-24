# Spikes de Fase 0 — WiiU Cast

Tres pruebas desechables en hardware real que resuelven las condiciones del
GO de [AUDITORIA.md](../AUDITORIA.md). **Ninguna necesita FFmpeg ni portlibs:
compilan solo con wut.** Anota los resultados en [RESULTADOS.md](RESULTADOS.md).

| Spike | Condición | Pregunta |
|---|---|---|
| `s1-multicast` | C1 | ¿Llegan datagramas del grupo SSDP 239.255.255.250:1900? |
| `s2-decode` | C2 | ¿Decodifica H264DEC un MP4 con B-frames en modo buffered, y a qué velocidad? |
| `s3-throughput` | C3 | ¿Cuántos Mbps reales da un GET HTTP por Wi-Fi y por LAN? |

## Requisitos

1. **Wii U con [Aroma](https://aroma.foryour.cafe/)** y el plugin de wiiload activo (viene por defecto).
2. **devkitPro con wut** en el PC:
   ```bash
   # Instalador/pacman: https://devkitpro.org/wiki/Getting_Started
   sudo dkp-pacman -S wiiu-dev
   export DEVKITPRO=/opt/devkitpro
   ```
3. (Opcional pero recomendado) `udplogserver` — viene con wut-tools; muestra en el
   PC los mismos logs que salen por pantalla:
   ```bash
   $DEVKITPRO/tools/bin/udplogserver
   ```

## Compilar y desplegar

```bash
cd spikes && make          # compila los tres (o make dentro de cada carpeta)
```

Enviar a la consola por red (la IP de la Wii U sale en Ajustes → Internet,
o en la pantalla del propio spike):

```bash
export WIILOAD=tcp:IP_DE_LA_WIIU
$DEVKITPRO/tools/bin/wiiload s1-multicast/s1-multicast.rpx
```

También puedes copiar los `.wuhb` a `sd:/wiiu/apps/` y lanzarlos desde el menú.

Salir de cualquier spike: botón **HOME**.

---

## S1 — Multicast

**En la consola:** lanza `s1-multicast`. Muestra su IP y el resultado de los
`IP_ADD_MEMBERSHIP` (SSDP y mDNS). Luego loguea cada paquete recibido.

**En el PC (misma red):**
```bash
python3 tools/s1_send_probes.py IP_DE_LA_WIIU
```

**En el teléfono:** abre BubbleUPnP, Web Video Caster o VLC y dale a buscar
dispositivos — emiten M-SEARCH multicast *reales*.

**Interpretación** (la pantalla lo resume al salir):

| Se ve en pantalla | Significa |
|---|---|
| `TEST-multicast` o `M-SEARCH` de apps | **C1 CONFIRMADA** — SSDP viable |
| `TEST-mdns` sí, `TEST-multicast` no | La consola no emite/procesa IGMP para 239/8 → probar router sin IGMP snooping; si persiste, fallback |
| Solo `TEST-unicast` | Multicast RX no funciona → discovery por web UI + QR |

Prueba con Wi-Fi **y** con LAN adapter: el comportamiento multicast puede diferir.

## S2 — Decode buffered

**Generar los archivos de prueba en el PC** (B-frames activados con `-bf 3`,
que es exactamente lo que ningún homebrew ha probado):

```bash
# 720p30 — el objetivo base del producto
ffmpeg -i cualquier_video.mp4 -t 60 -c:v libx264 -profile:v high -level 4.1 \
  -pix_fmt yuv420p -bf 3 -b:v 6M -r 30 -vf scale=1280:720 -an -movflags +faststart \
  test.mp4

# 1080p30 — el techo a verificar (renombrar a test.mp4 para probarlo)
ffmpeg -i cualquier_video.mp4 -t 60 -c:v libx264 -profile:v high -level 4.2 \
  -pix_fmt yuv420p -bf 3 -b:v 12M -r 30 -vf scale=1920:1080 -an -movflags +faststart \
  test-1080.mp4
```

Copiar a la SD como `sd:/wiiucast/test.mp4` y lanzar `s2-decode`.

**Qué reporta:**
- Sondeo de `H264DECMemoryRequirement` para niveles 4.1/4.2/5.0/5.1 (resuelve
  la contradicción 4.2-vs-5.1 de la auditoría).
- Si el MP4 tiene B-frames (vía `ctts`) — si dice que NO, regenera el archivo.
- PTS de entrada (orden decode) vs PTS de salida (deben salir **ordenados**).
- ms por `H264DECExecute` (avg/min/max) contra el presupuesto por frame.
- Volcados `sd:/wiiucast/s2-frame-{0,30,60}.pgm` — ábrelos en el PC (GIMP,
  Preview, `feh`): son el plano Y en gris; si se ven bien, el decode es correcto.

**GO si:** B-frames=SÍ, 0 errores, 0 PTS fuera de orden, avg < 33 ms (a 30 fps)
y los PGM se ven bien. Probar 720p y después 1080p.

## S3 — Throughput

**En el PC:**
```bash
# un archivo grande de datos aleatorios y un servidor HTTP simple
dd if=/dev/urandom of=test.bin bs=1M count=256
python3 -m http.server 8000
```

**En la SD:** crear `sd:/wiiucast/s3-url.txt` con una única línea
(IP literal del PC, sin DNS):
```
http://192.168.1.50:8000/test.bin
```

Lanzar `s3-throughput`. Hace dos pasadas (default y con
`SO_RUSRBUF`+`SO_WINSCALE`+`SO_NOSLOWSTART` sobre 3 MiB de `somemopt`) y
reporta Mbps por segundo y la media.

**Ejecutar dos veces: consola por Wi-Fi y consola por LAN adapter.** El mejor
valor × 0.7 es el techo de bitrate de vídeo razonable del producto.

---

## Notas

- Escrito contra wut v1.9.x. Si algo no compila con tu versión, revisa los
  headers en `$DEVKITPRO/wut/include` — las llamadas usadas están documentadas ahí.
- S1/S3 pueden ejecutarse parcialmente en Cemu (TCP sí; **multicast no** — Cemu
  lo tiene sin implementar, por eso estos spikes existen para hardware).
- Archivos de prueba < 2 GB (el demuxer del S2 usa offsets de 32 bits).
- El código es desechable a propósito: optimiza por responder rápido las tres
  preguntas, no por reutilizarse.
