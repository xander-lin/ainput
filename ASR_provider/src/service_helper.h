#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

namespace ainput {

inline bool portReachable(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    bool ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

inline bool ensureSherpaService(const std::string &serviceName, int port,
                                 int timeoutSec = 10) {
    if (portReachable(port)) {
        fprintf(stderr, "Ainput: %s port %d already up (warm)\n",
                serviceName.c_str(), port);
        return true;
    }

    fprintf(stderr, "Ainput: %s port %d not up, starting via systemctl (cold)...\n",
            serviceName.c_str(), port);

    std::string cmd =
        "systemctl --user --no-block start " + serviceName;
    int ret = system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "Ainput: systemctl start %s returned %d\n",
                serviceName.c_str(), ret);
    }

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (portReachable(port)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            fprintf(stderr, "Ainput: %s ready after cold start\n",
                    serviceName.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fprintf(stderr, "Ainput: %s did not become ready within %ds\n",
            serviceName.c_str(), timeoutSec);
    throw std::runtime_error("sherpa service did not start in time");
}

}  // namespace ainput
