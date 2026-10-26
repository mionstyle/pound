#ifndef POUND_LOADER_SHA256_H
#define POUND_LOADER_SHA256_H

#include <stddef.h>
#include <stdint.h>

/// One-shot SHA-256 (FIPS 180-4).
void sha256(const uint8_t *data, size_t size, uint8_t out_digest[32]);

#endif // POUND_LOADER_SHA256_H
