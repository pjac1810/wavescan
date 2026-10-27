#!/bin/sh
# Host test of WAVESCAN's sound: WAVs in build/render-out/, CPU time, a fuzz.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/render-out
${CC:-cc} -O1 -g -fsanitize=undefined -fno-sanitize-recover -Wall -Isrc -Iplatform -DPOOL= \
  -DRING_PUBLISH\(\)= tests/render.c src/synth.c src/presets.c -lm -o build/render
build/render build/render-out
