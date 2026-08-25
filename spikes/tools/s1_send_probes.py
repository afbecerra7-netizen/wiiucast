#!/usr/bin/env python3
"""
Sondas de red para el spike S1 (ejecutar en un PC en la MISMA red que la Wii U).

Envía al puerto 1900 de la consola cuatro tipos de paquete — unicast,
broadcast de subred, broadcast limitado y multicast SSDP — más un paquete al
grupo mDNS. La consola loguea lo que le llega; comparando qué tipos aparecen
en su pantalla se sabe exactamente qué rutas de recepción funcionan.

Uso:
    python3 s1_send_probes.py IP_DE_LA_WIIU [rondas]

La IP la muestra el spike en pantalla al arrancar. `rondas` por defecto 5.
"""

import socket
import sys
import time

SSDP_GROUP = "239.255.255.250"
SSDP_PORT = 1900
MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353

# El tag va en la PRIMERA línea: la consola solo loguea los primeros 60
# caracteres de esa línea, y ahí es donde hay que poder distinguir cada tipo
# de sonda. (Sigue empezando por "M-SEARCH" para que el contador lo clasifique.)
MSEARCH = (
    "M-SEARCH * HTTP/1.1 {tag}\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    'MAN: "ssdp:discover"\r\n'
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "USER-AGENT: wiiucast-probe/{tag}\r\n"
    "\r\n"
)


def subnet_broadcast(ip: str) -> str:
    """Broadcast /24 de la IP dada (suficiente para redes domésticas)."""
    parts = ip.split(".")
    return ".".join(parts[:3] + ["255"])


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    wiiu_ip = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    probes = [
        ("TEST-unicast", wiiu_ip, SSDP_PORT, False),
        ("TEST-uni-1901", wiiu_ip, 1901, False),   # socket de control sin join
        ("TEST-bcast-subred", subnet_broadcast(wiiu_ip), SSDP_PORT, True),
        ("TEST-bcast-255", "255.255.255.255", SSDP_PORT, True),
        ("TEST-multicast", SSDP_GROUP, SSDP_PORT, False),
    ]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)

    print(f"Enviando {rounds} rondas de sondas hacia {wiiu_ip} ...")
    print("Mira la pantalla de la Wii U: cada paquete logueado dice su tipo.\n")

    for r in range(1, rounds + 1):
        for tag, dst, port, _is_bcast in probes:
            payload = MSEARCH.format(tag=tag).encode()
            try:
                sock.sendto(payload, (dst, port))
                print(f"  ronda {r}: {tag:<18} -> {dst}:{port}")
            except OSError as e:
                print(f"  ronda {r}: {tag:<18} -> {dst}:{port} FALLO: {e}")

        # Paquete al grupo mDNS (grupo de control 224.0.0.0/24)
        try:
            sock.sendto(b"TEST-mdns wiiucast-probe", (MDNS_GROUP, MDNS_PORT))
            print(f"  ronda {r}: TEST-mdns          -> {MDNS_GROUP}:{MDNS_PORT}")
        except OSError as e:
            print(f"  ronda {r}: TEST-mdns FALLO: {e}")

        time.sleep(2)

    print("\nListo. Interpretación (ver también spikes/README.md):")
    print("  - Solo aparece TEST-unicast        -> multicast RX NO funciona")
    print("  - Aparece TEST-mdns pero no TEST-multicast -> problema IGMP/239-8")
    print("  - Aparece TEST-multicast o M-SEARCH de apps reales -> C1 CONFIRMADA")


if __name__ == "__main__":
    main()
