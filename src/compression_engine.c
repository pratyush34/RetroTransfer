#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "compression_engine_c.h"

#include "fingerprint_c.h"
#include "huffman_codec_c.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define rt_seek _fseeki64
#define rt_tell _ftelli64
#else
#include <sys/types.h>
#define rt_seek fseeko
#define rt_tell ftello
#endif

#define RT_HEADER_SIZE UINT64_C(8)
#define RT_MAX_CHUNK_SIZE_BYTES (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))

typedef struct {
    size_t *heads;
    size_t *next;
    size_t bucket_count;
} ChunkIndex;

static void set_error(char *error, size_t capacity, const char *message) {
    if (error != NULL && capacity != 0) {
        (void)snprintf(error, capacity, "%s", message);
    }
}

static void set_error_format(char *error,
                             size_t capacity,
                             const char *format,
                             ...) {
    if (error == NULL || capacity == 0) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static size_t fingerprint_bucket(const char *fingerprint, size_t bucket_count) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *character = (const unsigned char *)fingerprint;
         *character != '\0'; ++character) {
        hash ^= *character;
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)(hash % bucket_count);
}

static int index_initialize(ChunkIndex *index, size_t bucket_count, size_t entry_count) {
    index->heads = NULL;
    index->next = NULL;
    index->bucket_count = bucket_count;
    if (bucket_count != 0) {
        if (bucket_count > SIZE_MAX / sizeof(*index->heads)) {
            return 0;
        }
        index->heads = (size_t *)malloc(bucket_count * sizeof(*index->heads));
        if (index->heads == NULL) {
            return 0;
        }
        for (size_t i = 0; i < bucket_count; ++i) {
            index->heads[i] = SIZE_MAX;
        }
    }
    if (entry_count != 0) {
        if (entry_count > SIZE_MAX / sizeof(*index->next)) {
            free(index->heads);
            index->heads = NULL;
            return 0;
        }
        index->next = (size_t *)malloc(entry_count * sizeof(*index->next));
        if (index->next == NULL) {
            free(index->heads);
            index->heads = NULL;
            return 0;
        }
    }
    return 1;
}

static void index_destroy(ChunkIndex *index) {
    free(index->heads);
    free(index->next);
    memset(index, 0, sizeof(*index));
}

static void index_insert(ChunkIndex *index,
                         const RtCompressionChunk *chunks,
                         size_t chunk_index) {
    const size_t bucket =
        fingerprint_bucket(chunks[chunk_index].fingerprint, index->bucket_count);
    index->next[chunk_index] = index->heads[bucket];
    index->heads[bucket] = chunk_index;
}

static int read_chunk(const RtCompressionChunk *chunk,
                      uint8_t **bytes,
                      size_t *size) {
    *bytes = NULL;
    *size = 0;
    if (chunk->length > SIZE_MAX || chunk->length > RT_MAX_CHUNK_SIZE_BYTES ||
        chunk->file_path == NULL) {
        return 0;
    }
    FILE *file = fopen(chunk->file_path, "rb");
    if (file == NULL) {
        return 0;
    }
#ifdef _WIN32
    const int seek_result = rt_seek(file, (__int64)chunk->offset, SEEK_SET);
#else
    if (chunk->offset > (uint64_t)INT64_MAX) {
        fclose(file);
        return 0;
    }
    const int seek_result = rt_seek(file, (off_t)chunk->offset, SEEK_SET);
#endif
    if (seek_result != 0) {
        fclose(file);
        return 0;
    }
    const size_t length = (size_t)chunk->length;
    uint8_t *buffer = length == 0 ? NULL : (uint8_t *)malloc(length);
    if (length != 0 && buffer == NULL) {
        fclose(file);
        return 0;
    }
    const size_t read_size = length == 0 ? 0 : fread(buffer, 1, length, file);
    const int read_ok = read_size == length && !ferror(file);
    fclose(file);
    if (!read_ok) {
        free(buffer);
        return 0;
    }
    *bytes = buffer;
    *size = length;
    return 1;
}

static int find_matching_chunk(const RtCompressionChunk *candidate,
                               const uint8_t *candidate_bytes,
                               const RtCompressionChunk *indexed_chunks,
                               const ChunkIndex *index,
                               char *error,
                               size_t error_capacity) {
    if (index->bucket_count == 0) {
        return 0;
    }
    const size_t bucket =
        fingerprint_bucket(candidate->fingerprint, index->bucket_count);
    for (size_t current = index->heads[bucket]; current != SIZE_MAX;
         current = index->next[current]) {
        const RtCompressionChunk *indexed = &indexed_chunks[current];
        if (indexed->length != candidate->length ||
            strcmp(indexed->fingerprint, candidate->fingerprint) != 0) {
            continue;
        }
        uint8_t *indexed_bytes = NULL;
        size_t indexed_size = 0;
        if (!read_chunk(indexed, &indexed_bytes, &indexed_size)) {
            set_error_format(error, error_capacity,
                             "Could not reread baseline chunk: %s",
                             indexed->file_path != NULL ? indexed->file_path : "");
            return -1;
        }
        const int matches = indexed_size == candidate->length &&
                            (indexed_size == 0 ||
                             memcmp(candidate_bytes, indexed_bytes, indexed_size) == 0);
        free(indexed_bytes);
        if (matches) {
            return 1;
        }
    }
    return 0;
}

