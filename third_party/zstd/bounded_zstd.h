#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The frame's declared content size, or (size_t)-1 when it declares none or
// the header is invalid.
size_t spool_zstd_content_size(const void *source, size_t sourceLength);
// Decodes one zstd frame into at most `capacity` bytes. Returns 0 and sets
// `written` on success, -1 on corrupt, truncated or oversized input.
int spool_zstd_decompress(const void *source, size_t sourceLength, void *destination, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
