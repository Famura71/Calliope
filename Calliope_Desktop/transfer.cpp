#include "transfer.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <mmreg.h>

#include <chrono>

namespace {

constexpr uint32_t kMagic = 0x31504C43;   // "CLP1"
constexpr uint32_t kSyncMagic = 0x31434E53;  // "SNC1"
constexpr uint16_t kDefaultPort = 4010;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint32_t sequence;
    uint64_t captureTimeUs;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint16_t formatTag;
    uint32_t payloadBytes;
};

struct SyncRequest {
    uint32_t magic;
    uint64_t clientSendTimeUs;
};

struct SyncReply {
    uint32_t magic;
    uint64_t clientSendTimeUs;
    uint64_t serverReceiveTimeUs;
    uint64_t serverSendTimeUs;
};
#pragma pack(pop)

bool sendAll(SOCKET socket, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(SOCKET socket, uint8_t* data, size_t len) {
    size_t received = 0;
    while (received < len) {
        const int n = recv(socket, reinterpret_cast<char*>(data + received), static_cast<int>(len - received), 0);
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

uint64_t monotonicNowUs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

TransferServer::TransferServer(LogCallback logger)
    : logger_(std::move(logger)) {}

TransferServer::~TransferServer() {
    stop();
}

bool TransferServer::start() {
    if (running_.load()) {
        return true;
    }
    if (!initWinsock()) {
        logger_("WSAStartup failed");
        return false;
    }
    if (!startServer()) {
        logger_("Server startup failed");
        WSACleanup();
        return false;
    }

    running_.store(true);
    acceptThread_ = std::thread(&TransferServer::acceptLoop, this);
    logger_("Transfer server started");
    return true;
}

void TransferServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (clientSocket_ != INVALID_SOCKET) {
            closesocket(clientSocket_);
            clientSocket_ = INVALID_SOCKET;
        }
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (controlThread_.joinable()) {
        controlThread_.join();
    }

    WSACleanup();
    clientReady_.store(false);
    sequence_ = 0;
    logger_("Transfer server stopped");
}

void TransferServer::sendAudioFrame(const AudioFrame& frame) {
    PacketHeader header{};
    header.magic = kMagic;
    header.sequence = sequence_++;
    header.captureTimeUs = frame.captureTimeUs;
    header.sampleRate = frame.sampleRate;
    header.channels = frame.channels;
    header.bitsPerSample = frame.bitsPerSample;
    header.formatTag = frame.formatTag;
    header.payloadBytes = static_cast<uint32_t>(frame.payload.size());

    std::lock_guard<std::mutex> lock(socketMutex_);
    if (clientSocket_ == INVALID_SOCKET || !clientReady_.load()) {
        return;
    }

    const bool okHeader = sendAll(clientSocket_, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    const bool okPayload = okHeader && sendAll(clientSocket_, frame.payload.data(), frame.payload.size());
    if (!okPayload) {
        logger_("Client disconnected during send");
        closesocket(clientSocket_);
        clientSocket_ = INVALID_SOCKET;
        clientReady_.store(false);
    }
}

bool TransferServer::initWinsock() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        logger_("WSAStartup error code: " + std::to_string(result));
        return false;
    }
    return true;
}

bool TransferServer::startServer() {
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        logger_("socket() failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kDefaultPort);

    const int opt = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        logger_("bind() failed on port " + std::to_string(kDefaultPort) + ": " + std::to_string(WSAGetLastError()));
        return false;
    }
    if (listen(listenSocket_, 1) != 0) {
        logger_("listen() failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    logger_("Listening on TCP port " + std::to_string(kDefaultPort));
    return true;
}

void TransferServer::acceptLoop() {
    while (running_.load()) {
        fd_set readSet{};
        FD_ZERO(&readSet);
        FD_SET(listenSocket_, &readSet);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }

        SOCKET accepted = accept(listenSocket_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            if (running_.load()) {
                logger_("accept() failed: " + std::to_string(WSAGetLastError()));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            if (clientSocket_ != INVALID_SOCKET) {
                closesocket(clientSocket_);
            }
            clientSocket_ = accepted;
            const BOOL noDelay = TRUE;
            setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        }

        clientReady_.store(false);
        sequence_ = 0;
        if (controlThread_.joinable()) {
            controlThread_.join();
        }
        controlThread_ = std::thread(&TransferServer::controlLoop, this, accepted);
        logger_("Client connected");
    }
}

void TransferServer::controlLoop(SOCKET socket) {
    while (running_.load()) {
        SyncRequest request{};
        if (!recvAll(socket, reinterpret_cast<uint8_t*>(&request), sizeof(request))) {
            break;
        }
        if (request.magic != kSyncMagic) {
            logger_("Unknown control packet received");
            break;
        }

        SyncReply reply{};
        reply.magic = kSyncMagic;
        reply.clientSendTimeUs = request.clientSendTimeUs;
        reply.serverReceiveTimeUs = monotonicNowUs();
        reply.serverSendTimeUs = monotonicNowUs();

        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            if (clientSocket_ != socket ||
                !sendAll(socket, reinterpret_cast<const uint8_t*>(&reply), sizeof(reply))) {
                break;
            }
        }

        if (!clientReady_.exchange(true)) {
            logger_("Client sync completed, enabling audio stream");
        }
    }

    std::lock_guard<std::mutex> lock(socketMutex_);
    if (clientSocket_ == socket) {
        closesocket(clientSocket_);
        clientSocket_ = INVALID_SOCKET;
    }
    clientReady_.store(false);
    logger_("Client control loop ended");
}
