#!/usr/bin/env bash
# Generates deterministic audio fixtures for manual and integration tests.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v ffmpeg >/dev/null || { echo "error: ffmpeg not installed" >&2; exit 1; }
mkdir -p testenv/audio

gen_wav() { # <duration_s> <outfile>
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=$1" \
           -ar 8000 -ac 1 -sample_fmt s16 "$2"
}

gen_wav 1  testenv/audio/hello_1s.wav
gen_wav 3  testenv/audio/hello.wav
gen_wav 30 testenv/audio/hello_30s.wav

echo "fixtures written to testenv/audio/"
