# -*- coding: utf-8 -*-
"""Mejora v4: los hilos secundarios (lectura/descompresión del disco, registro, telemetría, guardado
de la caché) van a los núcleos 0-1, libres durante la partida, en vez de caer sobre EE/VU1/GS/Audio.
Uso: python mejoras_v4_ayudantes.py <carpeta del repo emulador>"""
import pathlib
import sys

raiz = pathlib.Path(sys.argv[1])


def rep(rel, pares):
    """Reemplazo exacto; se detiene si un texto no aparece exactamente una vez."""
    f = raiz / rel
    t = f.read_text(encoding="utf-8")
    for a, b in pares:
        n = t.count(a)
        if n != 1:
            raise SystemExit(f"{rel}: esperaba 1 vez y encontré {n}: {a[:70]!r}")
        t = t.replace(a, b)
    f.write_text(t, encoding="utf-8", newline="\n")
    print("ok", rel)


INC = '#include "ps4/core/ThreadAffinityPS4.h"\n'

# Lector del disco: también descomprime CSO/ZSO/CHD, es el ayudante que más CPU usa.
rep("pcsx2/CDVD/ThreadedFileReader.cpp", [
    ('#include "common/Threading.h"\n',
     '#include "common/Threading.h"\n#if defined(PCSX2_PS4_VM_BOOT)\n' + INC + '#endif\n'),
    ('\tThreading::SetNameOfCurrentThread("ISO Decompress");\n',
     '\tThreading::SetNameOfCurrentThread("ISO Decompress");\n'
     '#if defined(PCSX2_PS4_VM_BOOT)\n\tPS4ThreadAffinity::PinCurrentThreadToHelpers("disc reader");\n#endif\n'),
])

# Escritor de la caché de pipelines (mejora v3): nació en el hilo GS, no debe quedarse en su núcleo.
rep("pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp", [
    ('#include "GS/Renderers/Vulkan/VKOrbisTiming.h" // vk-285-38: [shaders]\n',
     '#include "GS/Renderers/Vulkan/VKOrbisTiming.h" // vk-285-38: [shaders]\n#if defined(PCSX2_PS4_VM_BOOT)\n' + INC + '#endif\n'),
    ('\t\tconst std::string temporary = filename + ".tmp";\n',
     '#if defined(PCSX2_PS4_VM_BOOT)\n\t\tPS4ThreadAffinity::PinCurrentThreadToHelpers("pipeline cache writer");\n#endif\n'
     '\t\tconst std::string temporary = filename + ".tmp";\n'),
])

# Registro y telemetría (código propio del port, siempre PS4).
rep("ps4/core/ProbeLoggerPS4.cpp", [
    ('#include "TelemetryPS4.h"\n', '#include "TelemetryPS4.h"\n#include "ThreadAffinityPS4.h"\n'),
    ('\tvoid* Run(void*)\n\t{\n\t\tEntry entry;\n',
     '\tvoid* Run(void*)\n\t{\n\t\tPS4ThreadAffinity::PinCurrentThreadToHelpers("log writer");\n\t\tEntry entry;\n'),
])
rep("ps4/core/TelemetryPS4.cpp", [
    ('#include "TelemetryPS4.h"\n', '#include "TelemetryPS4.h"\n#include "ThreadAffinityPS4.h"\n'),
    ('\t\tpthread_setname_np(pthread_self(), "PS4 telemetry");\n',
     '\t\tpthread_setname_np(pthread_self(), "PS4 telemetry");\n'
     '\t\tPS4ThreadAffinity::PinCurrentThreadToHelpers("telemetry");\n'),
])
print("Listo.")
