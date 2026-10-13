#!/usr/bin/env bash
# (AI-assisted) Build only PS4SX2, using its own source staging and build tree.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
stage="${VM_SOURCE_STAGE:-/root/ps4sx2-src}"
build="${VM_BUILD_DIR:-/root/ps4sx2-build}"
jobs="${1:-4}"
mkdir -p "$stage" "$repo/ps4/vm/build"
cd "$repo"
rsync -aR --exclude=.git --exclude=build --exclude='*.log' \
  common pcsx2 3rdparty bin/resources ps4/core ps4/compat ps4/vm \
  ps4/toolchain ps4/runtime ps4/deps/prefix "$stage/"
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PS4_SYSROOT="${PS4_SYSROOT:-/root/dolphinps4-build/sysroot}"
mesa="${PS4_MESA_BUILD:-/root/dolphinps4-build/build/mesa}"
cmake -S "$stage/ps4/vm" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$stage/ps4/toolchain/ps4-love-modern.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DPS4_MALLOC_REPLACE=ON -DPS4_PAID=0x3800000000000011 \
  -DPS4_MESA_BUILD="$mesa" \
  -DPCSX2_VM_JIT="${PCSX2_VM_JIT:-ON}" \
  -DPCSX2_VM_FASTMEM="${PCSX2_VM_FASTMEM:-ON}" \
  -DPCSX2_VM_MTVU="${PCSX2_VM_MTVU:-ON}"
cmake --build "$build" --parallel "$jobs"
cp "$build/vm_boot" "$repo/ps4/vm/build/vm_boot"
cp "$build/vm_boot_eboot/eboot.bin" "$repo/ps4/vm/build/eboot.bin"
cp "$build/vm_boot_eboot/vm_boot.oelf" "$repo/ps4/vm/build/vm_boot.oelf"
