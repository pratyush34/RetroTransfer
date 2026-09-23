#ifndef SCANNER_H
#define SCANNER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
using namespace std;

struct FileInfo {
    string fileName;
    string filePath;
    uintmax_t fileSize;
    string fileType;
    int64_t modificationTime;
};

struct DirectoryNode {
    string directoryName;
    string directoryPath;
    vector<DirectoryNode> directories;
    vector<FileInfo> files;
};

class ProjectScanner {
    private:
    string root_path;
    vector<string> ignored_folders;
    vector<string> ignored_extensions;

public:
    ProjectScanner(const string& path);
    DirectoryNode scan();
};


#endif 