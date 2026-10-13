// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

#if defined(ORBIS_VULKAN) || defined(PCSX2_PS4)
// (AI-assisted) Orbis builds link their Vulkan ICD into the eboot rather than loading a desktop Vulkan loader.
// The ICD's entry point stands in for vkGetInstanceProcAddr.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

static bool s_vulkan_library_loaded = false;

bool Vulkan::IsVulkanLibraryLoaded()
{
	return s_vulkan_library_loaded;
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
	pxAssertRel(!s_vulkan_library_loaded, "Vulkan module is not loaded.");
	vkGetInstanceProcAddr = vk_icdGetInstanceProcAddr;

	// With no instance, an ICD answers only the global commands. vkDestroyInstance is
	// a module export of the desktop loader but an instance command of the driver:
	// LoadVulkanInstanceFunctions fetches it once the instance exists.
	bool required_functions_missing = false;
#define VULKAN_MODULE_ENTRY_POINT(name, required) \
	if (std::strcmp(#name, "vkGetInstanceProcAddr") != 0 && std::strcmp(#name, "vkDestroyInstance") != 0) \
	{ \
		name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
		if (!name && required) \
		{ \
			ERROR_LOG("Vulkan: the linked driver has no module function {}", #name); \
			required_functions_missing = true; \
		} \
	}

#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

	if (required_functions_missing)
	{
		ResetVulkanLibraryFunctionPointers();
		Error::SetStringView(error, "The linked Vulkan driver lacks a required module function.");
		return false;
	}

	s_vulkan_library_loaded = true;
	return true;
}

void Vulkan::UnloadVulkanLibrary()
{
	ResetVulkanLibraryFunctionPointers();
	s_vulkan_library_loaded = false;
}
#else
static DynamicLibrary s_vulkan_library;

bool Vulkan::IsVulkanLibraryLoaded()
{
	return s_vulkan_library.IsOpen();
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
	pxAssertRel(!s_vulkan_library.IsOpen(), "Vulkan module is not loaded.");

#ifdef __APPLE__
	// Check if a path to a specific Vulkan library has been specified.
	char* libvulkan_env = getenv("LIBVULKAN_PATH");
	if (libvulkan_env)
		s_vulkan_library.Open(libvulkan_env, error);
	if (!s_vulkan_library.IsOpen() &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("MoltenVK").c_str(), error))
	{
		return false;
	}
#else
	// try versioned first, then unversioned.
	if (!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
	{
		return false;
	}
#endif

	bool required_functions_missing = false;
#define VULKAN_MODULE_ENTRY_POINT(name, required) \
	if (!s_vulkan_library.GetSymbol(#name, &name)) \
	{ \
		ERROR_LOG("Vulkan: Failed to load required module function {}", #name); \
		required_functions_missing = true; \
	}

#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

	if (required_functions_missing)
	{
		ResetVulkanLibraryFunctionPointers();
		s_vulkan_library.Close();
		return false;
	}

	return true;
}

void Vulkan::UnloadVulkanLibrary()
{
	ResetVulkanLibraryFunctionPointers();
	s_vulkan_library.Close();
}
#endif

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, instance](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetInstanceProcAddr(instance, name);
#if defined(ORBIS_VULKAN) || defined(PCSX2_PS4)
		// A core 1.1 command the driver answers only by its extension's name.
		if (!(*func_ptr))
			*func_ptr = vkGetInstanceProcAddr(instance, (std::string(name) + "KHR").c_str());
#endif
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required instance function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_INSTANCE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT
#if defined(ORBIS_VULKAN) || defined(PCSX2_PS4)
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&vkDestroyInstance), "vkDestroyInstance", true);
#endif

	return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, device](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetDeviceProcAddr(device, name);
#if defined(ORBIS_VULKAN) || defined(PCSX2_PS4)
		if (!(*func_ptr))
			*func_ptr = vkGetDeviceProcAddr(device, (std::string(name) + "KHR").c_str());
#endif
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required device function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_DEVICE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

	return !required_functions_missing;
}
