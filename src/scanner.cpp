#include "scanner.h"

#include <chrono>
#include <iostream>
using namespace std;

namespace fs = filesystem;

ProjectScanner::ProjectScanner(const string& path) : root_path(path) {
    ignored_folders = {".git", "build", "node_modules", ".vscode", "bin", "obj"};
    ignored_extensions = {".o", ".exe", ".dll", ".class"};
}

DirectoryNode scanDirectory(const fs::path& directory,
                             const vector<string>& ignored_folders,
                             const vector<string>& ignored_extensions) {
    DirectoryNode node;
    node.directoryName = directory.filename().string();
    if (node.directoryName.empty()) {
        node.directoryName = directory.string();
    }
    node.directoryPath = directory.string();

    for (const auto& entry : fs::directory_iterator(directory)) {
        const fs::path entryPath = entry.path();
        bool ignoredFolder = false;
        for (const auto& folder : ignored_folders) {
            if (entryPath.filename() == folder) {
                ignoredFolder = true;
                break;
            }
        }
        if (ignoredFolder) {
            continue;
        }

        if (entry.is_directory()) {
            node.directories.push_back(scanDirectory(entryPath, ignored_folders, ignored_extensions));
        } else if (entry.is_regular_file()) {
            bool ignoredExtension = false;
            for (const auto& extension : ignored_extensions) {
                if (entryPath.extension() == extension) {
                    ignoredExtension = true;
                    break;
                }
            }
            if (!ignoredExtension) {
                FileInfo info;
                info.fileName = entryPath.filename().string();
                info.filePath = entryPath.string();
                info.fileSize = fs::file_size(entryPath);
                info.fileType = entryPath.extension().string();
                info.modificationTime = chrono::duration_cast<chrono::seconds>(
                    entry.last_write_time().time_since_epoch()).count();
                node.files.push_back(info);
            }
        }
    }
    return node;
}

DirectoryNode ProjectScanner::scan() {
    if (!fs::exists(root_path) || !fs::is_directory(root_path)) {
        cerr << "Error: Directory does not exist.\n";
        return {};
    }

    return scanDirectory(root_path, ignored_folders, ignored_extensions);
}