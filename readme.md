# RetroTransfer

Pratyush & Mahesh
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

### Quick Build & Run (MinGW / UCRT64 + CMake)

```powershell
# Set compiler environment if not already in system PATH:
$env:Path = "D:\mingw64\bin;C:\msys64\ucrt64\bin;C:\Program Files\CMake\bin;$env:Path"

# Configure and build
cmake -B build -G "Ninja"
cmake --build build

# Run project scan and adaptive chunking on current directory
.\build\RetroTransfer.exe .

# Run automated test suite
ctest --test-dir build --output-on-failure
```

The scan is saved as JSON in the `collection` folder by default:
```powershell
.\build\RetroTransfer.exe "any directory"
```

To choose a different JSON filename inside `collection`, provide it as the second argument:
```powershell
.\build\RetroTransfer.exe "any directory" "project-name.json"
```

Each saved JSON file contains a directory tree. Every directory has `name`, `path`,
`directories`, and `files` fields. Every file records its `name`, `path`, `size`,
`type`, and `modificationTime`.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

## 1. Project Overview

**RetroTransfer** is an incremental project-transfer system designed to transfer software repositories and evolving directories over high-latency or constrained network links with minimal redundant data transmission.

RetroTransfer is being developed to reduce retransmission of unchanged content across project revisions. This branch implements directory scanning, fixed and content-defined chunking, chunk fingerprints, metrics, and JSON manifests. Persistent deduplication, compression, socket transfer, and reconstruction are planned stages and are not implemented yet.

### System Pipeline Roadmap

```mermaid
flowchart LR
    A["Project Scanner<br/>(Implemented)"] --> B["Chunking Engine<br/>(Implemented)"]
    B --> C["Fingerprinting<br/>(Implemented)"]
    C --> D["Deduplication Engine<br/>(Planned - C Hash Table)"]
    D --> E["Manifest Generation<br/>(Implemented / Planned)"]
    E --> F["Compression<br/>(Planned - RLE/Huffman/LZW)"]
    F --> G["TCP Socket Transfer<br/>(Planned - Winsock/POSIX)"]
    G --> H["Reconstruction & Verification<br/>(Planned - CRC32)"]
```

> [!NOTE]
> Stages currently implemented: **Project Scanner**, **Chunking Engine** (Fixed & CDC), **Chunk Fingerprinting** (MurmurHash3 128-bit & CRC32), **Workload-Aware Adaptive Chunking Policy**, and **Automated Verification Tests**.
> Stages planned for subsequent development: C-based Hash Table Deduplication, RLE/Huffman/LZW Compression, TCP Sockets, and Reconstruction.

---

## 2. Chunking Engine Architecture

The chunkers process file payloads incrementally instead of loading each whole file into memory. Chunk metadata is retained in memory, so metadata use grows with the number of generated chunks.

```mermaid
flowchart TD
    Scanner["ProjectScanner (DirectoryNode tree)"] --> Engine["ChunkEngine (Coordinator)"]
    Engine --> Policy["AdaptivePolicy (File Evaluation)"]
    Policy -- "Small file / Pre-compressed" --> FixedChunker["FixedChunker (Uniform 64 KiB blocks)"]
    Policy -- "Source code / Text / Scripts" --> CDCChunker["CDCChunker (Gear Rolling Hash CDC)"]
    FixedChunker --> Stream1["Incremental File Stream (64 KiB I/O Buffer)"]
    CDCChunker --> Stream2["Incremental Byte Stream (Gear Window)"]
    Stream1 --> FP["Fingerprinting (MurmurHash3 128-bit + CRC32)"]
    Stream2 --> FP
    FP --> Meta["ChunkMetadata Vector & Performance Metrics"]
    Meta --> Manifest["JSON Manifest (collection/*_chunks.json)"]
```

---

## 3. Chunk Metadata Structure

Each chunk generates a lightweight `ChunkMetadata` record without retaining raw payload bytes in RAM. This separation ensures scalability to multi-gigabyte repositories:

```cpp
struct ChunkMetadata {
    std::string relativePath;   // Relative file path (e.g., "src/scanner.cpp")
    uint64_t offset;            // Starting byte position in file
    uint64_t length;            // Actual chunk length in bytes
    std::string fingerprint;    // 32-char hex digest (MurmurHash3_x64_128)
    std::string algorithm;      // Strategy used ("fixed" or "cdc")
    uint32_t sequenceNumber;    // Sequential chunk index (0, 1, 2, ...)
    uint32_t crc32;             // IEEE 802.3 CRC32 for corruption detection
};
```

Raw bytes can be retrieved on demand at any time using:
`ChunkEngine::readChunkPayload(filePath, offset, length, buffer);`

---

## 4. Chunking Strategies

