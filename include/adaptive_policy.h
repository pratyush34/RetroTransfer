#ifndef ADAPTIVE_POLICY_H
#define ADAPTIVE_POLICY_H

#include "scanner.h"
#include "chunk_config.h"

#include <string>

/**
 * @brief Records the policy decision for a file, including rationale and inputs.
 */
struct PolicyDecision {
    ChunkStrategy selectedStrategy = ChunkStrategy::Fixed;
    std::string reason;

    FixedChunkConfig fixedConfig;
    CDCConfig cdcConfig;

    uint64_t evaluatedFileSize = 0;
    std::string evaluatedFileType;
    bool wasOverridden = false;
};

/**
 * @brief Workload-Aware Adaptive Chunking Strategy Policy.
 * 
 * Evaluates file characteristics (size, type, entropy clues) to select
 * between Fixed-size chunking and Content-Defined Chunking (CDC).
 * 
 * Transparent Heuristic Rules:
 * 1. Small files (<= min CDC chunk size): Fixed chunking avoids rolling-hash CPU cost.
 * 2. Pre-compressed/media formats: Fixed chunking avoids useless boundary searches on high-entropy streams.
 * 3. Source code/text files: CDC isolates localized edits and prevents boundary shifts.
 * 4. General/unknown files: Configurable default (CDC for shift-resilience).
 * 5. Explicit user override: Respects user-specified fixed or cdc flags unconditionally.
 */
class AdaptivePolicy {
public:
    AdaptivePolicy();

    /**
     * @brief Evaluates an individual file and returns the selected strategy with rationale.
     * 
     * @param file FileInfo from the ProjectScanner.
     * @param globalStrategy The global operating mode (Fixed, CDC, or Adaptive).
     * @param defaultFixed Fixed-size configuration.
     * @param defaultCDC CDC configuration.
     * @return PolicyDecision detailing strategy, configuration, and reason.
     */
    PolicyDecision decide(const FileInfo& file,
                          ChunkStrategy globalStrategy,
                          const FixedChunkConfig& defaultFixed,
                          const CDCConfig& defaultCDC) const;

    /**
     * @brief Checks if a file extension indicates pre-compressed or high-entropy data.
     */
    static bool isPrecompressedExtension(const std::string& ext);

    /**
     * @brief Checks if a file extension indicates human-editable source code or text.
     */
    static bool isSourceOrTextExtension(const std::string& ext);
};

#endif // ADAPTIVE_POLICY_H
