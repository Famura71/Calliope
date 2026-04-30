#include "audio_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kCaptureWaitTimeoutMs = 1000;

bool isWaveFormatExtensible(const WAVEFORMATEX* format) {
    return format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22;
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (!isWaveFormatExtensible(format)) {
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

bool isPcmFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (!isWaveFormatExtensible(format)) {
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
}

int16_t clampToInt16(int sample) {
    return static_cast<int16_t>(std::clamp(sample, -32768, 32767));
}

std::vector<uint8_t> convertPayloadToPcm16(const BYTE* data, UINT32 frames, const WAVEFORMATEX* format) {
    const int channels = static_cast<int>(format->nChannels);
    const int bitsPerSample = static_cast<int>(format->wBitsPerSample);
    std::vector<uint8_t> converted(static_cast<size_t>(frames) * channels * sizeof(int16_t), 0);
    auto* out = reinterpret_cast<int16_t*>(converted.data());
    const size_t sampleCount = static_cast<size_t>(frames) * channels;

    if (isFloatFormat(format) && bitsPerSample == 32) {
        const auto* in = reinterpret_cast<const float*>(data);
        for (size_t i = 0; i < sampleCount; ++i) {
            const float value = std::clamp(in[i], -1.0f, 1.0f);
            out[i] = clampToInt16(static_cast<int>(value * 32767.0f));
        }
        return converted;
    }

    if (isPcmFormat(format) && bitsPerSample == 16) {
        std::memcpy(converted.data(), data, converted.size());
        return converted;
    }

    if (isPcmFormat(format) && bitsPerSample == 32) {
        const auto* in = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < sampleCount; ++i) {
            out[i] = clampToInt16(in[i] >> 16);
        }
        return converted;
    }

    if (isPcmFormat(format) && bitsPerSample == 24) {
        for (size_t i = 0; i < sampleCount; ++i) {
            const size_t base = i * 3;
            int32_t sample = static_cast<int32_t>(data[base]) |
                             (static_cast<int32_t>(data[base + 1]) << 8) |
                             (static_cast<int32_t>(data[base + 2]) << 16);
            if (sample & 0x00800000) {
                sample |= ~0x00FFFFFF;
            }
            out[i] = clampToInt16(sample >> 8);
        }
        return converted;
    }

    if (isPcmFormat(format) && bitsPerSample == 8) {
        for (size_t i = 0; i < sampleCount; ++i) {
            out[i] = static_cast<int16_t>((static_cast<int>(data[i]) - 128) << 8);
        }
        return converted;
    }

    return converted;
}

uint64_t monotonicNowUs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

AudioCaptureService::AudioCaptureService(LogCallback logger, FrameCallback onFrame)
    : logger_(std::move(logger)), onFrame_(std::move(onFrame)) {}

AudioCaptureService::~AudioCaptureService() {
    stop();
}

bool AudioCaptureService::start() {
    if (running_) {
        return true;
    }
    running_ = true;
    captureThread_ = std::thread(&AudioCaptureService::captureLoop, this);
    return true;
}

void AudioCaptureService::stop() {
    running_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

void AudioCaptureService::captureLoop() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        logger_("CoInitializeEx failed: " + std::to_string(static_cast<unsigned long>(comResult)));
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        logger_("CoCreateInstance(MMDeviceEnumerator) failed");
        CoUninitialize();
        return;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        logger_("GetDefaultAudioEndpoint failed");
        CoUninitialize();
        return;
    }

    ComPtr<IAudioClient> audioClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf())))) {
        logger_("IMMDevice::Activate(IAudioClient) failed");
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* waveFormat = nullptr;
    if (FAILED(audioClient->GetMixFormat(&waveFormat)) || waveFormat == nullptr) {
        logger_("IAudioClient::GetMixFormat failed");
        CoUninitialize();
        return;
    }

    logger_(
        "Mix format: tag=" + std::to_string(waveFormat->wFormatTag) +
        " channels=" + std::to_string(waveFormat->nChannels) +
        " rate=" + std::to_string(waveFormat->nSamplesPerSec) +
        " bits=" + std::to_string(waveFormat->wBitsPerSample));

    REFERENCE_TIME defaultDevicePeriod = 0;
    REFERENCE_TIME minimumDevicePeriod = 0;
    if (FAILED(audioClient->GetDevicePeriod(&defaultDevicePeriod, &minimumDevicePeriod))) {
        logger_("IAudioClient::GetDevicePeriod failed");
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent) {
        logger_("CreateEventW failed");
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    const REFERENCE_TIME bufferDuration = defaultDevicePeriod;
    const HRESULT initHr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufferDuration,
        0,
        waveFormat,
        nullptr);
    if (FAILED(initHr)) {
        logger_("IAudioClient::Initialize failed: " + std::to_string(static_cast<unsigned long>(initHr)));
        CloseHandle(captureEvent);
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    if (FAILED(audioClient->SetEventHandle(captureEvent))) {
        logger_("IAudioClient::SetEventHandle failed");
        CloseHandle(captureEvent);
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    if (FAILED(audioClient->GetService(IID_PPV_ARGS(&captureClient)))) {
        logger_("IAudioClient::GetService(IAudioCaptureClient) failed");
        CloseHandle(captureEvent);
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    if (FAILED(audioClient->Start())) {
        logger_("IAudioClient::Start failed");
        CloseHandle(captureEvent);
        CoTaskMemFree(waveFormat);
        CoUninitialize();
        return;
    }

    logger_(
        "Loopback capture started with defaultPeriod=" + std::to_string(defaultDevicePeriod) +
        " minPeriod=" + std::to_string(minimumDevicePeriod));

    while (running_) {
        const DWORD waitResult = WaitForSingleObject(captureEvent, kCaptureWaitTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            continue;
        }

        UINT32 packetLength = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
            continue;
        }

        while (packetLength > 0 && running_) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }

            const size_t bytes = static_cast<size_t>(frames) * waveFormat->nBlockAlign;
            std::vector<uint8_t> payload(bytes, 0);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                std::memcpy(payload.data(), data, bytes);
            }

            AudioFrame frame;
            frame.captureTimeUs = monotonicNowUs();
            frame.sampleRate = static_cast<uint32_t>(waveFormat->nSamplesPerSec);
            frame.channels = waveFormat->nChannels;
            frame.bitsPerSample = 16;
            frame.formatTag = WAVE_FORMAT_PCM;
            frame.payload = convertPayloadToPcm16(payload.data(), frames, waveFormat);
            onFrame_(std::move(frame));

            captureClient->ReleaseBuffer(frames);
            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                packetLength = 0;
            }
        }
    }

    audioClient->Stop();
    CloseHandle(captureEvent);
    CoTaskMemFree(waveFormat);
    CoUninitialize();
}