### A. Fixed-Size Chunking (`FixedChunker`)
- **Concept**: Partitions a file into uniform fixed-size blocks (default: 64 KiB = 65,536 bytes).
- **Final Chunk**: Correctly accommodates partial trailing bytes without appending zero-byte chunks.
- **Divisible Files**: Files whose size is an exact multiple of chunk size emit exactly $N$ chunks (no trailing empty chunk).
- **Empty Files**: Produces 0 chunks and 0 bytes without error.
- **Use Case**: Serves as the performance baseline and optimal choice for pre-compressed media or high-entropy archives.

### B. Content-Defined Chunking (`CDCChunker`)
- **Concept**: Uses a Gear rolling hash to identify boundaries based on byte content rather than fixed offsets, mitigating the "byte-shift problem" caused by insertions/deletions.
- **Gear Hash Algorithm**:
  A 256-entry 64-bit lookup table `GEAR_MATRIX[256]` updates rolling state with 1 bitwise shift, 1 array lookup, and 1 integer addition per byte:
  $$\text{rolling\_hash} = (\text{rolling\_hash} \ll 1) + \text{GEAR\_MATRIX}[\text{byte}]$$
- The lookup table is generated deterministically from a fixed seed so the same data and configuration produce the same boundaries across runs. The previous table in this branch contained only odd values, making the low-bit-zero boundary condition impossible; the corrected table changes CDC boundaries compared with old manifests. Regenerate old chunk manifests before comparing runs across this fix.
- **Boundary Detection Rules**:
  1. *Minimum Chunk Constraint*: No boundary is evaluated while chunk size $< \text{minChunkSize}$ (default: 16 KiB).
  2. *Content Boundary*: For chunk size $\ge \text{minChunkSize}$, a boundary is declared when:
     $$(\text{rolling\_hash} \ \& \ \text{mask}) == 0$$
     Where $\text{mask} = 2^k - 1$ produces an expected average chunk size of $2^k \approx \text{targetChunkSize}$ (default: 64 KiB).
  3. *Maximum Chunk Constraint*: If chunk size reaches $\text{maxChunkSize}$ (default: 256 KiB) without triggering a content boundary, a hard boundary cut is forced.
  4. *Stream Termination*: Remaining bytes at EOF form the final partial chunk.
- **Advantage**: CDC can preserve boundaries after localized insertions or deletions, improving reuse compared with fixed offsets. The actual reuse rate depends on the input data and chunk parameters; it is not guaranteed to be limited to 1 or 2 chunks.

---

## 5. Chunk Fingerprinting & Integrity Verification

- **MurmurHash3_x64_128**:
  - Implements Austin Appleby's reference algorithm for 128-bit non-cryptographic hashing.
  - Formatted as a deterministic 32-character lowercase hexadecimal string.
  - High-throughput non-cryptographic hashing. Actual throughput depends on hardware, compiler, and input size.
  - Handles arbitrary binary streams and embedded null bytes correctly.
- **Collision Safety**:
  - MurmurHash3 is non-cryptographic, so its fingerprint must not be treated as a security guarantee.
  - `readChunkPayload` can reread bytes for comparison, but the deduplication/transfer engine is not implemented yet. `compareVersions` currently compares fingerprints only; do not use its matches as a collision-proof transfer decision.
- **CRC32 Checksum**:
  - Standard IEEE 802.3 CRC32 polynomial (`0xEDB88320`) computed for every chunk to detect transport bit corruption.

---

## 6. Workload-Aware Adaptive Chunking Strategy

Rather than naively applying CDC or Fixed chunking uniformly across all files, RetroTransfer evaluates each file's characteristics:

| File Category | Criteria | Chosen Strategy | Algorithmic Rationale |
| :--- | :--- | :--- | :--- |
| **Sub-threshold Files** | Size $\le \text{minChunkSize}$ (16 KiB) | **Fixed** | CDC cannot create an earlier boundary below the minimum; fixed chunking avoids rolling-hash work. With default settings, these files usually fit in one fixed chunk. |
| **Pre-compressed / Media** | `.zip`, `.gz`, `.png`, `.jpg`, `.mp4`, `.pdf`, `.7z` | **Fixed** | These formats often have high entropy; fixed chunking avoids the rolling-hash work. Actual throughput depends on the input and runtime environment. |
| **Source Code & Text** | `.cpp`, `.h`, `.py`, `.java`, `.js`, `.json`, `.md` | **CDC** | Project source files frequently experience localized line insertions and deletions; CDC prevents cascading boundary shifts across subsequent lines. |
| **Large General Data** | Size $> \text{minChunkSize}$ | **CDC** | Content-defined boundaries maximize deduplication resilience across version evolutions. |
| **Explicit CLI Override** | User passes `--strategy fixed` or `--strategy cdc` | **Override** | Direct user instruction unconditionally overrides heuristic decisions. |

Every file decision records its rationale string in the JSON manifest for auditable grading and research evaluation.

---

