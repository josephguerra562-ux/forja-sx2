# (AI-assisted) Build current zlib and libpng for the OpenOrbis PS4 target.
[CmdletBinding()]
param(
	[string]$ToolchainRoot = $env:OO_PS4_TOOLCHAIN,
	[string]$Prefix = (Join-Path $PSScriptRoot "prefix"),
	[int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"

if (-not $ToolchainRoot -or -not (Test-Path (Join-Path $ToolchainRoot "include/orbis/Pad.h"))) {
	throw "Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 Toolchain root."
}

$toolchainFile = Join-Path $PSScriptRoot "../cmake/OpenOrbisToolchain.cmake"
$cacheDir = Join-Path $PSScriptRoot "downloads"
$sourceDir = Join-Path $PSScriptRoot "src"
$buildDir = Join-Path $PSScriptRoot "build"
$prefixPath = [IO.Path]::GetFullPath($Prefix)
New-Item -ItemType Directory -Force -Path $cacheDir, $sourceDir, $buildDir, $prefixPath | Out-Null

$packages = @(
	@{
		Name = "zlib-1.3.2"
		Url = "https://zlib.net/fossils/zlib-1.3.2.tar.gz"
		Sha256 = "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
	},
	@{
		Name = "libpng-1.6.59"
		# Pinned upstream tag v1.6.59 (commit cd952f49f95bb27154ae77dbb103032d95f6e580).
		# SourceForge's canonical tarball is also hash-verified on libpng.org.
		Url = "https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.59"
		Sha256 = "2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5"
		ArchiveName = "libpng-1.6.59-upstream.tar.gz"
	},
	@{
		Name = "libjpeg-turbo-3.2.0"
		# Pinned upstream release tag 3.2.0 (commit c85e6b905bf237038faa936dab160ebfc5da0344).
		Url = "https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/refs/tags/3.2.0"
		Sha256 = "980dd81f425082aa6d7c9e47fef27554ce7a9ffc8e2f6e863b97d263c5c50858"
	},
	@{
		Name = "zstd-1.5.7"
		# Pinned upstream release tag v1.5.7 (commit ac66b19e6bd6b83238bf008eecc1298105298532).
		Url = "https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7"
		Sha256 = "37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3"
	},
	@{
		Name = "lz4-1.10.0"
		# Pinned upstream release tag v1.10.0 (commit ebb370ca83af193212df4dcbadcc5d87bc0de2f0).
		Url = "https://codeload.github.com/lz4/lz4/tar.gz/refs/tags/v1.10.0"
		Sha256 = "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b"
	},
	@{
		Name = "libwebp-1.6.0"
		# Pinned upstream release tag v1.6.0 (commit b7e29b9d75bd31422b00c2a446d49d7af06c328d).
		Url = "https://codeload.github.com/webmproject/libwebp/tar.gz/refs/tags/v1.6.0"
		Sha256 = "93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136"
	}
)

foreach ($package in $packages) {
	$archiveName = if ($package.ArchiveName) { $package.ArchiveName } else { "$($package.Name).tar.gz" }
	$archive = Join-Path $cacheDir $archiveName
	$source = Join-Path $sourceDir $package.Name
	if (-not (Test-Path $archive)) {
		Write-Host "Downloading $($package.Name)"
		Invoke-WebRequest -Uri $package.Url -OutFile $archive
	}
	$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
	if ($actualHash -ne $package.Sha256) {
		throw "SHA-256 mismatch for $($package.Name): expected $($package.Sha256), got $actualHash"
	}
	if (-not (Test-Path $source)) {
		New-Item -ItemType Directory -Path $source | Out-Null
		& tar -xzf $archive --strip-components=1 -C $source
		if ($LASTEXITCODE -ne 0) { throw "Could not extract $($package.Name)" }
	}
}

$env:OO_PS4_TOOLCHAIN = $ToolchainRoot
$env:PS4_DEPS = $prefixPath

function Invoke-CMake([string[]]$Arguments) {
	& cmake @Arguments
	if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }
}

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "zlib-1.3.2"),
	"-B", (Join-Path $buildDir "zlib"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DZLIB_BUILD_SHARED=OFF",
	"-DZLIB_BUILD_STATIC=ON",
	"-DZLIB_BUILD_TESTING=OFF"
)
Invoke-CMake @("--build", (Join-Path $buildDir "zlib"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "zlib"))

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "libjpeg-turbo-3.2.0"),
	"-B", (Join-Path $buildDir "libjpeg-turbo"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DENABLE_SHARED=OFF",
	"-DENABLE_STATIC=ON",
	"-DWITH_SIMD=OFF",
	"-DWITH_TURBOJPEG=OFF",
	"-DWITH_TESTS=OFF",
	"-DWITH_TOOLS=OFF"
)
Invoke-CMake @("--build", (Join-Path $buildDir "libjpeg-turbo"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "libjpeg-turbo"))

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "zstd-1.5.7/build/cmake"),
	"-B", (Join-Path $buildDir "zstd"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DZSTD_BUILD_PROGRAMS=OFF",
	# (AI-assisted) FSELF cannot resolve optional weak trace hooks as imports.
	"-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -DZSTD_TRACE=0",
	"-DZSTD_BUILD_TESTS=OFF",
	"-DZSTD_BUILD_SHARED=OFF",
	"-DZSTD_BUILD_STATIC=ON"
)
Invoke-CMake @("--build", (Join-Path $buildDir "zstd"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "zstd"))

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "lz4-1.10.0/build/cmake"),
	"-B", (Join-Path $buildDir "lz4"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DBUILD_SHARED_LIBS=OFF",
	"-DLZ4_BUILD_CLI=OFF",
	"-DLZ4_BUILD_LEGACY=OFF"
)
Invoke-CMake @("--build", (Join-Path $buildDir "lz4"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "lz4"))

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "libwebp-1.6.0"),
	"-B", (Join-Path $buildDir "libwebp"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG -msse2",
	"-DBUILD_SHARED_LIBS=OFF",
	"-DWEBP_BUILD_ANIM_UTILS=OFF",
	"-DWEBP_BUILD_CWEBP=OFF",
	"-DWEBP_BUILD_DWEBP=OFF",
	"-DWEBP_BUILD_GIF2WEBP=OFF",
	"-DWEBP_BUILD_IMG2WEBP=OFF",
	"-DWEBP_BUILD_VWEBP=OFF",
	"-DWEBP_BUILD_WEBPMUX=OFF",
	"-DWEBP_BUILD_WEBPINFO=OFF",
	"-DWEBP_BUILD_EXTRAS=OFF",
	"-DWEBP_ENABLE_SIMD=OFF"
)
Invoke-CMake @("--build", (Join-Path $buildDir "libwebp"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "libwebp"))

Invoke-CMake @(
	"-S", (Join-Path $sourceDir "libpng-1.6.59"),
	"-B", (Join-Path $buildDir "libpng"),
	"-G", "Ninja",
	"--toolchain", $toolchainFile,
	"-DCMAKE_BUILD_TYPE=Release",
	"-DCMAKE_INSTALL_PREFIX=$prefixPath",
	"-DZLIB_ROOT=$prefixPath",
	"-DZLIB_USE_STATIC_LIBS=ON",
	"-DPNG_SHARED=OFF",
	"-DPNG_STATIC=ON",
	"-DPNG_TESTS=OFF",
	"-DPNG_TOOLS=OFF"
)
Invoke-CMake @("--build", (Join-Path $buildDir "libpng"), "--parallel", "$Jobs")
Invoke-CMake @("--install", (Join-Path $buildDir "libpng"))

Write-Host "PS4 prerequisites installed to $prefixPath"
Write-Host "Configure PCSX2 with PS4_DEPS=$prefixPath"
