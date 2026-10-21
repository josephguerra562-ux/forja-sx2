# (AI-assisted) Link the real PCSX2 Vulkan device and runtime shader compiler for PS4.
cmake_minimum_required(VERSION 3.20)
project(pcsx2_ps4_vm C CXX)
# (AI-assisted) Restore the game's Vulkan authority, paired with CATEGORY=gd.
# The CyberTV mini-app profile exposes only 768 MiB and froze during RADV init.
set(PS4_PAID "0x3800000000000011" CACHE STRING "Program authority ID of eboot.bin" FORCE)
option(PCSX2_VM_JIT "Enable the experimental x86 EE/IOP/VU recompilers" ON)
option(PCSX2_VM_FASTMEM "Enable experimental 4 GiB VTLB fastmem and Orbis fault handling" OFF)
option(PCSX2_VM_MTVU "Enable experimental multithreaded VU1" ON)
if(PCSX2_VM_FASTMEM AND NOT PCSX2_VM_JIT)
  message(FATAL_ERROR "PCSX2_VM_FASTMEM requires PCSX2_VM_JIT=ON")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)
get_filename_component(PCSX2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(FetchContent)
# (AI-assisted) OpenOrbis's toolchain labels the target Generic; glslang uses its POSIX platform implementation.
set(UNIX TRUE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  URL https://codeload.github.com/KhronosGroup/glslang/tar.gz/275822a6261ee689aadb1da5f09a0ec2f058685c
  URL_HASH SHA256=971848a1cc639ce8dc244e778b17efe0f690e32ac398a75e31d1c67ad06d3e0a)
FetchContent_MakeAvailable(glslang)
FetchContent_Declare(shaderc_headers
  URL https://codeload.github.com/google/shaderc/tar.gz/d5f08ae5c5a9a45165578445cbd0f9adf0223448
  URL_HASH SHA256=60256e43ae8ce73cd2b9d7d7ceaeabed4d79daf0d306fecf1f26bc7d61e68b16)
FetchContent_GetProperties(shaderc_headers)
if(NOT shaderc_headers_POPULATED)
  FetchContent_Populate(shaderc_headers)
endif()
set(gs_sources
  pcsx2/GS/Renderers/Common/GSDevice.cpp pcsx2/GS/Renderers/Common/GSTexture.cpp
  pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp pcsx2/GS/Renderers/Vulkan/GSTextureVK.cpp
  pcsx2/GS/Renderers/Vulkan/VKBuilders.cpp pcsx2/GS/Renderers/Vulkan/VKLoader.cpp
  pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp pcsx2/GS/Renderers/Vulkan/VKStreamBuffer.cpp
  pcsx2/GS/Renderers/Vulkan/VKSwapChain.cpp pcsx2/GS/Renderers/Vulkan/vk_mem_alloc.cpp
  pcsx2/GS/GS.cpp pcsx2/GS/GSVector.cpp pcsx2/GS/GSPerfMon.cpp pcsx2/Pcsx2Config.cpp
  pcsx2/GS/GSState.cpp pcsx2/GS/GSLocalMemory.cpp pcsx2/GS/GSLocalMemoryMultiISA.cpp
  pcsx2/GS/GSBlock.cpp pcsx2/GS/GSClut.cpp pcsx2/GS/GSTables.cpp pcsx2/GS/GSUtil.cpp
  pcsx2/GS/GSDrawingContext.cpp pcsx2/GS/GSDrawingEnvironment.cpp pcsx2/GS/GSXXH.cpp pcsx2/GS/GSPng.cpp
  pcsx2/GS/Renderers/Common/GSVertexTrace.cpp pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp
  pcsx2/GS/Renderers/Common/GSDirtyRect.cpp
  pcsx2/GS/Renderers/HW/GSRendererHW.cpp pcsx2/GS/Renderers/HW/GSRendererHWMultiISA.cpp
  pcsx2/GS/Renderers/HW/GSTextureCache.cpp pcsx2/GS/Renderers/HW/GSHwHack.cpp
  pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp pcsx2/GS/Renderers/HW/GSTextureReplacementLoaders.cpp
  pcsx2/GS/Renderers/SW/GSTextureCacheSW.cpp
  ps4/core/GSRendererPS4.cpp ps4/core/GSProcessorPS4.cpp
  ps4/core/framegen/ps5_framegen.cpp
  pcsx2/MTGS.cpp pcsx2/Gif_Unit.cpp pcsx2/Gif.cpp common/Semaphore.cpp common/Linux/LnxThreads.cpp
  pcsx2/Dmac.cpp pcsx2/Hw.cpp pcsx2/R5900.cpp
  pcsx2/vtlb.cpp pcsx2/R5900OpcodeImpl.cpp pcsx2/R5900OpcodeTables.cpp pcsx2/Interpreter.cpp
  pcsx2/COP0.cpp pcsx2/Memory.cpp pcsx2/FPU.cpp pcsx2/MMI.cpp pcsx2/Cache.cpp
  pcsx2/R3000A.cpp pcsx2/R3000AInterpreter.cpp pcsx2/R3000AOpcodeTables.cpp pcsx2/IopMem.cpp pcsx2/IopGte.cpp
  pcsx2/Sif0.cpp pcsx2/Sif1.cpp pcsx2/IopDma.cpp pcsx2/IopIrq.cpp ps4/core/MemoryMapPS4.cpp
  common/AlignedMalloc.cpp common/SmallString.cpp common/StringUtil.cpp
  common/Console.cpp common/Timer.cpp common/Error.cpp common/WindowInfo.cpp
  common/FileSystem.cpp common/MD5Digest.cpp common/ReadbackSpinManager.cpp
  common/Image.cpp common/FastJmp.cpp common/TextureDecompress.cpp
  ps4/core/VulkanVideoOut.cpp ps4/core/HostSysPS4.cpp ps4/core/OrbisCompatPS4.cpp ps4/core/ThreadingPS4.cpp common/HostSys.cpp
  ps4/compat/ShaderCompiler.cpp
  3rdparty/fmt/src/format.cc
  3rdparty/imgui/src/imgui.cpp 3rdparty/imgui/src/imgui_draw.cpp
  3rdparty/imgui/src/imgui_tables.cpp 3rdparty/imgui/src/imgui_widgets.cpp)

set(vm_sources
  common/AlignedMalloc.cpp
  common/HostSys.cpp
  common/SmallString.cpp
  pcsx2/Achievements.cpp
  pcsx2/CDVD/CDVD.cpp
  pcsx2/CDVD/CDVDcommon.cpp
  pcsx2/CDVD/CDVDisoReader.cpp
  pcsx2/CDVD/ChdFileReader.cpp
  pcsx2/CDVD/CsoFileReader.cpp
  pcsx2/CDVD/FlatFileReader.cpp
  pcsx2/CDVD/GzippedFileReader.cpp
  pcsx2/CDVD/InputIsoFile.cpp
  pcsx2/CDVD/IsoReader.cpp
  pcsx2/CDVD/Ps1CD.cpp
  pcsx2/COP0.cpp
  pcsx2/COP2.cpp
  pcsx2/Counters.cpp
  pcsx2/DebugTools/SymbolGuardian.cpp
  pcsx2/DebugTools/SymbolImporter.cpp
  pcsx2/DEV9/DEV9.cpp
  pcsx2/DEV9/flash.cpp
  pcsx2/DEV9/smap.cpp
  pcsx2/Dmac.cpp
  pcsx2/FPU.cpp
  pcsx2/FW.cpp
  pcsx2/GameDatabase.cpp
  pcsx2/Gif_Unit.cpp
  pcsx2/Gif.cpp
  pcsx2/GS/GS.cpp
  pcsx2/GS/GSBlock.cpp
  pcsx2/GS/GSClut.cpp
  pcsx2/GS/GSDrawingContext.cpp
  pcsx2/GS/GSDrawingEnvironment.cpp
  pcsx2/GS/GSLocalMemory.cpp
  pcsx2/GS/GSLocalMemoryMultiISA.cpp
  pcsx2/GS/GSPerfMon.cpp
  pcsx2/GS/GSRingHeap.cpp
  pcsx2/GS/GSState.cpp
  pcsx2/GS/GSTables.cpp
  pcsx2/GS/GSUtil.cpp
  pcsx2/GS/GSVector.cpp
  pcsx2/GS/GSXXH.cpp
  pcsx2/GS/MultiISA.cpp
  pcsx2/GS/Renderers/Null/GSRendererNull.cpp
  pcsx2/GSDumpReplayer.cpp
  pcsx2/Hw.cpp
  pcsx2/HwRead.cpp
  pcsx2/HwWrite.cpp
  pcsx2/INISettingsInterface.cpp
  pcsx2/Interpreter.cpp
  pcsx2/IopBios.cpp
  pcsx2/IopCounters.cpp
  pcsx2/IopDma.cpp
  pcsx2/IopGte.cpp
  pcsx2/IopHw.cpp
  pcsx2/IopIrq.cpp
  pcsx2/IopMem.cpp
  pcsx2/IPU/IPU_Fifo.cpp
  pcsx2/IPU/IPU.cpp
  pcsx2/IPU/IPUdither.cpp
  pcsx2/IPU/IPUdma.cpp
  pcsx2/IPU/yuv2rgb.cpp
  pcsx2/Memory.cpp
  pcsx2/MMI.cpp
  pcsx2/MTGS.cpp
  pcsx2/MTVU.cpp
  pcsx2/Patch.cpp
  pcsx2/Pcsx2Config.cpp
  pcsx2/PINE.cpp
  pcsx2/ps2/BiosTools.cpp
  pcsx2/ps2/Iop/PsxBios.cpp
  pcsx2/R3000A.cpp
  pcsx2/R3000AInterpreter.cpp
  pcsx2/R3000AOpcodeTables.cpp
  pcsx2/R5900.cpp
  pcsx2/R5900OpcodeImpl.cpp
  pcsx2/R5900OpcodeTables.cpp
  pcsx2/Recording/InputRecording.cpp
  pcsx2/Sif0.cpp
  pcsx2/Sif1.cpp
  pcsx2/SIO/Memcard/MemoryCardFile.cpp
  pcsx2/SIO/Multitap/MultitapProtocol.cpp
  pcsx2/SIO/Pad/Pad.cpp
  pcsx2/SIO/Pad/PadBase.cpp
  pcsx2/SIO/Pad/PadDualshock2.cpp
  pcsx2/SIO/Pad/PadGuitar.cpp
  pcsx2/SIO/Pad/PadJogcon.cpp
  pcsx2/SIO/Pad/PadNegcon.cpp
  pcsx2/SIO/Pad/PadNotConnected.cpp
  pcsx2/SIO/Pad/PadPopn.cpp
  pcsx2/SIO/Sio.cpp
  pcsx2/SIO/Sio0.cpp
  pcsx2/SIO/Sio2.cpp
  pcsx2/SourceLog.cpp
  pcsx2/SPU2/ADSR.cpp
  pcsx2/SPU2/Debug.cpp
  pcsx2/SPU2/Dma.cpp
  pcsx2/SPU2/Mixer.cpp
  pcsx2/SPU2/ReadInput.cpp
  pcsx2/SPU2/RegTable.cpp
  pcsx2/SPU2/Reverb.cpp
  pcsx2/SPU2/ReverbResample.cpp
  pcsx2/SPU2/spu2.cpp
  pcsx2/SPU2/spu2freeze.cpp
  pcsx2/SPU2/spu2sys.cpp
  pcsx2/SPU2/Wavedump_wav.cpp
  pcsx2/USB/USB.cpp
  pcsx2/Vif_Transfer.cpp
  pcsx2/Vif_Unpack.cpp
  pcsx2/Vif.cpp
  pcsx2/Vif0_Dma.cpp
  pcsx2/Vif1_Dma.cpp
  pcsx2/VMManager.cpp
  pcsx2/vtlb.cpp
  pcsx2/VU0micro.cpp
  pcsx2/VU0microInterp.cpp
  pcsx2/VU1micro.cpp
  pcsx2/VU1microInterp.cpp
  pcsx2/VUflags.cpp
  pcsx2/VUmicro.cpp
  pcsx2/VUmicroMem.cpp
  pcsx2/VUops.cpp
  pcsx2/x86/microVU.cpp
  pcsx2/x86/Vif_Dynarec.cpp
  ps4/core/GSRendererPS4.cpp
  ps4/core/HostSysPS4.cpp
  ps4/core/MemoryMapPS4.cpp
  ps4/core/OrbisCompatPS4.cpp
  ps4/core/ThreadingPS4.cpp
)
FetchContent_Declare(ryml
  URL https://github.com/biojppm/rapidyaml/releases/download/v0.12.1/rapidyaml-0.12.1-src.tgz
  URL_HASH SHA256=e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d)
set(RYML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RYML_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ryml)
list(REMOVE_ITEM vm_sources pcsx2/GS/MultiISA.cpp)
if(NOT PCSX2_VM_JIT)
  list(REMOVE_ITEM vm_sources pcsx2/x86/microVU.cpp pcsx2/x86/Vif_Dynarec.cpp)
endif()
if(PCSX2_VM_JIT)
  # (AI-assisted) Keep the stable interpreter target unchanged; this target adds
  # the existing x86 emitter/recompiler sources for a separate experiment.
  list(APPEND vm_sources
    pcsx2/x86/BaseblockEx.cpp pcsx2/x86/iCOP0.cpp pcsx2/x86/iCore.cpp
    pcsx2/x86/iFPU.cpp pcsx2/x86/iFPUd.cpp pcsx2/x86/iMMI.cpp
    pcsx2/x86/iR3000A.cpp pcsx2/x86/iR3000Atables.cpp pcsx2/x86/iR5900Analysis.cpp
    pcsx2/x86/iR5900Misc.cpp pcsx2/x86/Vif_Dynarec.cpp pcsx2/x86/Vif_UnpackSSE.cpp
    pcsx2/x86/microVU.cpp
    pcsx2/x86/ix86-32/iCore.cpp pcsx2/x86/ix86-32/iR5900.cpp
    pcsx2/x86/ix86-32/iR5900Arit.cpp pcsx2/x86/ix86-32/iR5900AritImm.cpp
    pcsx2/x86/ix86-32/iR5900Branch.cpp pcsx2/x86/ix86-32/iR5900Jump.cpp
    pcsx2/x86/ix86-32/iR5900LoadStore.cpp pcsx2/x86/ix86-32/iR5900Move.cpp
    pcsx2/x86/ix86-32/iR5900MultDiv.cpp pcsx2/x86/ix86-32/iR5900Shift.cpp
    pcsx2/x86/ix86-32/iR5900Templates.cpp pcsx2/x86/ix86-32/recVTLB.cpp)
endif()
list(APPEND vm_sources ${gs_sources}
  pcsx2/GS.cpp pcsx2/Sif.cpp pcsx2/sif2.cpp pcsx2/SPR.cpp pcsx2/FiFo.cpp
  pcsx2/COP2.cpp pcsx2/VU0.cpp pcsx2/Vif_Codes.cpp pcsx2/Vif1_MFIFO.cpp
  pcsx2/Mdec.cpp pcsx2/Elfheader.cpp pcsx2/ShiftJisToUnicode.cpp pcsx2/GameList.cpp
  common/Perf.cpp
  common/MemorySettingsInterface.cpp pcsx2/LayeredSettingsInterface.cpp common/SettingsWrapper.cpp
  common/ProgressCallback.cpp  common/YAML.cpp common/MemoryInterface.cpp
  common/DynamicLibrary.cpp common/HTTPDownloader.cpp common/WAVWriter.cpp
  pcsx2/Host/AudioStream.cpp pcsx2/PerformanceMetrics.cpp pcsx2/Host.cpp
  pcsx2/ps2/Iop/IopHwRead.cpp pcsx2/ps2/Iop/IopHwWrite.cpp pcsx2/ps2/pgif.cpp
  pcsx2/IPU/IPU_MultiISA.cpp pcsx2/StateWrapper.cpp pcsx2/SaveState.cpp
  pcsx2/SIO/Memcard/MemoryCardProtocol.cpp pcsx2/SIO/Memcard/MemoryCardFolder.cpp
  pcsx2/CDVD/ThreadedFileReader.cpp pcsx2/CDVD/BlockdumpFileReader.cpp
  pcsx2/CDVD/OutputIsoFile.cpp
  pcsx2/Recording/InputRecordingControls.cpp pcsx2/Recording/InputRecordingFile.cpp pcsx2/Recording/PadData.cpp
  pcsx2/Input/InputManager.cpp pcsx2/Input/InputSource.cpp
  pcsx2/GS/Renderers/SW/GSRendererSW.cpp pcsx2/GS/Renderers/SW/GSRasterizer.cpp
  pcsx2/GS/Renderers/SW/GSDrawScanline.cpp
  pcsx2/GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
  pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
  pcsx2/GS/Renderers/Common/GSFunctionMap.cpp pcsx2/GS/GSRingHeap.cpp
  )
file(GLOB extra_sources RELATIVE "${PCSX2_ROOT}"
  "${PCSX2_ROOT}/common/emitter/*.cpp"
  "${PCSX2_ROOT}/3rdparty/ccc/src/ccc/*.cpp"
  "${PCSX2_ROOT}/3rdparty/soundtouch/source/SoundTouch/*.cpp"
  "${PCSX2_ROOT}/3rdparty/freesurround/src/*.cpp"
  "${PCSX2_ROOT}/pcsx2/DebugTools/*.cpp")
list(APPEND vm_sources ${extra_sources})
# (AI-assisted) Real device/controller implementations; USB ports start disconnected.
file(GLOB_RECURSE device_sources RELATIVE "${PCSX2_ROOT}" "${PCSX2_ROOT}/pcsx2/USB/*.cpp" "${PCSX2_ROOT}/pcsx2/DEV9/ATA/*.cpp")
list(FILTER device_sources EXCLUDE REGEX "(cam-(linux|windows|macos)|audiodev-cubeb|usb-pad-sdl-ff|/Win32/|pcap_io)\\.?")
list(APPEND vm_sources ${device_sources}
  ps4/compat/InputStubs.cpp ps4/compat/AudioStubs.cpp
  ps4/compat/SaveStateStubs.cpp ps4/compat/ZipStubs.cpp
)
file(GLOB achievement_sources RELATIVE "${PCSX2_ROOT}"
  "${PCSX2_ROOT}/3rdparty/rcheevos/src/*.c" "${PCSX2_ROOT}/3rdparty/rcheevos/src/rapi/*.c"
  "${PCSX2_ROOT}/3rdparty/rcheevos/src/rcheevos/*.c" "${PCSX2_ROOT}/3rdparty/rcheevos/src/rhash/*.c")
list(FILTER achievement_sources EXCLUDE REGEX "(rc_libretro|rc_client_raintegration|/rhash/hash|/rhash/cdreader)\\.c$")
list(APPEND vm_sources ${achievement_sources})
list(REMOVE_DUPLICATES vm_sources)
list(TRANSFORM vm_sources PREPEND "${PCSX2_ROOT}/")
# (AI-assisted) Use the bundled CHD reader/codecs, not the BIOS bring-up stub.
add_library(ps4_chd STATIC
  "${PCSX2_ROOT}/3rdparty/libchdr/src/libchdr_bitstream.c"
  "${PCSX2_ROOT}/3rdparty/libchdr/src/libchdr_cdrom.c"
  "${PCSX2_ROOT}/3rdparty/libchdr/src/libchdr_chd.c"
  "${PCSX2_ROOT}/3rdparty/libchdr/src/libchdr_flac.c"
  "${PCSX2_ROOT}/3rdparty/libchdr/src/libchdr_huffman.c"
  "${PCSX2_ROOT}/3rdparty/lzma/src/LzmaDec.c")
target_include_directories(ps4_chd PUBLIC "${PCSX2_ROOT}/3rdparty/libchdr/include"
  PRIVATE "${PCSX2_ROOT}/3rdparty/lzma/include" "${PCSX2_ROOT}/ps4/deps/prefix/include")
target_compile_definitions(ps4_chd PRIVATE _7ZIP_ST)
target_compile_options(ps4_chd PRIVATE -march=btver2 -ffunction-sections -fdata-sections -fvisibility=hidden)
target_link_libraries(ps4_chd PRIVATE z "${PCSX2_ROOT}/ps4/deps/prefix/lib/libzstd.a")
# (AI-assisted) PS1 SPU runs at 44.1 kHz; AudioOut remains at 48 kHz.
# Reuse the bundled Speex resampler without changing vendored sources.
add_library(ps4_audio_resampler STATIC "${PCSX2_ROOT}/3rdparty/cubeb/subprojects/speex/resample.c")
target_compile_definitions(ps4_audio_resampler PRIVATE OUTSIDE_SPEEX FLOATING_POINT RANDOM_PREFIX=ps4sx2)
target_compile_options(ps4_audio_resampler PRIVATE -march=btver2 -ffunction-sections -fdata-sections -fvisibility=hidden)
set_property(SOURCE "${PCSX2_ROOT}/ps4/core/AudioStreamPS4.cpp"
  APPEND PROPERTY COMPILE_DEFINITIONS OUTSIDE_SPEEX RANDOM_PREFIX=ps4sx2)
add_executable(vm_boot main.cpp GameMenu.cpp CoverImage.cpp CoverDownload.cpp ReleaseCheck.cpp HostVM.cpp UIVM.cpp CPUInfoVM.cpp NetworkVM.cpp CaptureVM.cpp DiscVM.cpp PhysicalDisc.cpp DiscCopy.cpp PatchDownload.cpp PlatformVM.cpp
  "${PCSX2_ROOT}/ps4/core/ThreadCPUMetricsPS4.cpp" "${PCSX2_ROOT}/ps4/core/IPUProfilePS4.cpp"
  "${PCSX2_ROOT}/ps4/core/GSProfilePS4.cpp"
  "${PCSX2_ROOT}/ps4/core/EEEventProfilePS4.cpp"
  "${PCSX2_ROOT}/ps4/core/EEHotspotsPS4.cpp"
  "${PCSX2_ROOT}/ps4/core/ThreadAffinityPS4.cpp"
  "${PCSX2_ROOT}/ps4/core/AudioStreamPS4.cpp"
  "${PCSX2_ROOT}/ps4/core/ProbeLoggerPS4.cpp"
  "${PCSX2_ROOT}/ps4/core/TelemetryPS4.cpp"
  ${vm_sources} ${PS4_RUNTIME_SOURCES})
if(PCSX2_VM_JIT)
  target_sources(vm_boot PRIVATE "${PCSX2_ROOT}/ps4/core/JitThreadStatePS4.cpp")
endif()
# (AI-assisted) imconfig.h selects FreeType globally. This standalone frontend uses
# the bundled stb loader; compile its implementation and the adapter declaration.
set_property(SOURCE "${PCSX2_ROOT}/3rdparty/imgui/src/imgui_draw.cpp" UIVM.cpp
  APPEND PROPERTY COMPILE_DEFINITIONS IMGUI_ENABLE_STB_TRUETYPE=1)
target_include_directories(vm_boot PRIVATE
  ${PCSX2_ROOT} ${PCSX2_ROOT}/pcsx2 ${PCSX2_ROOT}/3rdparty/fmt/include
  ${PCSX2_ROOT}/3rdparty/fast_float/include ${PCSX2_ROOT}/3rdparty/imgui/include
  ${PCSX2_ROOT}/3rdparty/imgui/src ${PCSX2_ROOT}/3rdparty/vulkan/include
  ${PCSX2_ROOT}/3rdparty/stb/include
  ${PCSX2_ROOT}/3rdparty/xbyak ${PCSX2_ROOT}/3rdparty/include
  ${PCSX2_ROOT}/3rdparty/cpuinfo/include ${PCSX2_ROOT}/3rdparty/ccc/src
  ${PCSX2_ROOT}/ps4/compat/include ${PCSX2_ROOT}/3rdparty/simpleini/include
  ${PCSX2_ROOT}/3rdparty/rcheevos/include ${PCSX2_ROOT}/3rdparty/soundtouch/soundtouch
  ${PCSX2_ROOT}/3rdparty/freesurround/include ${PCSX2_ROOT}/ps4/deps/prefix/include
  ${shaderc_headers_SOURCE_DIR}/libshaderc/include ${glslang_SOURCE_DIR})
target_compile_definitions(vm_boot PRIVATE PCSX2_PS4=1 PCSX2_PS4_VM_BOOT=1 PS4=1 USE_GLES2=1 BOREALIS_USE_OPENGL=1 $<$<BOOL:${PCSX2_VM_JIT}>:PCSX2_PS4_VM_BOOT_JIT=1> $<$<BOOL:${PCSX2_VM_FASTMEM}>:PCSX2_PS4_VM_BOOT_FASTMEM=1> $<$<BOOL:${PCSX2_VM_MTVU}>:PCSX2_PS4_VM_BOOT_MTVU=1> ENABLE_VULKAN=1 __POSIX__=1 _M_X86=1
  OVERRIDE_HOST_PAGE_SIZE=0x4000 OVERRIDE_HOST_CACHE_LINE_SIZE=64)
target_compile_options(vm_boot PRIVATE -march=btver2 -msse4.1 -mno-avx2 -mno-bmi2 -ffunction-sections -fdata-sections -fvisibility=hidden
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include compare>" "SHELL:-include strings.h")
target_link_options(vm_boot PRIVATE --gc-sections --error-limit=0)
# Forja SX2: the GitHub Actions run number, shown in the library and the log so a tester can tell which build is installed.
set(FORJA_BUILD_NUMBER "$ENV{GITHUB_RUN_NUMBER}")
if(NOT FORJA_BUILD_NUMBER MATCHES "^[0-9]+$")
  set(FORJA_BUILD_NUMBER 0)
endif()
# Forja SX2: the public version (VERSION at the repository's root, "1.01"), shown in the library as "Versión 1.01 (38)". The
# stable releases are named after it (Forja SX2 v1.00); the run number still tells the builds of one version apart.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" FORJA_VERSION LIMIT_COUNT 1)
string(STRIP "${FORJA_VERSION}" FORJA_VERSION)
if(NOT FORJA_VERSION MATCHES "^([0-9]+)\\.([0-9][0-9])$")
  message(FATAL_ERROR "VERSION must look like 1.01, not '${FORJA_VERSION}'")
endif()
set(FORJA_VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX REPLACE "^0([0-9])$" "\\1" FORJA_VERSION_MINOR "${CMAKE_MATCH_2}")
message(STATUS "Forja SX2 version ${FORJA_VERSION_MAJOR}.${FORJA_VERSION_MINOR} build ${FORJA_BUILD_NUMBER}")
set_source_files_properties(main.cpp GameMenu.cpp PatchDownload.cpp PROPERTIES COMPILE_DEFINITIONS
  "FORJA_BUILD_NUMBER=${FORJA_BUILD_NUMBER};FORJA_VERSION_MAJOR=${FORJA_VERSION_MAJOR};FORJA_VERSION_MINOR=${FORJA_VERSION_MINOR}")
ps4_link_mesa(vm_boot)
target_link_libraries(vm_boot PRIVATE glslang glslang-default-resource-limits ryml ps4_audio_resampler ps4_chd
  png jpeg webp z ${PCSX2_ROOT}/ps4/deps/prefix/lib/libzstd.a ${PCSX2_ROOT}/ps4/deps/prefix/lib/liblz4.a
  SceNet SceNetCtl SceHttp SceSsl
  SceAudioOut SceSysmodule SceSystemService SceUserService SceVideoOut ScePad pthread)
ps4_add_eboot(vm_boot)
