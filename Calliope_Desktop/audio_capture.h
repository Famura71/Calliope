#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct AudioFrame {
    uint64_t captureTimeUs = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t formatTag = 0;
    std::vector<uint8_t> payload;
};

class AudioCaptureService {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using FrameCallback = std::function<void(AudioFrame)>;

    AudioCaptureService(LogCallback logger, FrameCallback onFrame);
    ~AudioCaptureService();

    bool start();
    void stop();

private:
    void captureLoop();

    LogCallback logger_;
    FrameCallback onFrame_;
    bool running_ = false;
    std::thread captureThread_;
};
