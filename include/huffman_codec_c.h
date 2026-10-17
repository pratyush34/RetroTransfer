#ifndef RETROTRANSFER_HUFFMAN_CODEC_C_H
#define RETROTRANSFER_HUFFMAN_CODEC_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int rt_huffman_compress(const uint8_t *input,
                        size_t input_size,
                        uint8_t **output,
                        size_t *output_size,
                        char *error,
                        size_t error_capacity);

int rt_huffman_decompress(const uint8_t *input,
                          size_t input_size,
                          uint64_t original_size,
                          uint8_t **output,
                          size_t *output_size,
                          char *error,
                          size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
