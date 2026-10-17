#include "diagnostic_log.h"
#include "sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <string>

namespace {

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    if (ainput::sha256::hashHex("") !=
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        std::cerr << "SHA-256 empty-string vector mismatch\n";
        return 1;
    }
    if (ainput::sha256::hashHex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "SHA-256 abc vector mismatch\n";
        return 1;
    }
    if (ainput::sha256::hashHex("The quick brown fox jumps over the lazy dog") !=
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592") {
        std::cerr << "SHA-256 fox vector mismatch\n";
        return 1;
    }
    if (ainput::hashDiagnosticValue("diagnostic-secret") !=
        "de5a55536fc8350ce2c60389ad2ae99807bc8ecdbf940c05816e9b72ca14c9bf") {
        std::cerr << "SHA-256 diagnostic hash mismatch\n";
        return 1;
    }

    ainput::MemoryDiagnosticLog memory;
    memory.event("test", "sample", {{"value_hash", "abc"}, {"length", "4"}});
    const auto lines = memory.lines();
    if (lines.size() != 1 || !contains(lines.front(), "\"event\":\"sample\"") ||
        !contains(lines.front(), "\"value_hash\":\"abc\"")) {
        std::cerr << "memory diagnostic log did not preserve JSON fields\n";
        return 1;
    }
    if (contains(lines.front(), "diagnostic-secret")) {
        std::cerr << "diagnostic log contained a raw sensitive value\n";
        return 1;
    }

    const auto root = std::filesystem::temp_directory_path() / "ainput-diagnostic-log-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto path = root / "diagnostic.log";
    ainput::FileDiagnosticLog file(path.string(), 256);
    file.event("test", "first", {{"value_hash", "abc"}});
    file.event("test", "second", {{"value_hash", "def"}});
    file.event("test", "third", {{"value_hash", "ghi"}});

    const auto size = std::filesystem::file_size(path);
    struct stat statBuffer {};
    if (stat(path.c_str(), &statBuffer) != 0 ||
        (statBuffer.st_mode & 0777) != 0600) {
        std::cerr << "diagnostic log permissions are not private\n";
        return 1;
    }
    if (size > 256 || !file.stopped()) {
        std::cerr << "diagnostic log exceeded its configured limit\n";
        return 1;
    }
    const auto before = size;
    file.event("test", "after_limit");
    if (std::filesystem::file_size(path) != before) {
        std::cerr << "diagnostic log continued after reaching its limit\n";
        return 1;
    }

    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output << std::string(256 - std::filesystem::file_size(path), 'x');
    }
    ainput::FileDiagnosticLog existing(path.string(), 256);
    existing.event("test", "must_not_write");
    if (!existing.stopped() || std::filesystem::file_size(path) != 256) {
        std::cerr << "pre-existing full diagnostic log was not stopped\n";
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
