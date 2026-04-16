#!/usr/bin/env bash
# setup_host.sh — SkinsAreSilly build environment setup for Ubuntu 24.04
# Installs all tools needed to cross-compile Windows targets from Linux.
set -euo pipefail

if [[ "$EUID" -ne 0 ]]; then
    echo "Run as root (or with sudo): sudo bash setup_host.sh"
    exit 1
fi

echo "==> Updating package lists..."
apt-get update -qq

echo "==> Installing cross-compilation toolchain and build tools..."
apt-get install -y \
    mingw-w64 \
    cmake \
    build-essential \
    git \
    ninja-build

echo ""
echo "==> Done. Toolchain installed:"
x86_64-w64-mingw32-g++ --version | head -1
cmake --version | head -1
echo ""
echo "Build instructions:"
echo "  mkdir build && cd build"
echo "  cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-windows.cmake -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build . -j\$(nproc)"
echo ""
echo "Output: build/SovereignHook.dll  build/MuseumCurator.exe"
