#pragma once
#include <stdint.h>
#include <stddef.h>

uint32_t crc32_compute(const void *data, size_t len);
