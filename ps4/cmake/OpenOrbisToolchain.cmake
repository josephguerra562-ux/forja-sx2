# (AI-assisted) Cross-compile PCSX2 as a FreeBSD-ABI x86-64 app for PS4.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(OO_PS4_TOOLCHAIN "$ENV{OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis PS4 Toolchain root")
if(NOT OO_PS4_TOOLCHAIN OR NOT EXISTS "${OO_PS4_TOOLCHAIN}/include/orbis/Pad.h")
	message(FATAL_ERROR "Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 Toolchain root.")
endif()

set(PS4_DEPS "$ENV{PS4_DEPS}" CACHE PATH "Prefix containing third-party libraries cross-built for PS4")

set(CMAKE_C_COMPILER clang CACHE FILEPATH "OpenOrbis Clang C compiler")
set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "OpenOrbis Clang C++ compiler")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)


# Keep objects PIC for the eventual PS4 eboot; skip CMake's desktop PIE link probe.
set(POSITION_INDEPENDENT_CODE OFF CACHE BOOL "OpenOrbis uses PIC objects and a custom final link" FORCE)
set(CMAKE_C_FLAGS_INIT "-fPIC -isystem${OO_PS4_TOOLCHAIN}/include")
set(CMAKE_CXX_FLAGS_INIT "-fPIC -isystem${OO_PS4_TOOLCHAIN}/include/c++/v1 -isystem${OO_PS4_TOOLCHAIN}/include")

# The SDK's bundled zlib/libpng versions are too old for PCSX2. Search only the
# separately cross-built dependency prefix; SDK headers/libs are added explicitly.
set(CMAKE_FIND_ROOT_PATH "${PS4_DEPS}")
set(CMAKE_PREFIX_PATH "${PS4_DEPS}" CACHE PATH "PS4 third-party dependency prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
