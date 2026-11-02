#ifndef RETROTRANSFER_RECONSTRUCTION_ENGINE_H
#define RETROTRANSFER_RECONSTRUCTION_ENGINE_H

#include <cstddef>
#include <string>

struct ReconstructionResult {
    size_t filesWritten = 0;
    size_t chunksWritten = 0;
    size_t chunksReused = 0;
};

class ReconstructionEngine {
public:
    bool reconstruct(const std::string& manifestPath,
                     const std::string& payloadPath,
                     const std::string& outputDirectory,
                     ReconstructionResult& result,
                     std::string& error) const;
};

#endif
