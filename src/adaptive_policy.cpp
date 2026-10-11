#include "adaptive_policy.h"

#include <algorithm>
#include <cctype>

AdaptivePolicy::AdaptivePolicy() {
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool AdaptivePolicy::isPrecompressedExtension(const std::string& extRaw) {
    std::string ext = toLower(extRaw);
    static const char* precompressed[] = {
        ".zip", ".gz", ".tar.gz", ".tgz", ".7z", ".rar", ".xz", ".bz2",
        ".png", ".jpg", ".jpeg", ".webp", ".gif", ".mp4", ".mkv", ".avi",
        ".mp3", ".wav", ".flac", ".ogg", ".pdf", ".iso", ".apk", ".jar"
    };
    for (const char* item : precompressed) {
        if (ext == item) return true;
    }
    return false;
}

bool AdaptivePolicy::isSourceOrTextExtension(const std::string& extRaw) {
    std::string ext = toLower(extRaw);
    static const char* textExts[] = {
        ".cpp", ".h", ".c", ".hpp", ".cc", ".cxx", ".py", ".java", ".js",
        ".ts", ".jsx", ".tsx", ".html", ".css", ".scss", ".json", ".xml",
        ".md", ".txt", ".csv", ".yml", ".yaml", ".sql", ".sh", ".bash",
        ".bat", ".cmd", ".ps1", ".cmake", ".rs", ".go", ".rb", ".php",
        ".lua", ".ini", ".cfg", ".toml", ".gradle", ".properties"
    };
    for (const char* item : textExts) {
        if (ext == item) return true;
    }
    return false;
}

PolicyDecision AdaptivePolicy::decide(const FileInfo& file,
                                      ChunkStrategy globalStrategy,
                                      const FixedChunkConfig& defaultFixed,
                                      const CDCConfig& defaultCDC) const {
    PolicyDecision decision;
    decision.evaluatedFileSize = file.fileSize;
    decision.evaluatedFileType = file.fileType;
    decision.fixedConfig = defaultFixed;
    decision.cdcConfig = defaultCDC;

    // 1. Explicit overrides
    if (globalStrategy == ChunkStrategy::Fixed) {
        decision.selectedStrategy = ChunkStrategy::Fixed;
        decision.wasOverridden = true;
        decision.reason = "User configuration override: fixed-size chunking explicitly requested.";
        return decision;
    }

    if (globalStrategy == ChunkStrategy::CDC) {
        decision.selectedStrategy = ChunkStrategy::CDC;
        decision.wasOverridden = true;
        decision.reason = "User configuration override: Content-Defined Chunking (CDC) explicitly requested.";
        return decision;
    }

    // 2. Adaptive Mode Heuristic Decision Tree

    // Rule A: File smaller than CDC minimum chunk size
    if (file.fileSize <= defaultCDC.minChunkSize) {
        decision.selectedStrategy = ChunkStrategy::Fixed;
        decision.reason = "File size (" + std::to_string(file.fileSize) + 
                          " B) <= CDC minimum chunk threshold (" + 
                          std::to_string(defaultCDC.minChunkSize) + 
                          " B); fixed chunking avoids rolling-hash CPU cost for small files.";
        return decision;
    }

    // Rule B: Pre-compressed / high-entropy format
    if (isPrecompressedExtension(file.fileType)) {
        decision.selectedStrategy = ChunkStrategy::Fixed;
        decision.reason = "Pre-compressed/high-entropy format (" + file.fileType + 
                          "); bitstream diffusion makes CDC content-shift boundary alignment ineffective; fixed chunking maximizes throughput.";
        return decision;
    }

    // Rule C: Source code or structured text
    if (isSourceOrTextExtension(file.fileType)) {
        decision.selectedStrategy = ChunkStrategy::CDC;
        decision.reason = "Source code or text file (" + file.fileType + 
                          "); content-defined chunking isolates localized line edits and prevents downstream boundary shift.";
        return decision;
    }

    // Rule D: Fallback for generic or unknown files
    // If file is moderately large, CDC provides shift-resilience during incremental updates.
    decision.selectedStrategy = ChunkStrategy::CDC;
    decision.reason = "General file type (" + (file.fileType.empty() ? "none" : file.fileType) + 
                      ") > CDC min size; CDC selected as resilient default for incremental project transfer.";
    return decision;
}
