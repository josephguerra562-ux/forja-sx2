// Medidor de DawnStation: un plugin de GoldHEN que mira lo que el emulador le entrega al sistema, sin tocar ni leer su código.
// Cuenta cuántas imágenes enviadas al televisor son de verdad distintas de la anterior, y cuánto se envía a la gráfica por imagen.
// Escribe en /data/medidor_dawn/medidor.log. Se prende solo para DAWN00001 en /data/GoldHEN/plugins.ini.
//
// Versión 2 (2026-11-01). La 1 enganchaba todo apenas cargaba, también en el menú de DawnStation, y usaba fopen: creó su carpeta, no escribió nada y la
// app se trabó, sin saberse en qué paso. Ahora cada paso queda escrito al momento con llamadas directas del sistema, y solo se engancha cuando aparece el
// módulo del menú dentro del juego (menu.prx). En el menú de la app no engancha nada.

#include <Common.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"

#define PLUGIN_NAME "medidor_dawn"
#define LOG_FOLDER "/data/" PLUGIN_NAME
#define LOG_PATH LOG_FOLDER "/medidor.log"
#define MAX_BUFFERS 16
#define MAX_MODULES 256
#define GRID_W 48
#define GRID_H 27
#define WINDOW_US 5000000u
// Fewer than this many samples changed (of 1296) counts as a slight change: a fade or a blinking icon, not a new frame.
#define SLIGHT_SAMPLES 13
#define PROT_CPU_READ 1
#define OPEN_WRITE_CREATE_APPEND (0x0001 | 0x0200 | 0x0008)
// How long the thread looks for the in-game menu's module, and how often. It has to catch it before the core registers its display buffers.
#define SEARCH_US 180000000u
#define SEARCH_STEP_US 10000u

attr_public const char *g_pluginName = PLUGIN_NAME;
attr_public const char *g_pluginDesc = "Mide cuadros distintos y envios a la grafica.";
attr_public const char *g_pluginAuth = "Forja SX2";
attr_public u32 g_pluginVersion = 0x00000300;

int32_t sceGnmSubmitAndFlipCommandBuffers(uint32_t count, void *dcb[], uint32_t *dcbSizes, void *ccb[], uint32_t *ccbSizes, uint32_t handle,
	uint32_t index, uint32_t flipMode, int64_t flipArg);
int32_t sceGnmSubmitAndFlipCommandBuffersForWorkload(uint32_t workload, uint32_t count, void *dcb[], uint32_t *dcbSizes, void *ccb[], uint32_t *ccbSizes,
	uint32_t handle, uint32_t index, uint32_t flipMode, int64_t flipArg);

HOOK_INIT(sceGnmSubmitAndFlipCommandBuffers);
HOOK_INIT(sceGnmSubmitAndFlipCommandBuffersForWorkload);
HOOK_INIT(sceVideoOutRegisterBuffers);
HOOK_INIT(sceVideoOutSubmitFlip);

typedef struct
{
	const uint32_t *pixels;
	uint32_t width, height, pitch;
	int readable; // -1 not checked, 0 no, 1 yes
} Buffer;

static Buffer s_buffers[MAX_BUFFERS];
static uint32_t s_previous[GRID_W * GRID_H], s_current[GRID_W * GRID_H];
static bool s_have_previous = false, s_hooked = false;
static volatile bool s_stop = false;
static int s_last_index = -1;
static int s_log = -1;

// One window of 5 s.
static uint64_t s_window_start = 0, s_last_flip = 0, s_last_gnm = 0;
static uint32_t s_flips = 0, s_same = 0, s_slight = 0, s_new = 0, s_unread = 0, s_over_25ms = 0, s_over_42ms = 0;
static uint64_t s_gap_max = 0, s_dcb_bytes = 0, s_submits = 0, s_submit_us = 0, s_read_us = 0;

static void Log(const char *format, ...)
{
	if (s_log < 0)
		return;
	char line[320];
	va_list args;
	va_start(args, format);
	const int size = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (size > 0)
		sceKernelWrite(s_log, line, (size_t)size < sizeof(line) ? (size_t)size : sizeof(line) - 1);
}

static bool BufferReadable(Buffer *b)
{
	if (b->readable < 0)
	{
		void *start = NULL, *end = NULL;
		int32_t prot = 0;
		b->readable = (b->pixels && sceKernelQueryMemoryProtection((void *)b->pixels, &start, &end, &prot) == 0 && (prot & PROT_CPU_READ)) ? 1 : 0;
		Log("buffer %p %ux%u pitch %u: %s (prot 0x%x)\n", (const void *)b->pixels, b->width, b->height, b->pitch, b->readable ? "readable" : "NOT readable", (unsigned)prot);
	}
	return b->readable == 1;
}

