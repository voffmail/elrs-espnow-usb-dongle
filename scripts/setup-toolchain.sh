#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="$ROOT/.toolchain"
VENV="$TC/venv"
PY="$VENV/bin/python"
PIO="$VENV/bin/pio"

mkdir -p "$TC/platformio" "$TC/cache"

if [ ! -x "$PY" ]; then
    if ! command -v python3 >/dev/null 2>&1; then
        echo "python3 is required to create $VENV" >&2
        exit 1
    fi
    python3 -m venv "$VENV"
fi

# System PlatformIO can be too old for the installed Python. Always use the
# project-local venv so builds are reproducible and ~/.platformio is untouched.
if [ ! -x "$PIO" ] || ! "$PIO" --version 2>/dev/null | grep -qE 'version 6\.'; then
    "$PY" -m pip install --upgrade pip >/dev/null
    "$PY" -m pip install "platformio==6.2.0"
fi

if ! "$PIO" --version >/dev/null 2>&1; then
    echo "failed to install a working PlatformIO into $VENV" >&2
    exit 1
fi

cat > "$TC/env.sh" <<EOF
export PLATFORMIO_CORE_DIR="$TC/platformio"
export PLATFORMIO_CACHE_DIR="$TC/cache"
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=false
export PATH="$VENV/bin:\$PATH"
EOF

echo "toolchain: $TC"
echo "platformio: $PIO ($("$PIO" --version 2>/dev/null | tail -1))"
echo "run: source .toolchain/env.sh"
echo "firmware: ./tools/pio run -d firmware -e esp32-c3"
