#pragma once

// Minimal WebSocket client over raw POSIX sockets.
// Sends one binary message, receives one text message.
// No dependencies beyond sys/socket.

#include <string>
#include <vector>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace ainput {
namespace ws {

inline std::string sendBinaryRecvText(int port, const uint8_t *wavData, size_t wavLen) {
    // Parse WAV header to extract PCM data
    if (wavLen < 44) throw std::runtime_error("WAV too short");

    int sampleRate = *(const int32_t*)(wavData + 24);
    int numSamples = (int)((wavLen - 44) / 2);  // int16 samples
    int floatBytes = numSamples * (int)sizeof(float);

    // Build protocol payload: [int32 sr] [int32 float_byte_count] [float PCM...]
    std::vector<uint8_t> payload(8 + floatBytes);
    memcpy(payload.data(), &sampleRate, 4);
    memcpy(payload.data() + 4, &floatBytes, 4);
    auto *samples = (const int16_t*)(wavData + 44);
    auto *floats = (float*)(payload.data() + 8);
    for (int i = 0; i < numSamples; i++)
        floats[i] = (float)samples[i] / 32768.0f;
    // 1. TCP connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect() failed");
    }

    // 2. WebSocket upgrade handshake
    unsigned char keyBytes[16];
    for (int i = 0; i < 16; i++) keyBytes[i] = (unsigned char)(rand() % 256);

    // base64 encode the key
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char keyB64[25] = {};
    for (int i = 0; i < 16; i += 3) {
        unsigned v = ((unsigned)keyBytes[i] << 16) | ((i+1<16 ? (unsigned)keyBytes[i+1] : 0) << 8) | (i+2<16 ? (unsigned)keyBytes[i+2] : 0);
        keyB64[i/3*4]   = b64[(v >> 18) & 63];
        keyB64[i/3*4+1] = b64[(v >> 12) & 63];
        keyB64[i/3*4+2] = (i+1 < 16) ? b64[(v >> 6) & 63] : '=';
        keyB64[i/3*4+3] = (i+2 < 16) ? b64[v & 63] : '=';
    }

    char req[512];
    int reqLen = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n", port, keyB64);

    if (write(fd, req, (size_t)reqLen) != reqLen) {
        fprintf(stderr, "Ainput ws: handshake write failed, errno=%d\n", errno);
        close(fd);
        throw std::runtime_error("ws handshake write failed");
    }

    // Read handshake response (in a loop until \r\n\r\n)
    std::string handshake;
    char resp[4096];
    while (handshake.find("\r\n\r\n") == std::string::npos) {
        int n = (int)read(fd, resp, sizeof(resp) - 1);
        if (n <= 0) {
            fprintf(stderr, "Ainput ws: handshake read returned %d, errno=%d (%s)\n",
                    n, errno, strerror(errno));
            close(fd);
            throw std::runtime_error("ws handshake read failed");
        }
        resp[n] = '\0';
        handshake += resp;
    }
    if (!strstr(handshake.c_str(), "101")) {
        fprintf(stderr, "Ainput ws: handshake response: %s\n", handshake.c_str());
        close(fd);
        throw std::runtime_error("ws handshake not 101");
    }

    // 3. Send binary frame (masked) — protocol payload, not raw WAV
    // Frame: [FIN|opcode(2)] [MASK|len] [4 mask bytes] [payload XOR mask]
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() % 256);

    std::vector<unsigned char> frame;
    frame.push_back(0x82); // FIN + BINARY
    size_t pl = payload.size();

    if (pl < 126) {
        frame.push_back((unsigned char)(0x80 | pl));
    } else if (pl < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((unsigned char)((pl >> 8) & 0xFF));
        frame.push_back((unsigned char)(pl & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((unsigned char)((pl >> (i * 8)) & 0xFF));
    }

    for (int i = 0; i < 4; i++) frame.push_back(mask[i]);

    for (size_t i = 0; i < pl; i++)
        frame.push_back(payload[i] ^ mask[i % 4]);

    if (write(fd, frame.data(), frame.size()) != (ssize_t)frame.size()) {
        close(fd);
        throw std::runtime_error("ws send failed");
    }

    // 4. Receive text frame
    std::string result;
    unsigned char header[2];
    while (true) {
        if (read(fd, header, 2) != 2) break;
        bool fin = (header[0] & 0x80) != 0;
        int opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        size_t payloadLen = header[1] & 0x7F;

        if (payloadLen == 126) {
            unsigned char ext[2];
            if (read(fd, ext, 2) != 2) break;
            payloadLen = ((size_t)ext[0] << 8) | ext[1];
        } else if (payloadLen == 127) {
            unsigned char ext[8];
            if (read(fd, ext, 8) != 8) break;
            payloadLen = 0;
            for (int i = 0; i < 8; i++)
                payloadLen = (payloadLen << 8) | ext[i];
        }

        unsigned char rmask[4] = {};
        if (masked) {
            if (read(fd, rmask, 4) != 4) break;
        }

        std::vector<unsigned char> payload(payloadLen);
        size_t total = 0;
        while (total < payloadLen) {
            ssize_t r = read(fd, payload.data() + total, payloadLen - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        if (total < payloadLen) break;

        if (masked) {
            for (size_t i = 0; i < payloadLen; i++)
                payload[i] ^= rmask[i % 4];
        }

        if (opcode == 0x01 || opcode == 0x00) { // TEXT or continuation
            result.append((const char*)payload.data(), payloadLen);
        }

        if (fin) break;
    }

    close(fd);
    return result;
}

} // namespace ws
} // namespace ainput
