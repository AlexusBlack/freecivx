#!/bin/bash

# Freeciv Emscripten/WASM Build Script
#
# Usage:
#   ./prepare_freeciv_emscripten.sh [OPTIONS]
#
# Options:
#   --debug         Build with debug symbols and source maps
#   --no-wasm       Build traditional freeciv-web server (not WASM)
#   --clean         Clean build directory before building
#   --help          Show this help message

set -e

# Set up the working directory
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
cd "${DIR}"

# Source version information
if [ -f ./version.txt ]; then
  . ./version.txt
fi

# Default settings
BUILD_DIR="build"
INSTALL_DIR="${HOME}/freeciv"
NUM_CORES=$(nproc)
DEBUG_BUILD=false
WASM_SERVER=true
CLEAN_BUILD=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --debug)
      DEBUG_BUILD=true
      shift
      ;;
    --no-wasm)
      WASM_SERVER=false
      shift
      ;;
    --clean)
      CLEAN_BUILD=true
      shift
      ;;
    --help)
      echo "Freeciv Emscripten/WASM Build Script"
      echo ""
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --debug         Build with debug symbols and source maps"
      echo "  --no-wasm       Build traditional freeciv-web server (not WASM)"
      echo "  --clean         Clean build directory before building"
      echo "  --help          Show this help message"
      echo ""
      echo "Environment variables:"
      echo "  BUILD_DIR       Build directory (default: build)"
      echo "  INSTALL_DIR     Installation directory (default: \$HOME/freeciv)"
      echo "  NUM_CORES       Number of parallel jobs (default: nproc)"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      echo "Use --help for usage information"
      exit 1
      ;;
  esac
done

# Clean build directory if requested
if [ "$CLEAN_BUILD" = true ] && [ -d "${BUILD_DIR}" ]; then
  echo "Cleaning build directory..."
  rm -rf "${BUILD_DIR}"
fi

# Create build directory if it doesn't exist
mkdir -p "${BUILD_DIR}"

# Determine build configuration
if [ "$WASM_SERVER" = true ]; then
  echo "Building WASM server..."
  SERVER_OPT="-Dserver=disabled"
  WASM_OPT="-Dwasm-server=true"

  if [ "$DEBUG_BUILD" = true ]; then
    echo "Debug build enabled"
    WASM_DEBUG_OPT="-Dwasm-server-debug=true"
    OPT_LEVEL="-Doptimization=0"
    LTO_OPT="-Db_lto=false"
  else
    WASM_DEBUG_OPT=""
    OPT_LEVEL="-Doptimization=3"
    LTO_OPT="-Db_lto=true"
  fi
else
  echo "Building traditional freeciv-web server..."
  SERVER_OPT="-Dserver=freeciv-web"
  WASM_OPT=""
  WASM_DEBUG_OPT=""
  OPT_LEVEL="-Doptimization=3"
  LTO_OPT="-Db_lto=true"
fi

# Build process
(
  cd "${BUILD_DIR}" || exit

  # Configure with meson
  meson setup ../freeciv \
    ${SERVER_OPT} \
    ${WASM_OPT} \
    ${WASM_DEBUG_OPT} \
    -Dclients=[] \
    -Dfcmp=[] \
    -Djson-protocol=true \
    -Dnls=false \
    -Daudio=false \
    -Druledit=false \
    -Dreadline=false \
    -Ddefault_library=static \
    -Dprefix="${INSTALL_DIR}" \
    ${OPT_LEVEL} \
    ${LTO_OPT} \
    --cross-file ../emscripten_crossfile.ini \
    --wipe

  # Build using all available CPU cores
  ninja -j "${NUM_CORES}"
)

# Finish up
echo ""
echo "========================================"
echo "Build complete!"
echo "========================================"
echo "Output located in: ${BUILD_DIR}"

if [ "$WASM_SERVER" = true ]; then
  echo ""
  echo "WASM server files:"
  ls -lh "${BUILD_DIR}"/freeciv-server-wasm.* 2>/dev/null || echo "  (not found - check build output)"
  echo ""
  echo "To use the WASM server:"
  echo "  1. Copy freeciv-server-wasm.js and .wasm to your web server"
  echo "  2. Include freeciv_server.js bridge from freeciv/server/wasm/"
  echo "  3. See freeciv/server/wasm/example.html for integration example"
fi
