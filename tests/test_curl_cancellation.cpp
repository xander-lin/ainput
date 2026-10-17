#include "ainput_config.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

size_t discardResponse(char *, size_t size, size_t count, void *) {
    return size * count;
}

} // namespace

int main() {
    {
        CURL *before = ainput::getCurl();
        if (!before) {
            std::cerr << "getCurl returned nullptr\n";
            return 1;
        }
        void *sentinel = reinterpret_cast<void *>(0x1);
        curl_easy_setopt(before, CURLOPT_PRIVATE, sentinel);
        ainput::evictCurlHandle();
        CURL *after = ainput::getCurl();
        if (!after) {
            std::cerr << "getCurl returned nullptr after evict\n";
            return 1;
        }
        void *privateValue = nullptr;
        curl_easy_getinfo(after, CURLINFO_PRIVATE, &privateValue);
        if (privateValue == sentinel) {
            std::cerr << "evictCurlHandle did not recreate the handle\n";
            return 1;
        }
        CURLcode check = curl_easy_setopt(after, CURLOPT_URL,
                                          "https://example.com/");
        if (check != CURLE_OK) {
            std::cerr << "recreated handle rejected CURLOPT_URL: "
                      << static_cast<int>(check) << "\n";
            return 1;
        }
        curl_easy_reset(after);
    }

    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        std::cerr << "failed to create local test socket\n";
        return 1;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(listener, 1) < 0) {
        close(listener);
        std::cerr << "failed to bind local test socket\n";
        return 1;
    }

    socklen_t addressSize = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &addressSize) < 0) {
        close(listener);
        std::cerr << "failed to read local test socket address\n";
        return 1;
    }

    std::atomic_bool serverReady = false;
    std::thread server([&] {
        int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;
        serverReady.store(true);
        char request[1024];
        (void)read(client, request, sizeof(request));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        close(client);
    });

    CURL *curl = curl_easy_init();
    if (!curl) {
        close(listener);
        server.join();
        std::cerr << "failed to initialize curl\n";
        return 1;
    }

    auto cancel = std::make_shared<std::atomic_bool>(false);
    std::string url = "http://127.0.0.1:" +
                      std::to_string(ntohs(address.sin_port)) + "/hang";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode result = CURLE_OK;
    auto start = std::chrono::steady_clock::now();
    {
        ainput::CurlCancellationScope cancellation(curl, cancel);
        std::thread request([&] { result = curl_easy_perform(curl); });
        while (!serverReady.load() &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cancel->store(true);
        request.join();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    curl_easy_cleanup(curl);
    close(listener);
    server.join();

    if (result != CURLE_ABORTED_BY_CALLBACK) {
        std::cerr << "curl cancellation returned " << static_cast<int>(result)
                  << " instead of CURLE_ABORTED_BY_CALLBACK\n";
        return 1;
    }
    if (elapsed > std::chrono::seconds(2)) {
        std::cerr << "curl cancellation exceeded two seconds\n";
        return 1;
    }
    return 0;
}
