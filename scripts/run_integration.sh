#!/usr/bin/env bash
# Integration test harness for pjcppagent.
# Preflight -> build -> gen_fixtures -> pytest
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== Preflight ==="
command -v sipp  >/dev/null 2>&1 || { echo "error: sipp not installed"            >&2; exit 1; }
command -v ffmpeg>/dev/null 2>&1 || { echo "error: ffmpeg not installed"          >&2; exit 1; }
command -v python3>/dev/null 2>&1 || { echo "error: python3 not installed"        >&2; exit 1; }
sipp -v 2>&1 | head -3 || true
ffmpeg -version 2>&1 | head -1
echo "OK"

echo "=== Build ==="
make 2>&1
echo "OK"

echo "=== Audio fixtures ==="
./scripts/gen_fixtures.sh
echo "OK"

echo "=== Python venv ==="
VENV="tests/integration/.python-venv"
if [ ! -d "$VENV" ]; then
    python3 -m venv "$VENV"
fi
source "$VENV/bin/activate"
pip install -q -r tests/integration/requirements.txt
echo "OK"

echo "=== pytest ==="
python3 -m pytest tests/integration -v --tb=short -x "$@"
