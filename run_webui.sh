#!/bin/bash
# CLMA Framework - Web UI Launcher
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
WEB_APP="$SCRIPT_DIR/python_interface/web_app.py"

# Use venv if available, otherwise system python
if [ -d "$VENV_DIR" ]; then
    PYTHON="$VENV_DIR/bin/python3"
else
    PYTHON="python3"
fi

echo "========================================"
echo " CLMA Framework - Web Interface"
echo " Closed-Loop Multi-Agent Reasoning"
echo "========================================"
echo ""

# Rebuild C++ bindings if needed (any CPython version / arch, not only 3.14)
BUILD_DIR="$SCRIPT_DIR/build"
shopt -s nullglob
_bindings=(
    "$BUILD_DIR/python_bindings"/clma_core.cpython-*.so
    "$SCRIPT_DIR/python_interface"/clma_core.cpython-*.so
)
if [ ${#_bindings[@]} -gt 0 ]; then
    echo "[✓] Python bindings found (${_bindings[0]##*/})"
else
    echo "[!] Building Python bindings..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. > /dev/null 2>&1
    make -j4 clma_core > /dev/null 2>&1
    echo "[✓] Build complete"
fi
shopt -u nullglob

echo "[✓] Starting server at http://0.0.0.0:5000"
echo ""
$PYTHON "$WEB_APP"
