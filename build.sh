#!/bin/bash

# Exit on error
set -e

# Print commands
set -x

# Create build directory if it doesn't exist
mkdir -p build

# Configure with CMake
# Find vcpkg toolchain file
if [ -d "$HOME/vcpkg" ]; then
    VCPKG_TOOLCHAIN="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
elif [ -n "$VCPKG_ROOT" ]; then
    VCPKG_TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
else
    echo "Error: vcpkg not found. Please install vcpkg or set VCPKG_ROOT environment variable."
    exit 1
fi

# Configure with CMake using vcpkg
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN"

# Build
cmake --build build --config Release

# Print success message
echo "Build completed successfully!"
echo "You can run the application with: ./build/bin/kubera"
