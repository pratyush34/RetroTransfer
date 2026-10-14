#include "scanner.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

ProjectScanner::ProjectScanner(const std::string& path) : root_path(path) {
    ignored_folders = {".git", "collection", "node_modules", ".vscode", "bin", "obj"};
    ignored_extensions = {".o", ".exe", ".dll", ".class"};
}

static std::string lowerAscii(std::string value) {
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z') {
            character = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

static bool containsName(const std::vector<std::string>& names, const fs::path& path) {
    const std::string name = lowerAscii(path.filename().string());
    for (const auto& candidate : names) {
        if (name == lowerAscii(candidate)) return true;
    }
    return false;
}

static bool isBuildOutputDirectory(const fs::path& path) {
    const std::string name = lowerAscii(path.filename().string());
    return name == "build" || name.rfind("build-", 0) == 0;
}

static bool containsExtension(const std::vector<std::string>& extensions, const fs::path& path) {
    const std::string extension = lowerAscii(path.extension().string());
    for (const auto& candidate : extensions) {
        if (extension == candidate) return true;
    }
    return false;
}

static DirectoryNode scanDirectory(const fs::path& directory,
                                   const std::vector<std::string>& ignoredFolders,
                                   const std::vector<std::string>& ignoredExtensions) {
    DirectoryNode node;
    node.directoryName = directory.filename().string();
    if (node.directoryName.empty()) {
        node.directoryName = directory.string();
    }
    node.directoryPath = directory.string();

    std::error_code ec;
    fs::directory_iterator iterator(
        directory, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    if (ec) {
        std::cerr << "Warning [ProjectScanner]: Could not read directory "
                  << directory.string() << ": " << ec.message() << "\n";
        return node;
    }

    while (iterator != end) {
        const fs::directory_entry entry = *iterator;
        const fs::path entryPath = entry.path();

        // Do not follow symlinks. A symlink may point outside the project or back
        // to an ancestor and otherwise cause unbounded recursion.
        const bool isSymlink = entry.is_symlink(ec);
        if (ec) {
            ec.clear();
        } else if (isSymlink) {
            iterator.increment(ec);
            if (ec) ec.clear();
            continue;
        }

        const bool isDirectory = entry.is_directory(ec);
        if (ec) {
            ec.clear();
            iterator.increment(ec);
            if (ec) ec.clear();
            continue;
        }

        if (isDirectory) {
            if (!containsName(ignoredFolders, entryPath) &&
                !isBuildOutputDirectory(entryPath)) {
                node.directories.push_back(
                    scanDirectory(entryPath, ignoredFolders, ignoredExtensions));
            }
        } else {
            const bool isRegularFile = entry.is_regular_file(ec);
            if (ec) {
                ec.clear();
                iterator.increment(ec);
                if (ec) ec.clear();
                continue;
            }
            if (isRegularFile && !containsExtension(ignoredExtensions, entryPath)) {
                std::error_code sizeError;
                const uintmax_t fileSize = fs::file_size(entryPath, sizeError);
                if (sizeError) {
                    std::cerr << "Warning [ProjectScanner]: Could not get size for "
                              << entryPath.string() << ": " << sizeError.message() << "\n";
                    iterator.increment(ec);
                    if (ec) ec.clear();
                    continue;
                }

                std::error_code timeError;
                const auto writeTime = entry.last_write_time(timeError);
                int64_t modificationTime = 0;
                if (!timeError) {
                    modificationTime = std::chrono::duration_cast<std::chrono::seconds>(
                        writeTime.time_since_epoch()).count();
                }

                FileInfo info;
                info.fileName = entryPath.filename().string();
                info.filePath = entryPath.string();
                info.fileSize = fileSize;
                info.fileType = entryPath.extension().string();
                info.modificationTime = modificationTime;
                node.files.push_back(std::move(info));
            }
        }

        iterator.increment(ec);
        if (ec) {
            std::cerr << "Warning [ProjectScanner]: Directory iteration issue in "
                      << directory.string() << ": " << ec.message() << "\n";
            ec.clear();
        }
    }

    // Filesystem iteration order is unspecified; sort to keep scan output and
    // processing order reproducible between runs.
    std::sort(node.directories.begin(), node.directories.end(),
              [](const DirectoryNode& left, const DirectoryNode& right) {
                  return left.directoryName < right.directoryName;
              });
    std::sort(node.files.begin(), node.files.end(),
              [](const FileInfo& left, const FileInfo& right) {
                  return left.fileName < right.fileName;
              });
    return node;
}

DirectoryNode ProjectScanner::scan() {
    std::error_code ec;
    if (!fs::exists(root_path, ec) || ec || !fs::is_directory(root_path, ec) || ec) {
        std::cerr << "Error: Directory does not exist or is not accessible: "
                  << root_path << "\n";
        return {};
    }

    return scanDirectory(root_path, ignored_folders, ignored_extensions);
}
