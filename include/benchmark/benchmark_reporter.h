#ifndef RETROTRANSFER_BENCHMARK_REPORTER_H
#define RETROTRANSFER_BENCHMARK_REPORTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace retro::benchmark {

struct BenchmarkResult {
    std::string scenario;
    std::string timestamp;

    uint64_t originalBytes = 0;
    uint64_t transmittedBytes = 0;
    uint64_t deduplicatedBytes = 0;
    uint64_t compressionSavingsBytes = 0;
    uint64_t transferTimeMs = 0;

    uint32_t totalChunks = 0;
    uint32_t transferredChunks = 0;

    double throughputMBps = 0.0;
    double deduplicationPercent = 0.0;
    double compressionSavingsPercent = 0.0;
};

class BenchmarkReporter {
public:
    static BenchmarkResult calculateMetrics(
        const BenchmarkResult& result
    );

    void addResult(const BenchmarkResult& result);

    bool exportCSV(
        const std::string& filePath,
        std::string& error
    ) const;

    bool exportJSON(
        const std::string& filePath,
        std::string& error
    ) const;

    const std::vector<BenchmarkResult>& getResults() const;

private:
    std::vector<BenchmarkResult> results_;
};

} // namespace retro::benchmark

#endif