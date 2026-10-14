# -*- coding: utf-8 -*-
"""Mejora v3: guardar la caché de pipelines de Vulkan en segundo plano, cada 30 s si cambió.
Antes solo se guardaba al salir desde el menú o al pausar con L3+R3; cerrar la app con el botón PS
la perdía y el juego volvía a trabarse compilando los mismos efectos en la siguiente sesión.
Uso: python mejoras_v3_cache.py <carpeta del repo emulador>"""
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


rep("pcsx2/GS/Renderers/Vulkan/VKShaderCache.h", [
    ("\t/// Writes pipeline cache to file, saving all newly compiled pipelines.\n\tbool FlushPipelineCache();\n",
     "\t/// Writes pipeline cache to file, saving all newly compiled pipelines.\n\tbool FlushPipelineCache();\n\n"
     "\t/// Forja SX2: copies the pipeline cache on the calling (GS) thread and writes it from a background\n"
     "\t/// thread through a temporary file and a rename, so a game closed with the PS button keeps it.\n"
     "\tbool FlushPipelineCacheInBackground();\n"),
])

FONDO = '''
bool VKShaderCache::FlushPipelineCacheInBackground()
{
	// One write at a time; a cache that changes meanwhile is picked up by the next call.
	static std::atomic<bool> s_writing{false};
	if (m_pipeline_cache == VK_NULL_HANDLE || !m_pipeline_cache_dirty || m_pipeline_cache_filename.empty() ||
		s_writing.load(std::memory_order_acquire))
	{
		return false;
	}

	size_t data_size = 0;
	VkResult res = vkGetPipelineCacheData(GSDeviceVK::GetInstance()->GetDevice(), m_pipeline_cache, &data_size, nullptr);
	if (res != VK_SUCCESS || data_size == 0)
		return false;
	std::vector<u8> data(data_size);
	res = vkGetPipelineCacheData(GSDeviceVK::GetInstance()->GetDevice(), m_pipeline_cache, &data_size, data.data());
	if (res != VK_SUCCESS)
		return false;
	data.resize(data_size);
	m_pipeline_cache_dirty = false;

	s_writing.store(true, std::memory_order_release);
	std::thread([data = std::move(data), filename = m_pipeline_cache_filename]() {
		const std::string temporary = filename + ".tmp";
		const bool ok = FileSystem::WriteBinaryFile(temporary.c_str(), data.data(), data.size()) &&
						FileSystem::RenamePath(temporary.c_str(), filename.c_str());
		Console.WriteLn("Forja SX2: pipeline cache %s in the background (%zu bytes)", ok ? "saved" : "NOT saved",
			data.size());
		s_writing.store(false, std::memory_order_release);
	}).detach();
	return true;
}

void VKShaderCache::ClosePipelineCache()'''

rep("pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp", [
    ("#include <cstring>\n#include <memory>\n", "#include <atomic>\n#include <cstring>\n#include <memory>\n#include <thread>\n"),
    ("\nvoid VKShaderCache::ClosePipelineCache()", FONDO),
])

# Gancho periódico: Host::OnPerformanceMetricsUpdated corre en el hilo GS (dueño de la caché).
rep("ps4/vm/HostVM.cpp", [
    ('#include "PerformanceMetrics.h"\n', '#include "PerformanceMetrics.h"\n#include "GS/Renderers/Vulkan/VKShaderCache.h"\n'),
    ("\tstatic Common::Timer::Value first_update = Common::Timer::GetCurrentValue();\n"
     "\tstatic Common::Timer::Value last_log = 0;\n"
     "\tconst auto now = Common::Timer::GetCurrentValue();\n",
     "\tstatic Common::Timer::Value first_update = Common::Timer::GetCurrentValue();\n"
     "\tstatic Common::Timer::Value last_log = 0;\n"
     "\tconst auto now = Common::Timer::GetCurrentValue();\n"
     "\t// Forja SX2: keep the Vulkan pipeline cache on disk while playing (every 30 s, only when it changed),\n"
     "\t// so closing the app from the PS button does not throw away the compiled effects.\n"
     "\tstatic Common::Timer::Value last_cache_save = now;\n"
     "\tif (Common::Timer::ConvertValueToSeconds(now - last_cache_save) >= 30.0)\n"
     "\t{\n"
     "\t\tlast_cache_save = now;\n"
     "\t\tif (g_vulkan_shader_cache)\n"
     "\t\t\tg_vulkan_shader_cache->FlushPipelineCacheInBackground();\n"
     "\t}\n"),
])
print("Listo.")