// A grid of pixels from the picture shown last: the buffer about to be replaced is finished by now. Leaves out the top left corner,
// where the FPS counter goes.
static void ComparePicture(int index)
{
	if (index < 0 || index >= MAX_BUFFERS || !BufferReadable(&s_buffers[index]))
	{
		s_unread++;
		return;
	}
	const Buffer *b = &s_buffers[index];
	const uint64_t start = sceKernelGetProcessTime();
	uint32_t changed = 0;
	for (uint32_t y = 0; y < GRID_H; y++)
	{
		const uint32_t py = (y * b->height) / GRID_H;
		for (uint32_t x = 0; x < GRID_W; x++)
		{
			const uint32_t i = y * GRID_W + x;
			s_current[i] = (x < GRID_W / 5 && y < GRID_H / 8) ? 0 : b->pixels[py * b->pitch + (x * b->width) / GRID_W];
			changed += s_current[i] != s_previous[i];
		}
	}
	s_read_us += sceKernelGetProcessTime() - start;
	if (s_have_previous)
	{
		if (changed == 0)
			s_same++;
		else if (changed < SLIGHT_SAMPLES)
			s_slight++;
		else
			s_new++;
	}
	memcpy(s_previous, s_current, sizeof(s_previous));
	s_have_previous = true;
}

static void Flip(int index)
{
	const uint64_t now = sceKernelGetProcessTime();
	if (s_window_start == 0)
		s_window_start = now;
	if (s_last_flip != 0)
	{
		const uint64_t gap = now - s_last_flip;
		if (gap > s_gap_max)
			s_gap_max = gap;
		s_over_25ms += gap >= 25000;
		s_over_42ms += gap >= 42000;
	}
	s_last_flip = now;
	s_flips++;
	if (s_last_index >= 0)
		ComparePicture(s_last_index);
	s_last_index = index;
	const uint64_t elapsed = now - s_window_start;
	if (elapsed >= WINDOW_US)
	{
		const double seconds = (double)elapsed / 1000000.0;
		const uint32_t compared = s_same + s_slight + s_new;
		Log("tramo s=%.2f imagenes/s=%.1f nuevas/s=%.1f leves/s=%.1f iguales/s=%.1f sin_leer=%u mayor_hueco_ms=%.1f sobre25ms=%u sobre42ms=%u "
			"envio_kb=%.1f envio_us=%.1f lectura_us=%.1f\n",
			seconds, s_flips / seconds, s_new / seconds, s_slight / seconds, s_same / seconds, s_unread, (double)s_gap_max / 1000.0, s_over_25ms, s_over_42ms,
			s_submits ? (double)s_dcb_bytes / s_submits / 1024.0 : 0.0, s_submits ? (double)s_submit_us / s_submits : 0.0,
			compared ? (double)s_read_us / compared : 0.0);
		s_window_start = now;
		s_flips = s_same = s_slight = s_new = s_unread = s_over_25ms = s_over_42ms = 0;
		s_gap_max = s_dcb_bytes = s_submits = s_submit_us = s_read_us = 0;
	}
}

static void CountSubmit(uint32_t count, const uint32_t *sizes)
{
	for (uint32_t i = 0; sizes && i < count; i++)
		s_dcb_bytes += sizes[i];
	s_submits++;
}

int32_t sceGnmSubmitAndFlipCommandBuffers_hook(uint32_t count, void *dcb[], uint32_t *dcbSizes, void *ccb[], uint32_t *ccbSizes, uint32_t handle,
	uint32_t index, uint32_t flipMode, int64_t flipArg)
{
	s_last_gnm = sceKernelGetProcessTime();
	Flip((int)index);
	CountSubmit(count, dcbSizes);
	const uint64_t start = sceKernelGetProcessTime();
	const int32_t result = HOOK_CONTINUE(sceGnmSubmitAndFlipCommandBuffers, int32_t(*)(uint32_t, void **, uint32_t *, void **, uint32_t *, uint32_t, uint32_t, uint32_t, int64_t),
		count, dcb, dcbSizes, ccb, ccbSizes, handle, index, flipMode, flipArg);
	s_submit_us += sceKernelGetProcessTime() - start;
	return result;
}

int32_t sceGnmSubmitAndFlipCommandBuffersForWorkload_hook(uint32_t workload, uint32_t count, void *dcb[], uint32_t *dcbSizes, void *ccb[], uint32_t *ccbSizes,
	uint32_t handle, uint32_t index, uint32_t flipMode, int64_t flipArg)
{
	s_last_gnm = sceKernelGetProcessTime();
	Flip((int)index);
	CountSubmit(count, dcbSizes);
	const uint64_t start = sceKernelGetProcessTime();
	const int32_t result = HOOK_CONTINUE(sceGnmSubmitAndFlipCommandBuffersForWorkload,
		int32_t(*)(uint32_t, uint32_t, void **, uint32_t *, void **, uint32_t *, uint32_t, uint32_t, uint32_t, int64_t),
		workload, count, dcb, dcbSizes, ccb, ccbSizes, handle, index, flipMode, flipArg);
	s_submit_us += sceKernelGetProcessTime() - start;
	return result;
}

