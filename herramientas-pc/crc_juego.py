# -*- coding: utf-8 -*-
"""Dice el SERIAL y el CRC de un juego de PS2 (ISO), igual que los calcula PCSX2,
y el nombre exacto de los archivos de ajustes y parches por juego para Forja SX2.
Uso: python crc_juego.py "juego.iso" [otra.iso ...]"""
import struct
import sys

SECTOR = 2048


def leer_sector(f, lba, n=1):
    f.seek(lba * SECTOR)
    return f.read(n * SECTOR)


def buscar_en_raiz(f, nombre):
    """Busca un archivo en el directorio raíz del ISO9660; devuelve (lba, tamaño)."""
    pvd = leer_sector(f, 16)
    if pvd[1:6] != b"CD001":
        raise SystemExit("No parece una ISO9660 de PS2 (falta CD001)")
    raiz = pvd[156:190]
    lba, tam = struct.unpack_from("<I", raiz, 2)[0], struct.unpack_from("<I", raiz, 10)[0]
    datos = leer_sector(f, lba, (tam + SECTOR - 1) // SECTOR)
    pos = 0
    while pos < len(datos):
        largo = datos[pos]
        if largo == 0:                       # relleno hasta el siguiente sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        n = datos[pos + 32]
        ident = datos[pos + 33:pos + 33 + n].decode("ascii", "replace").split(";")[0]
        if ident.upper() == nombre.upper():
            return struct.unpack_from("<I", datos, pos + 2)[0], struct.unpack_from("<I", datos, pos + 10)[0]
        pos += largo
    raise SystemExit(f"No encontré {nombre} en la raíz de la ISO")


def info(ruta):
    with open(ruta, "rb") as f:
        lba, tam = buscar_en_raiz(f, "SYSTEM.CNF")
        cnf = leer_sector(f, lba)[:tam].decode("ascii", "replace")
        boot = next(l for l in cnf.splitlines() if l.strip().upper().startswith("BOOT2"))
        elf = boot.split("\\")[-1].split(";")[0].strip()          # p. ej. SLUS_209.03
        lba, tam = buscar_en_raiz(f, elf)
        datos = leer_sector(f, lba, (tam + SECTOR - 1) // SECTOR)[:tam]
    # CRC de PCSX2 = XOR de todas las palabras de 32 bits del ELF
    crc = 0
    for (palabra,) in struct.iter_unpack("<I", datos[: len(datos) // 4 * 4]):
        crc ^= palabra
    serial = elf.replace("_", "-").replace(".", "")
    return serial, crc


for ruta in sys.argv[1:]:
    serial, crc = info(ruta)
    print(ruta)
    print(f"  Serial: {serial}   CRC: {crc:08X}")
    print(f"  Ajustes: /data/ForjaSX2/gamesettings/{serial}_{crc:08X}.ini")
    print(f"  Parches: /data/ForjaSX2/patches/{serial}_{crc:08X}.pnach")
