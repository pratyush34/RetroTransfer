#ifndef RETROTRANSFER_MANIFEST_WRITER_H
#define RETROTRANSFER_MANIFEST_WRITER_H

#include "chunk_engine.h"
#include "chunk_config.h"
#include "compression_engine.h"

#include <ostream>
#include <string>

namespace retro {

/*
 * Canonical JSON escaping for strings according to RFC 8259.
 */
std::string jsonEscape(const std::string& value);

/*
 * Canonical manifest generation function for RetroTransfer.
 * Writes the structured JSON recipe manifest that pairs with an .rtpack bundle.
 */
void writeChunkManifestJson(std::ostream& out,
                            const ProjectChunkResult& result,
                            const EngineConfig& config,
                            const std::string& rootPath,
                            const std::string& baselinePath,
                            const std::string& payloadName,
                            const CompressionResult* compression);

} // namespace retro

#endif // RETROTRANSFER_MANIFEST_WRITER_H
