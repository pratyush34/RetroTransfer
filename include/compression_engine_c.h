#ifndef RETROTRANSFER_COMPRESSION_ENGINE_C_H
#define RETROTRANSFER_COMPRESSION_ENGINE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *file_path;
    const char *relative_path;
    const char *fingerprint;
    uint64_t offset;
    uint64_t length;
    uint32_t sequence_number;
    uint32_t crc32;
} RtCompressionChunk;

typedef struct {
    size_t current_index;
    uint64_t payload_offset;
    uint64_t stored_size;
    int uses_huffman;
} RtCompressedChunk;

typedef struct {
    size_t total_chunks;
    size_t reusable_chunks;
    size_t missing_chunks;
    size_t duplicate_chunks;
    size_t huffman_chunks;
    size_t stored_raw_chunks;
    uint64_t reusable_bytes;
    uint64_t duplicate_bytes_avoided;
    uint64_t total_source_bytes;
    uint64_t selected_input_bytes;
    uint64_t unique_input_bytes;
    uint64_t payload_bytes;
    uint64_t bundle_header_bytes;
    uint64_t compression_bytes_saved;
    uint64_t total_bytes_avoided;
} RtCompressionMetrics;

typedef struct {
    RtCompressedChunk *chunks;
    size_t chunk_count;
    RtCompressionMetrics metrics;
} RtCompressionResult;

int rt_compress_missing_chunks(const RtCompressionChunk *current,
                               size_t current_count,
                               const RtCompressionChunk *baseline,
                               size_t baseline_count,
                               const char *payload_path,
                               RtCompressionResult *result,
                               char *error,
                               size_t error_capacity);

void rt_free_compression_result(RtCompressionResult *result);

#ifdef __cplusplus
}
#endif

#endif
