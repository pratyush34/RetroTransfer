#include "scanner.h"
#include <fstream>
#include <filesystem>
#include <iostream>

static string jsonEscape(const string& value) {
    string escaped;
    for (char character : value) {
        if (character == '\\' || character == '"') {
            escaped += '\\';
        }
        escaped += character;
    }
    return escaped;
}

static void writeIndent(ofstream& output, int depth) {
    output << string(depth * 2, ' ');
}

static void writeDirectory(ofstream& output, const DirectoryNode& directory, int depth) {
    writeIndent(output, depth);
    output << "{\n";
    writeIndent(output, depth + 1);
    output << "\"name\": \"" << jsonEscape(directory.directoryName) << "\",\n";
    writeIndent(output, depth + 1);
    output << "\"path\": \"" << jsonEscape(directory.directoryPath) << "\",\n";
    writeIndent(output, depth + 1);
    output << "\"directories\": [\n";
    for (size_t index = 0; index < directory.directories.size(); ++index) {
        writeDirectory(output, directory.directories[index], depth + 2);
        if (index + 1 < directory.directories.size()) {
            output << ",";
        }
        output << "\n";
    }
    writeIndent(output, depth + 1);
    output << "],\n";
    writeIndent(output, depth + 1);
    output << "\"files\": [\n";
    for (size_t index = 0; index < directory.files.size(); ++index) {
        const FileInfo& file = directory.files[index];
        writeIndent(output, depth + 2);
        output << "{\"name\": \"" << jsonEscape(file.fileName)
               << "\", \"path\": \"" << jsonEscape(file.filePath)
               << "\", \"size\": " << file.fileSize
               << ", \"type\": \"" << jsonEscape(file.fileType)
               << "\", \"modificationTime\": " << file.modificationTime << "}";
        if (index + 1 < directory.files.size()) {
            output << ",";
        }
        output << "\n";
    }
    writeIndent(output, depth + 1);
    output << "]\n";
    writeIndent(output, depth);
    output << "}";
}

int main(int argc, char* argv[]) {
    const string path = argc > 1 ? argv[1] : ".";
    const filesystem::path collectionFolder = "collection";
    const string outputName = argc > 2 ? argv[2] : "scan.json";
    const filesystem::path outputPath = collectionFolder / filesystem::path(outputName).filename();
    ProjectScanner scanner(path);
    const DirectoryNode directoryTree = scanner.scan();

    filesystem::create_directories(collectionFolder);
    ofstream output(outputPath);
    if (!output) {
        cerr << "Error: Could not create output file.\n";
        return 1;
    }

    writeDirectory(output, directoryTree, 0);
    output << "\n";

    cout << "Saved scan data to " << outputPath << "\n";

    return 0;
}