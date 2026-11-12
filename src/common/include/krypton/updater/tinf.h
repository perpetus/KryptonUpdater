#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define TINF_OK           0
#define TINF_DATA_ERROR  (-3)
#define TINF_BUF_ERROR   (-5)

/* Decompress raw DEFLATE data (RFC 1951) */
int tinf_uncompress(void* dest, unsigned int* destLen,
                    const void* source, unsigned int sourceLen);

#ifdef __cplusplus
}
#endif
