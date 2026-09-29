#ifndef TKL_VAD_INTERNAL_H
#define TKL_VAD_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Feeds every complete configured VAD frame contained in a captured PCM
 * buffer. No-op while VAD is stopped/uninitialized. */
void tkl_jieli_vad_feed_capture(const uint8_t *data, size_t size);

#endif
