# PowerShell script to build the project

# Exit on error
$ErrorActionPreference = "Stop"

# Create build directory if it doesn't exist
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Find vcpkg toolchain file
Write-Host "Finding vcpkg toolchain file..."
$vcpkgToolchain = $null

if (Test-Path "$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake") {
    $vcpkgToolchain = "$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
} elseif ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    $vcpkgToolchain = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
} else {
    Write-Host "Error: vcpkg not found. Please install vcpkg or set VCPKG_ROOT environment variable." -ForegroundColor Red
    exit 1
}

# Configure with CMake using vcpkg
Write-Host "Configuring with CMake..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$vcpkgToolchain"

# Build
Write-Host "Building..."
cmake --build build --config Release

# Print success message
Write-Host "Build completed successfully!"
Write-Host "You can run the application with: .\build\bin\Release\kubera.exe"