static int checked_add_u64(uint64_t *target, uint64_t value) {
    if (UINT64_MAX - *target < value) {
        return 0;
    }
    *target += value;
    return 1;
}

static int write_payload(FILE *output,
                         const uint8_t *bytes,
                         size_t size,
                         uint64_t *offset) {
    const int64_t position = (int64_t)rt_tell(output);
    if (position < 0) {
        return 0;
    }
    if (size != 0 && fwrite(bytes, 1, size, output) != size) {
        return 0;
    }
    *offset = (uint64_t)position;
    return 1;
}

void rt_free_compression_result(RtCompressionResult *result) {
    if (result == NULL) {
        return;
    }
    free(result->chunks);
    memset(result, 0, sizeof(*result));
}

int rt_compress_missing_chunks(const RtCompressionChunk *current,
                               size_t current_count,
                               const RtCompressionChunk *baseline,
                               size_t baseline_count,
                               const char *payload_path,
                               RtCompressionResult *result,
                               char *error,
                               size_t error_capacity) {
    ChunkIndex baseline_index = {0};
    ChunkIndex payload_index = {0};
    FILE *output = NULL;
    int success = 0;

    if (result == NULL || payload_path == NULL ||
        (current == NULL && current_count != 0) ||
        (baseline == NULL && baseline_count != 0)) {
        set_error(error, error_capacity, "Invalid compression engine arguments.");
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->metrics.bundle_header_bytes = RT_HEADER_SIZE;
    if (error != NULL && error_capacity != 0) {
        error[0] = '\0';
    }
    for (size_t i = 0; i < current_count; ++i) {
        if (current[i].fingerprint == NULL || current[i].file_path == NULL ||
            current[i].relative_path == NULL) {
            set_error(error, error_capacity, "Invalid current chunk metadata.");
            goto cleanup;
        }
    }
    for (size_t i = 0; i < baseline_count; ++i) {
        if (baseline[i].fingerprint == NULL || baseline[i].file_path == NULL ||
            baseline[i].relative_path == NULL) {
            set_error(error, error_capacity, "Invalid baseline chunk metadata.");
            goto cleanup;
        }
    }

    size_t bucket_count = 0;
    if (current_count > (SIZE_MAX - 1) / 2 ||
        baseline_count > (SIZE_MAX - 1) / 2) {
        set_error(error, error_capacity, "Too many chunks to index.");
        goto cleanup;
    }
    const size_t max_count = current_count > baseline_count ? current_count : baseline_count;
    if (max_count != 0) {
        bucket_count = max_count * 2 + 1;
    }
    if (!index_initialize(&baseline_index, bucket_count, baseline_count) ||
        !index_initialize(&payload_index, bucket_count, current_count)) {
        set_error(error, error_capacity, "Could not allocate chunk indexes.");
        goto cleanup;
    }
    for (size_t i = 0; i < baseline_count; ++i) {
        index_insert(&baseline_index, baseline, i);
    }

    output = fopen(payload_path, "wb");
    if (output == NULL) {
        set_error_format(error, error_capacity,
                         "Could not create compressed payload: %s", payload_path);
        goto cleanup;
    }
    static const uint8_t header[RT_HEADER_SIZE] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
    if (fwrite(header, 1, sizeof(header), output) != sizeof(header)) {
        set_error_format(error, error_capacity,
                         "Could not write compressed payload header: %s", payload_path);
        goto cleanup;
    }
    result->chunks = current_count == 0
                         ? NULL
                         : (RtCompressedChunk *)calloc(current_count,
                                                       sizeof(*result->chunks));
    if (current_count != 0 && result->chunks == NULL) {
        set_error(error, error_capacity, "Could not allocate compressed chunk results.");
        goto cleanup;
    }

    for (size_t i = 0; i < current_count; ++i) {
        const RtCompressionChunk *chunk = &current[i];
        uint8_t *raw = NULL;
        size_t raw_size = 0;
        if (!checked_add_u64(&result->metrics.total_source_bytes, chunk->length)) {
            set_error(error, error_capacity, "Compression metrics overflow.");
            goto cleanup;
        }
        ++result->metrics.total_chunks;
        if (!read_chunk(chunk, &raw, &raw_size)) {
            set_error_format(error, error_capacity,
                             "Could not reread source chunk: %s", chunk->file_path);
            goto cleanup;
        }

        char fingerprint[33];
        rt_compute_fingerprint_hex(raw, raw_size, 42, fingerprint);
        if (strcmp(fingerprint, chunk->fingerprint) != 0 ||
            rt_compute_crc32(raw, raw_size) != chunk->crc32) {
            set_error_format(error, error_capacity,
                             "Source chunk changed after scanning: %s chunk %u",
                             chunk->relative_path, chunk->sequence_number);
            free(raw);
            goto cleanup;
        }

        const int found_in_baseline =
            find_matching_chunk(chunk, raw, baseline, &baseline_index,
                                error, error_capacity);
        if (found_in_baseline < 0) {
            free(raw);
            goto cleanup;
        }
        if (found_in_baseline) {
            ++result->metrics.reusable_chunks;
            if (!checked_add_u64(&result->metrics.reusable_bytes, chunk->length)) {
                free(raw);
                set_error(error, error_capacity, "Compression metrics overflow.");
                goto cleanup;
            }
            free(raw);
            continue;
        }

        ++result->metrics.missing_chunks;
        if (!checked_add_u64(&result->metrics.selected_input_bytes, chunk->length)) {
            free(raw);
            set_error(error, error_capacity, "Compression metrics overflow.");
            goto cleanup;
        }
        const int found_in_payload =
            find_matching_chunk(chunk, raw, current, &payload_index,
                                error, error_capacity);
        if (found_in_payload < 0) {
            free(raw);
            goto cleanup;
        }
        if (found_in_payload) {
            ++result->metrics.duplicate_chunks;
            if (!checked_add_u64(&result->metrics.duplicate_bytes_avoided,
                                 chunk->length)) {
                free(raw);
                set_error(error, error_capacity, "Compression metrics overflow.");
                goto cleanup;
            }
            free(raw);
            continue;
        }

        uint8_t *encoded = NULL;
        size_t encoded_size = 0;
        char huffman_error[256] = {0};
        if (!rt_huffman_compress(raw, raw_size, &encoded, &encoded_size,
                                 huffman_error, sizeof(huffman_error))) {
            set_error_format(error, error_capacity,
                             "Could not Huffman-compress %s chunk %u: %s",
                             chunk->relative_path, chunk->sequence_number,
                             huffman_error);
            free(raw);
            goto cleanup;
        }

        const int use_huffman = encoded_size < raw_size;
        const uint8_t *payload = use_huffman ? encoded : raw;
        const size_t payload_size = use_huffman ? encoded_size : raw_size;
        RtCompressedChunk *compressed = &result->chunks[result->chunk_count];
        compressed->current_index = i;
        compressed->uses_huffman = use_huffman;
        if (!write_payload(output, payload, payload_size,
                           &compressed->payload_offset)) {
            set_error_format(error, error_capacity,
                             "Failed writing compressed payload: %s", payload_path);
            free(encoded);
            free(raw);
            goto cleanup;
        }
        compressed->stored_size = payload_size;
        ++result->chunk_count;
        index_insert(&payload_index, current, i);

        if (!checked_add_u64(&result->metrics.unique_input_bytes, chunk->length) ||
            !checked_add_u64(&result->metrics.payload_bytes, payload_size)) {
            free(encoded);
            free(raw);
            set_error(error, error_capacity, "Compression metrics overflow.");
            goto cleanup;
        }
        if (use_huffman) {
            ++result->metrics.huffman_chunks;
        } else {
            ++result->metrics.stored_raw_chunks;
        }
        free(encoded);
        free(raw);
    }

    if (fclose(output) != 0) {
        output = NULL;
        set_error_format(error, error_capacity,
                         "Failed closing compressed payload: %s", payload_path);
        goto cleanup;
    }
    output = NULL;
    result->metrics.compression_bytes_saved =
        result->metrics.unique_input_bytes > result->metrics.payload_bytes
            ? result->metrics.unique_input_bytes - result->metrics.payload_bytes
            : 0;
    const uint64_t bundle_bytes =
        result->metrics.payload_bytes + result->metrics.bundle_header_bytes;
    result->metrics.total_bytes_avoided =
        result->metrics.total_source_bytes > bundle_bytes
            ? result->metrics.total_source_bytes - bundle_bytes
            : 0;
    success = 1;

cleanup:
    if (output != NULL) {
        (void)fclose(output);
    }
    index_destroy(&baseline_index);
    index_destroy(&payload_index);
    if (!success) {
        rt_free_compression_result(result);
    }
    return success;
}
