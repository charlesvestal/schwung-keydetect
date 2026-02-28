#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="keydetect-builder"

# Auto-detect: use Docker if not already in container and CROSS_PREFIX not set
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Building via Docker ==="

    # Build Docker image if needed (includes FFTW3 + libkeyfinder)
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time — downloads FFTW3 + libkeyfinder)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi

    # Run build inside container
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    exit 0
fi

# === Actual build (inside Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building KeyDetect Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create directories
mkdir -p build
mkdir -p dist/keydetect

# Compile C++ wrapper (links against libkeyfinder headers)
echo "Compiling keyfinder wrapper..."
${CROSS_PREFIX}g++ -Ofast -fPIC -c \
    -march=armv8-a -mtune=cortex-a72 \
    -std=c++11 \
    -DNDEBUG \
    -I/opt/arm64/include \
    src/dsp/keyfinder_wrapper.cpp \
    -o build/keyfinder_wrapper.o

# Compile C plugin
echo "Compiling keydetect plugin..."
${CROSS_PREFIX}gcc -Ofast -fPIC -c \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/keydetect.c \
    -o build/keydetect.o \
    -Isrc/dsp

# Link everything into shared library
# Use g++ for linking since we have C++ objects
echo "Linking keydetect.so..."
${CROSS_PREFIX}g++ -shared \
    -march=armv8-a -mtune=cortex-a72 \
    build/keydetect.o \
    build/keyfinder_wrapper.o \
    -L/opt/arm64/lib \
    -lkeyfinder \
    -lfftw3 \
    -lm -lpthread \
    -static-libstdc++ -static-libgcc \
    -o build/keydetect.so

# Package
echo "Packaging..."
cp src/module.json dist/keydetect/module.json
[ -f src/help.json ] && cp src/help.json dist/keydetect/help.json
cp build/keydetect.so dist/keydetect/keydetect.so
chmod +x dist/keydetect/keydetect.so

# Create tarball for GitHub release
cd dist
tar -czvf keydetect-module.tar.gz keydetect/
cd ..

echo "=== Build Complete ==="
echo "Output: dist/keydetect/"
echo "Tarball: dist/keydetect-module.tar.gz"
