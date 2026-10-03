#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$root/tests/audio" -B /tmp/waveshare-audio-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/waveshare-audio-tests -j 4
ctest --test-dir /tmp/waveshare-audio-tests --output-on-failure