int32_t sceVideoOutRegisterBuffers_hook(int32_t handle, int32_t start, void *const *addresses, int32_t count, const OrbisVideoOutBufferAttribute *attribute)
{
	for (int32_t i = 0; addresses && attribute && i < count && start + i < MAX_BUFFERS; i++)
	{
		if (start + i < 0)
			continue;
		Buffer *b = &s_buffers[start + i];
		b->pixels = (const uint32_t *)addresses[i];
		b->width = attribute->width;
		b->height = attribute->height;
		b->pitch = attribute->pixelPitch;
		b->readable = -1;
	}
	s_have_previous = false;
	s_last_index = -1;
	Log("buffers registrados: desde %d, %d\n", start, count);
	return HOOK_CONTINUE(sceVideoOutRegisterBuffers, int32_t(*)(int32_t, int32_t, void *const *, int32_t, const OrbisVideoOutBufferAttribute *),
		handle, start, addresses, count, attribute);
}

int32_t sceVideoOutSubmitFlip_hook(int32_t handle, int32_t index, uint32_t flipMode, int64_t flipArg)
{
	// Only a flip of its own: the GNM call above may come through here too.
	if (sceKernelGetProcessTime() - s_last_gnm > 2000)
		Flip(index);
	return HOOK_CONTINUE(sceVideoOutSubmitFlip, int32_t(*)(int32_t, int32_t, uint32_t, int64_t), handle, index, flipMode, flipArg);
}

static bool FindMenuModule(size_t *modules_seen)
{
	OrbisKernelModule list[MAX_MODULES];
	size_t count = 0;
	if (sceKernelGetModuleList(list, sizeof(list), &count) != 0)
		return false;
	// The call gives the number of modules, not bytes. Version 2 divided it by 4, saw a quarter of the list and never found the menu's module.
	bool found = false;
	for (size_t i = 0; i < count && i < MAX_MODULES; i++)
	{
		OrbisKernelModuleInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(list[i], &info) != 0)
			continue;
		// Only the ones new since the last look: the list only grows at the end.
		if (i >= *modules_seen)
			Log("modulo %s\n", info.name);
		found = found || strstr(info.name, "menu") != NULL;
	}
	*modules_seen = count;
	return found;
}

static void *SearchThread(void *argument)
{
	size_t modules_seen = 0;
	Log("hilo: buscando el modulo del menu\n");
	for (uint64_t waited = 0; waited < SEARCH_US && !s_stop; waited += SEARCH_STEP_US)
	{
		if (FindMenuModule(&modules_seen))
		{
			Log("hilo: menu encontrado, enganchando\n");
			HOOK32(sceVideoOutRegisterBuffers);
			HOOK32(sceVideoOutSubmitFlip);
			HOOK32(sceGnmSubmitAndFlipCommandBuffers);
			HOOK32(sceGnmSubmitAndFlipCommandBuffersForWorkload);
			s_hooked = true;
			Log("hilo: enganchado\n");
			scePthreadExit(NULL);
		}
		sceKernelUsleep(SEARCH_STEP_US);
	}
	Log("hilo: sin menu, no se engancha nada\n");
	scePthreadExit(NULL);
	return NULL;
}

s32 attr_public plugin_load(s32 argc, const char *argv[])
{
	final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
	for (int i = 0; i < MAX_BUFFERS; i++)
		s_buffers[i].readable = -1;
	sceKernelMkdir(LOG_FOLDER, 0777);
	s_log = sceKernelOpen(LOG_PATH, OPEN_WRITE_CREATE_APPEND, 0666);
	Log("medidor v3: cargado, proceso nuevo\n");
	OrbisPthread thread;
	const int32_t created = scePthreadCreate(&thread, NULL, SearchThread, NULL, "medidor_buscar");
	Log("hilo creado: 0x%08x\n", (unsigned)created);
	return 0;
}

s32 attr_public plugin_unload(s32 argc, const char *argv[])
{
	s_stop = true;
	if (s_hooked)
	{
		UNHOOK(sceGnmSubmitAndFlipCommandBuffersForWorkload);
		UNHOOK(sceGnmSubmitAndFlipCommandBuffers);
		UNHOOK(sceVideoOutSubmitFlip);
		UNHOOK(sceVideoOutRegisterBuffers);
	}
	if (s_log >= 0)
		sceKernelClose(s_log);
	s_log = -1;
	return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args)
{
	return 0;
}

s32 attr_module_hidden module_stop(s64 argc, const void *args)
{
	return 0;
}