## 7. CLI Usage & Options

The command syntax supports the established positional arguments and strategy options:

```bash
# Basic scan + default adaptive chunking
./RetroTransfer [directory] [output.json]

# Select chunking strategy explicitly
./RetroTransfer . scan.json --strategy fixed --chunk-size 32768
./RetroTransfer . scan.json --strategy cdc --min-chunk 4096 --target-chunk 16384 --max-chunk 65536
./RetroTransfer . scan.json --strategy adaptive

# Run comparative benchmark across Fixed, CDC, and Adaptive
./RetroTransfer . scan.json --benchmark

# Scan-only mode (skip chunking)
./RetroTransfer . scan.json --no-chunk

# Help
./RetroTransfer --help
```

Chunk-size arguments must be positive integers no larger than 1 GiB. Invalid or inconsistent sizes are rejected before scanning.

---

## 8. Automated Testing

RetroTransfer includes a dedicated CTest suite covering 23 edge cases:

```bash
ctest --test-dir build --output-on-failure
# or run executable directly:
./build/test_chunking.exe
```

### Verified Test Cases:
1. `test_empty_file` (0 bytes $\to$ 0 chunks, no empty chunk)
2. `test_one_byte_file` (1 byte $\to$ 1 chunk of length 1)
3. `test_smaller_than_chunk_size` (sub-chunk handling)
4. `test_exact_chunk_size` (exact 1 chunk, no trailing chunk)
5. `test_exactly_divisible_by_chunk_size` ($N \times \text{size} \to$ exactly $N$ chunks)
6. `test_not_divisible_by_chunk_size` (accurate partial last chunk)
7. `test_binary_data_embedded_nulls` (binary safety, embedded 0x00)
8. `test_identical_files_produce_identical_fingerprints` (determinism)
9. `test_changed_contents_produce_different_fingerprints` (checks that modified content changes the fingerprint)
10. `test_correct_offsets_and_lengths` (accurate offset summation and byte reread)
11. `test_cdc_min_chunk_size_behavior` (no chunk $< \text{minChunkSize}$ except EOF)
12. `test_cdc_max_chunk_size_behavior` (no chunk $> \text{maxChunkSize}$)
13. `test_cdc_deterministic_output` (CDC determinism across runs)
14. `test_invalid_configurations` (error handling for invalid size configs)
15. `test_missing_unreadable_file` (graceful error handling)
16. `test_large_file_incremental` (1 MB streaming verification)
17. `test_adaptive_mode_heuristics` (verifies rule tree for code, zip, and small files)
18. `test_explicit_mode_overrides` (override flag verification)
19. `test_cdc_content_boundaries_trigger` (ensures CDC finds content-defined cuts before max size and file/buffer paths agree)
20. `test_scanner_skips_generated_dirs_and_symlinks` (avoids self-scanning generated data and symlink recursion)
21. `test_payload_reread_and_byte_comparison` (rereads bytes and compares them directly)
22. `test_version_comparison_metrics` (reusable bytes & reuse ratio calculation)
23. `test_crc32_standard_vector` (validates the standard CRC32 result for `123456789`)

---

## 9. Benchmark & Example Results

Running `./RetroTransfer . scan.json --benchmark` prints a comparative summary. The following numbers are an illustrative example, not a performance guarantee; results depend on the project, hardware, and configuration:

```
============================================================
        RETROTRANSFER CHUNKING STRATEGY BENCHMARK           
============================================================
Strategy    Chunks    Unique FPs  Dedup %     Time (ms)     Throughput(MB/s)  Metadata (KB)   
------------------------------------------------------------------------------------
fixed       23        23          0.0%        12.54         8.76              3               
cdc         23        23          0.0%        11.22         9.79              3               
adaptive    23        23          0.0%        9.73          11.29             3               
============================================================
```

Output manifests:
- `collection/scan.json`: Directory hierarchy (unchanged scanner output format)
- `collection/scan_chunks.json`: Full chunk manifest, fingerprints, adaptive rationale, and metrics

---

## 10. Current Limitations & Next Steps

### Current Limitations:
- The adaptive policy currently uses file extension and size heuristics; it does not yet profile runtime Shannon entropy per block.
- Deduplication ratio is currently reported across chunks within the scanned project baseline; multi-host transfer history will be added in the deduplication engine stage.

### Recommended Next Steps for Deduplication Stage:
1. **C Hash Table**: Implement an open-addressing or chained hash table keyed on the 128-bit `Fingerprint128` structs.
2. **Chunk Index / Store**: Map fingerprints to disk locations (`filePath`, `offset`, `length`).
3. **Candidate Verification**: Read payloads via `readChunkPayload` to confirm zero false-positive collisions before flagging chunks as reusable.
4. **Delta Manifest**: Output a transfer manifest containing only *new* chunks alongside a recipe for reconstructing identical files at the receiver.
