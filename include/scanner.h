#ifndef SCANNER_H
#define SCANNER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct FileInfo {
    std::string fileName;
    std::string filePath;
    uintmax_t fileSize;
    std::string fileType;
    int64_t modificationTime;
};

struct DirectoryNode {
    std::string directoryName;
    std::string directoryPath;
    std::vector<DirectoryNode> directories;
    std::vector<FileInfo> files;
};

class ProjectScanner {
    private:
    std::string root_path;
    std::vector<std::string> ignored_folders;
    std::vector<std::string> ignored_extensions;

public:
    ProjectScanner(const std::string& path);
    DirectoryNode scan();
};


#endif 