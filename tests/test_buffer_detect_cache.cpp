#include "buffer_detect.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string tempConfigPath() {
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    return std::string(tmp) + "/ainput-pa-buffer-test.json";
}

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool contains(const std::string &s, const std::string &needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    auto path = tempConfigPath();
    {
        std::ofstream f(path);
        f << "{\"buffer_bytes\": 64000}\n";
    }

    setenv("AINPUT_PA_BUFFER_CONFIG", path.c_str(), 1);
    setenv("AINPUT_PA_SOURCE_ID", "alsa_input.usb-CX31993.analog-stereo", 1);

    size_t migrated = ainput::loadOrDetectBufferBytes();
    if (migrated != 64000) {
        std::cerr << "legacy cache was not migrated for first device\n";
        return 1;
    }

    {
        std::ofstream f(path);
        f << "{\n"
          << "  \"devices\": {\n"
          << "    \"alsa_input.usb-CX31993.analog-stereo\": {\"buffer_bytes\": 64000},\n"
          << "    \"alsa_input.pci-0000_00_1f.3.analog-stereo\": {\"buffer_bytes\": 16384}\n"
          << "  }\n"
          << "}\n";
    }

    setenv("AINPUT_PA_SOURCE_ID", "alsa_input.pci-0000_00_1f.3.analog-stereo", 1);
    size_t secondDevice = ainput::loadOrDetectBufferBytes();
    if (secondDevice != 16384) {
        std::cerr << "second device did not use its own cached buffer\n";
        return 1;
    }

    auto content = readFile(path);
    if (!contains(content, "alsa_input.usb-CX31993.analog-stereo") ||
        !contains(content, "alsa_input.pci-0000_00_1f.3.analog-stereo")) {
        std::cerr << "device cache entries were not preserved\n";
        return 1;
    }

    std::remove(path.c_str());
    return 0;
}
