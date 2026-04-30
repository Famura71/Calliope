#pragma once

#include "audio_capture.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class TransferServer {
public:
    using LogCallback = std::function<void(const std::string&)>;

    explicit TransferServer(LogCallback logger);
    ~TransferServer();

    bool start();
    void stop();
    void sendAudioFrame(const AudioFrame& frame);

private:
    bool initWinsock();
    bool startServer();
    void acceptLoop();
    void controlLoop(SOCKET socket);

    LogCallback logger_;
    std::atomic<bool> running_{false};
    std::atomic<bool> clientReady_{false};
    SOCKET listenSocket_{INVALID_SOCKET};
    SOCKET clientSocket_{INVALID_SOCKET};
    std::mutex socketMutex_;
    std::thread acceptThread_;
    std::thread controlThread_;
    uint32_t sequence_ = 0;
};
