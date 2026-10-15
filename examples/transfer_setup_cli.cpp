#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static bool readLine(const std::string& prompt, std::string& value) {
    std::cout << prompt;
    return static_cast<bool>(std::getline(std::cin, value));
}

static std::string readName(const std::string& prompt,
                            const std::string& defaultName) {
    std::string value;
    if (!readLine(prompt + " [" + defaultName + "]: ", value)) return {};
    return value.empty() ? defaultName : value;
}

#ifdef _WIN32
static std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1,
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, result.data(), length);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

static std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += L'"';
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted += character;
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

static bool runRetroTransfer(const std::vector<std::string>& arguments) {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        std::cerr << "Could not locate this UI executable.\n";
        return false;
    }
    executable.resize(length);

    fs::path cliPath = fs::path(executable).parent_path() / L"RetroTransfer.exe";
    if (!fs::exists(cliPath)) {
        std::cerr << "RetroTransfer.exe was not found beside this UI:\n"
                  << cliPath.string() << "\nBuild both application targets first.\n";
        return false;
    }

    std::wstring commandLine = quoteWindowsArgument(cliPath.wstring());
    for (const std::string& argument : arguments) {
        const std::wstring wideArgument = widen(argument);
        if (wideArgument.empty() && !argument.empty()) {
            std::cerr << "Could not convert an input path or filename.\n";
            return false;
        }
        commandLine += L" " + quoteWindowsArgument(wideArgument);
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(cliPath.c_str(), commandLine.data(), nullptr, nullptr,
                        TRUE, 0, nullptr, nullptr, &startupInfo, &processInfo)) {
        std::cerr << "Could not start RetroTransfer.exe (Windows error "
                  << GetLastError() << ").\n";
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    const BOOL gotExitCode = GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    if (!gotExitCode) {
        std::cerr << "Could not read RetroTransfer.exe's exit status.\n";
        return false;
    }
    if (exitCode != 0) {
        std::cerr << "RetroTransfer.exe failed with exit code " << exitCode << ".\n";
        return false;
    }
    return true;
}
#else
static bool runRetroTransfer(const std::vector<std::string>& arguments) {
    std::error_code pathError;
    const fs::path executable = fs::canonical("/proc/self/exe", pathError);
    if (pathError) {
        std::cerr << "Could not locate this UI executable: "
                  << pathError.message() << "\n";
        return false;
    }
    const fs::path cliPath = executable.parent_path() / "RetroTransfer";
    if (!fs::exists(cliPath)) {
        std::cerr << "RetroTransfer was not found beside this UI. "
                     "Build both application targets first.\n";
        return false;
    }

    std::vector<std::string> command;
    command.reserve(arguments.size() + 1);
    command.push_back(cliPath.string());
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (std::string& argument : command) argv.push_back(argument.data());
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "Could not start RetroTransfer.\n";
        return false;
    }
    if (child == 0) {
        execv(cliPath.c_str(), argv.data());
        std::cerr << "Could not execute RetroTransfer.\n";
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::cerr << "Could not wait for RetroTransfer to finish.\n";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "RetroTransfer failed";
        if (WIFEXITED(status)) std::cerr << " with exit code " << WEXITSTATUS(status);
        std::cerr << ".\n";
        return false;
    }
    return true;
}
#endif

static bool askOutputNames(bool includePayload,
                           std::vector<std::string>& arguments) {
    const std::string scanName = readName("Scan JSON filename", "scan.json");
    if (scanName.empty()) return false;
    const std::string chunksName =
        readName("Chunk manifest filename", "scan_chunks.json");
    if (chunksName.empty()) return false;
    arguments.push_back("--scan-output");
    arguments.push_back(scanName);
    arguments.push_back("--chunks-output");
    arguments.push_back(chunksName);
    if (includePayload) {
        const std::string payloadName =
            readName("Compressed payload filename", "missing-chunks.rtpack");
        if (payloadName.empty()) return false;
        arguments.push_back("--payload-output");
        arguments.push_back(payloadName);
    }
    return true;
}

static std::string unquotePath(std::string path) {
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

static void showMenu() {
    std::cout << "\n============================================\n"
                 "          RETROTRANSFER PROJECT TOOL\n"
                 "============================================\n"
                 "  1. Scan and chunk a project\n"
                 "  2. Scan, chunk, and selectively compress\n"
                 "  3. Exit\n"
                 "============================================\n"
                 "  Choose an option: ";
}

static bool runScanAndChunk() {
    std::string projectPath;
    if (!readLine("Project folder path: ", projectPath)) return false;
    projectPath = unquotePath(std::move(projectPath));
    if (projectPath.empty()) {
        std::cout << "Project path cannot be empty.\n";
        return false;
    }

    std::vector<std::string> arguments{projectPath};
    if (!askOutputNames(false, arguments)) {
        std::cout << "Could not read output filenames.\n";
        return false;
    }
    std::cout << "\nStarting scan and chunking. Output folder: "
              << fs::current_path() / "collection" << "\n\n";
    const bool success = runRetroTransfer(arguments);
    std::cout << (success ? "Scan and chunking completed.\n"
                          : "Scan and chunking did not complete successfully.\n");
    return success;
}

static bool runSelectiveCompression() {
    std::string projectPath;
    std::string baselinePath;
    std::string useBaseline;
    if (!readLine("Current project folder path: ", projectPath) ||
        !readLine("Compare with an older project? (y/n) [n]: ", useBaseline)) {
        return false;
    }
    projectPath = unquotePath(std::move(projectPath));
    if (projectPath.empty()) {
        std::cout << "Project path is required.\n";
        return false;
    }

    std::vector<std::string> arguments{projectPath};
    if (useBaseline == "y" || useBaseline == "Y") {
        if (!readLine("Older project folder path: ", baselinePath)) return false;
        baselinePath = unquotePath(std::move(baselinePath));
        if (baselinePath.empty()) {
            std::cout << "Older project path cannot be empty when comparison is selected.\n";
            return false;
        }
        arguments.push_back("--baseline");
        arguments.push_back(baselinePath);
    } else if (useBaseline.empty() || useBaseline == "n" || useBaseline == "N") {
        arguments.push_back("--compress-all");
    } else {
        std::cout << "Enter y or n to choose whether to use an older project.\n";
        return false;
    }

    if (!askOutputNames(true, arguments)) {
        std::cout << "Could not read output filenames.\n";
        return false;
    }
    std::cout << "\nStarting "
              << (baselinePath.empty() ? "full-project compression"
                                       : "baseline comparison and selective compression")
              << ". Output folder: " << fs::current_path() / "collection" << "\n\n";
    const bool success = runRetroTransfer(arguments);
    std::cout << (success ? "Selective compression completed.\n"
                          : "Selective compression did not complete successfully.\n");
    return success;
}

int main() {
    std::string input;
    while (true) {
        showMenu();
        if (!std::getline(std::cin, input)) {
            std::cout << "\nInput ended; exiting.\n";
            return 0;
        }

        if (input == "1") {
            runScanAndChunk();
        } else if (input == "2") {
            runSelectiveCompression();
        } else if (input == "3") {
            std::cout << "Exiting RetroTransfer.\n";
            return 0;
        } else {
            std::cout << "Invalid choice. Enter 1, 2, or 3.\n";
        }
    }
}
