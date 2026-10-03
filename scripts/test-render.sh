#!/usr/bin/env bash
set -eu
task_dir=$(mktemp -d)
mkdir -p .cache
project_dir=$PWD
cd firmware/components/codex_micro/assets
ld -r -b binary bg_day.bin -o "$task_dir/bg_day.o"
ld -r -b binary bg_night.bin -o "$task_dir/bg_night.o"
cd "$project_dir"
g++ -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer -g -Wl,-z,noexecstack \
    -I tests/stubs -I firmware/components/codex_micro \
    tests/render_codex.cpp firmware/components/codex_micro/gfx.cpp \
    "$task_dir/bg_day.o" "$task_dir/bg_night.o" -o "$task_dir/render_test"
ASAN_OPTIONS=detect_leaks=1 "$task_dir/render_test"
