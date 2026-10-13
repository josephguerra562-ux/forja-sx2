// (AI-assisted) stb_image implementation for user-provided PS4SX2 covers.
// Covers are loaded from /data/PS4SX2/covers and are never packaged with the
// emulator. The launcher copies decoded pixels into Dear ImGui texture data;
// GSDeviceVK uploads them on the existing Vulkan/ImGui path.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
