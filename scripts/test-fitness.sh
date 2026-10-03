#!/usr/bin/env bash
set -eu
task_dir=$(mktemp -d)
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I firmware/main tests/fitness_model_test.cpp firmware/main/fitness_model.cpp -o "$task_dir/fitness_test"
ASAN_OPTIONS=detect_leaks=1 "$task_dir/fitness_test"
