#pragma once
#include <cstddef>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
inline size_t heap_caps_get_free_size(int) { return 100000; }
inline size_t heap_caps_get_largest_free_block(int) { return 80000; }
