#include "benchmark/benchmark_reporter.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace retro::benchmark {

namespace {

// Escape special characters for CSV fields.
std::string escapeCSV(const std::string& value) {
    std::string result = "\"";

    for (char ch : value) {
        if (ch == '"') {
            result += "\"\"";
        } else {
            result += ch;
        }
    }

    result += "\"";
    return result;
}

// Escape special characters for JSON strings.
std::string escapeJSON(const std::string& value) {
    std::string result;

    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    const char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[(ch >> 4) & 0x0F];
                    result += hex[ch & 0x0F];
                } else {
                    result += static_cast<char>(ch);
                }
        }
    }

    return result;
}

} // anonymous namespace


BenchmarkResult BenchmarkReporter::calculateMetrics(
    const BenchmarkResult& input
) {
    BenchmarkResult result = input;

    // Calculate transfer throughput in MiB/s.
    if (result.transferTimeMs > 0) {
        result.throughputMBps =
            (static_cast<double>(result.transmittedBytes) /
             (1024.0 * 1024.0)) /
            (static_cast<double>(result.transferTimeMs) / 1000.0);
    } else {
        result.throughputMBps = 0.0;
    }

    // Calculate deduplication percentage.
    if (result.originalBytes > 0) {
        result.deduplicationPercent =
            (static_cast<double>(result.deduplicatedBytes) /
             static_cast<double>(result.originalBytes)) * 100.0;
    } else {
        result.deduplicationPercent = 0.0;
    }

    // Calculate compression savings percentage.
    const uint64_t compressionInputBytes =
        result.transmittedBytes + result.compressionSavingsBytes;

    if (compressionInputBytes > 0) {
        result.compressionSavingsPercent =
            (static_cast<double>(result.compressionSavingsBytes) /
             static_cast<double>(compressionInputBytes)) * 100.0;
    } else {
        result.compressionSavingsPercent = 0.0;
    }

    return result;
}


void BenchmarkReporter::addResult(const BenchmarkResult& result) {
    results_.push_back(calculateMetrics(result));
}


bool BenchmarkReporter::exportCSV(
    const std::string& filePath,
    std::string& error
) const {
    std::ofstream file(filePath);

    if (!file.is_open()) {
        error = "Could not open CSV output file: " + filePath;
        return false;
    }

    file << "Scenario,Timestamp,Original Bytes,Transmitted Bytes,"
            "Deduplicated Bytes,Compression Savings Bytes,"
            "Transfer Time (ms),Total Chunks,Transferred Chunks,"
            "Throughput (MiB/s),Deduplication (%),Compression Savings (%)\n";

    file << std::fixed << std::setprecision(4);

    for (const auto& result : results_) {
        file << escapeCSV(result.scenario) << ","
             << escapeCSV(result.timestamp) << ","
             << result.originalBytes << ","
             << result.transmittedBytes << ","
             << result.deduplicatedBytes << ","
             << result.compressionSavingsBytes << ","
             << result.transferTimeMs << ","
             << result.totalChunks << ","
             << result.transferredChunks << ","
             << result.throughputMBps << ","
             << result.deduplicationPercent << ","
             << result.compressionSavingsPercent << "\n";
    }

    if (!file.good()) {
        error = "Failed while writing CSV benchmark report.";
        return false;
    }

    return true;
}


bool BenchmarkReporter::exportJSON(
    const std::string& filePath,
    std::string& error
) const {
    std::ofstream file(filePath);

    if (!file.is_open()) {
        error = "Could not open JSON output file: " + filePath;
        return false;
    }

    file << std::fixed << std::setprecision(4);
    file << "{\n  \"benchmarks\": [\n";

    for (size_t i = 0; i < results_.size(); ++i) {
        const auto& result = results_[i];

        file << "    {\n"
             << "      \"scenario\": \"" << escapeJSON(result.scenario) << "\",\n"
             << "      \"timestamp\": \"" << escapeJSON(result.timestamp) << "\",\n"
             << "      \"originalBytes\": " << result.originalBytes << ",\n"
             << "      \"transmittedBytes\": " << result.transmittedBytes << ",\n"
             << "      \"deduplicatedBytes\": " << result.deduplicatedBytes << ",\n"
             << "      \"compressionSavingsBytes\": " << result.compressionSavingsBytes << ",\n"
             << "      \"transferTimeMs\": " << result.transferTimeMs << ",\n"
             << "      \"totalChunks\": " << result.totalChunks << ",\n"
             << "      \"transferredChunks\": " << result.transferredChunks << ",\n"
             << "      \"throughputMBps\": " << result.throughputMBps << ",\n"
             << "      \"deduplicationPercent\": " << result.deduplicationPercent << ",\n"
             << "      \"compressionSavingsPercent\": " << result.compressionSavingsPercent << "\n"
             << "    }";

        if (i + 1 < results_.size()) {
            file << ",";
        }

        file << "\n";
    }

    file << "  ]\n}\n";

    if (!file.good()) {
        error = "Failed while writing JSON benchmark report.";
        return false;
    }

    return true;
}


const std::vector<BenchmarkResult>&
BenchmarkReporter::getResults() const {
    return results_;
}

} // namespace retro::benchmark