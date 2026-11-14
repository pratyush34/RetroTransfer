#include "benchmark/benchmark_reporter.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace retro::benchmark;
namespace fs = std::filesystem;

int main() {
    BenchmarkReporter reporter;

    BenchmarkResult result;
    result.scenario = "First Transfer";
    result.timestamp = "2026-11-14";

    result.originalBytes = 10 * 1024 * 1024;
    result.transmittedBytes = 5 * 1024 * 1024;
    result.deduplicatedBytes = 2 * 1024 * 1024;
    result.compressionSavingsBytes = 1 * 1024 * 1024;
    result.transferTimeMs = 2000;

    result.totalChunks = 100;
    result.transferredChunks = 50;

    BenchmarkResult calculated =
        BenchmarkReporter::calculateMetrics(result);

    assert(std::abs(calculated.throughputMBps - 2.5) < 0.001);
    assert(std::abs(calculated.deduplicationPercent - 20.0) < 0.001);
    assert(std::abs(calculated.compressionSavingsPercent - (100.0 / 6.0)) < 0.001);

    reporter.addResult(result);

    assert(reporter.getResults().size() == 1);

    std::string error;

    const fs::path outputDir = fs::temp_directory_path() /
                               "retrotransfer_benchmark_tests";

    fs::create_directories(outputDir);

    const fs::path csvPath = outputDir / "results.csv";
    const fs::path jsonPath = outputDir / "results.json";

    assert(reporter.exportCSV(csvPath.string(), error));
    assert(reporter.exportJSON(jsonPath.string(), error));

    assert(fs::exists(csvPath));
    assert(fs::exists(jsonPath));

    std::ifstream csv(csvPath);
    std::ifstream json(jsonPath);

    assert(csv.good());
    assert(json.good());

    std::cout << "All benchmark tests passed.\n";

    fs::remove_all(outputDir);

    return 0;
}