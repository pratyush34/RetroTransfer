#ifndef TEST_SCANNER_COVERAGE_HPP
#define TEST_SCANNER_COVERAGE_HPP

#include "scanner.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

static void test_scanner_empty_directory() {
    const fs::path emptyDir = "test_scratch_retro_transfer/scanner_empty";
    fs::create_directories(emptyDir);

    ProjectScanner scanner(emptyDir.string());
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.files.empty(), "Empty directory should contain 0 files");
    TEST_ASSERT(root.directories.empty(), "Empty directory should contain 0 subdirectories");
    TEST_ASSERT(!root.directoryName.empty(), "Directory node must have a name");
}

static void test_scanner_single_and_multiple_files() {
    const fs::path multiDir = "test_scratch_retro_transfer/scanner_multi";
    fs::create_directories(multiDir);

    const std::vector<uint8_t> data1 = {'A', 'B', 'C'};
    const std::vector<uint8_t> data2 = {'1', '2', '3', '4', '5'};
    const std::vector<uint8_t> data3(100, 'Z');

    {
        std::ofstream f1(multiDir / "file_a.txt", std::ios::binary);
        f1.write(reinterpret_cast<const char*>(data1.data()), data1.size());
        std::ofstream f2(multiDir / "file_b.bin", std::ios::binary);
        f2.write(reinterpret_cast<const char*>(data2.data()), data2.size());
        std::ofstream f3(multiDir / "file_c.dat", std::ios::binary);
        f3.write(reinterpret_cast<const char*>(data3.data()), data3.size());
    }

    ProjectScanner scanner(multiDir.string());
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.files.size() == 3, "Expected 3 files in directory");
    TEST_ASSERT(root.files[0].fileName == "file_a.txt", "Files should be sorted alphabetically");
    TEST_ASSERT(root.files[0].fileSize == 3, "file_a.txt size mismatch");
    TEST_ASSERT(root.files[1].fileName == "file_b.bin", "Files should be sorted alphabetically");
    TEST_ASSERT(root.files[1].fileSize == 5, "file_b.bin size mismatch");
    TEST_ASSERT(root.files[2].fileName == "file_c.dat", "Files should be sorted alphabetically");
    TEST_ASSERT(root.files[2].fileSize == 100, "file_c.dat size mismatch");
}

static void test_scanner_nested_directories() {
    const fs::path baseDir = "test_scratch_retro_transfer/scanner_nested";
    const fs::path deepDir = baseDir / "level1" / "level2" / "level3";
    fs::create_directories(deepDir);

    {
        std::ofstream out(deepDir / "deep_file.txt");
        out << "deep content";
    }

    ProjectScanner scanner(baseDir.string());
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.directories.size() == 1, "Expected 1 subdirectory in root");
    TEST_ASSERT(root.directories[0].directoryName == "level1", "Expected level1 directory");
    TEST_ASSERT(root.directories[0].directories.size() == 1, "Expected level2 directory");
    TEST_ASSERT(root.directories[0].directories[0].directories.size() == 1, "Expected level3 directory");
    const DirectoryNode& l3 = root.directories[0].directories[0].directories[0];
    TEST_ASSERT(l3.files.size() == 1, "Expected 1 file in level3");
    TEST_ASSERT(l3.files[0].fileName == "deep_file.txt", "Deep file name mismatch");
}

static void test_scanner_ignored_folders_pruning() {
    const fs::path baseDir = "test_scratch_retro_transfer/scanner_ignored_dirs";
    fs::create_directories(baseDir / ".git");
    fs::create_directories(baseDir / ".vscode");
    fs::create_directories(baseDir / "node_modules");
    fs::create_directories(baseDir / "bin");
    fs::create_directories(baseDir / "obj");
    fs::create_directories(baseDir / "build");
    fs::create_directories(baseDir / "build-release");
    fs::create_directories(baseDir / "collection");
    fs::create_directories(baseDir / "valid_code_dir");

    {
        std::ofstream out(baseDir / ".git" / "HEAD");
        out << "ref: refs/heads/main";
        std::ofstream out2(baseDir / "build" / "app.exe");
        out2 << "binary";
        std::ofstream out3(baseDir / "valid_code_dir" / "main.cpp");
        out3 << "int main() {}";
    }

    ProjectScanner scanner(baseDir.string());
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.directories.size() == 1, "Only valid_code_dir should remain");
    TEST_ASSERT(root.directories[0].directoryName == "valid_code_dir", "valid_code_dir mismatch");
    TEST_ASSERT(root.directories[0].files.size() == 1, "Expected 1 file inside valid_code_dir");
    TEST_ASSERT(root.directories[0].files[0].fileName == "main.cpp", "main.cpp mismatch");
}

static void test_scanner_ignored_extensions_pruning() {
    const fs::path baseDir = "test_scratch_retro_transfer/scanner_ignored_exts";
    fs::create_directories(baseDir);

    {
        std::ofstream(baseDir / "test.exe") << "exe";
        std::ofstream(baseDir / "test.o") << "obj";
        std::ofstream(baseDir / "test.dll") << "dll";
        std::ofstream(baseDir / "test.class") << "class";
        std::ofstream(baseDir / "source.cpp") << "cpp";
        std::ofstream(baseDir / "header.h") << "h";
    }

    ProjectScanner scanner(baseDir.string());
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.files.size() == 2, "Expected exactly 2 files (source.cpp and header.h)");
    TEST_ASSERT(root.files[0].fileName == "header.h", "First file should be header.h");
    TEST_ASSERT(root.files[1].fileName == "source.cpp", "Second file should be source.cpp");
}

static void test_scanner_nonexistent_directory() {
    ProjectScanner scanner("test_scratch_retro_transfer/non_existent_directory_12345");
    DirectoryNode root = scanner.scan();

    TEST_ASSERT(root.files.empty(), "Non-existent directory should return 0 files");
    TEST_ASSERT(root.directories.empty(), "Non-existent directory should return 0 subdirectories");
}

#endif // TEST_SCANNER_COVERAGE_HPP
