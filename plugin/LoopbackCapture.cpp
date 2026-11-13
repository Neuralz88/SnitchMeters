#include "LoopbackCapture.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IMMDevice> defaultOutput() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    return device;
}

juce::String idOf(IMMDevice* device) {
    juce::String id;
    LPWSTR raw = nullptr;
    if (device != nullptr && SUCCEEDED(device->GetId(&raw)) && raw != nullptr) { id = juce::String(raw); CoTaskMemFree(raw); }
    return id;
}

juce::String friendlyName(IMMDevice* device) {
    juce::String result;
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) result = juce::String(value.pwszVal);
        PropVariantClear(&value);
    }
    return result;
}

enum class SampleKind { float32, int16, int24, int32, unsupported };

SampleKind kindOf(const WAVEFORMATEX* f) {
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        isPcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
    }
    if (isFloat && f->wBitsPerSample == 32) return SampleKind::float32;
    if (isPcm && f->wBitsPerSample == 16) return SampleKind::int16;
    if (isPcm && f->wBitsPerSample == 24) return SampleKind::int24;
    if (isPcm && f->wBitsPerSample == 32) return SampleKind::int32;
    return SampleKind::unsupported;
}

float sampleAt(const BYTE* p, SampleKind kind) {
    switch (kind) {
        case SampleKind::float32: { float v; memcpy(&v, p, 4); return v; }
        case SampleKind::int16:   { int16_t v; memcpy(&v, p, 2); return (float) v / 32768.0f; }
        case SampleKind::int24:   return (float) ((int32_t) (((uint32_t) p[0] << 8) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 24)) >> 8) / 8388608.0f;
        case SampleKind::int32:   { int32_t v; memcpy(&v, p, 4); return (float) ((double) v / 2147483648.0); }
        case SampleKind::unsupported: break;
    }
    return 0.0f;
}

} // namespace

LoopbackCapture::LoopbackCapture(Callback onAudio) : juce::Thread("Snitch loopback"), callback(std::move(onAudio)) {}
LoopbackCapture::~LoopbackCapture() { stop(); }

void LoopbackCapture::start() { startThread(juce::Thread::Priority::high); }
void LoopbackCapture::stop()  { stopThread(3000); }

juce::String LoopbackCapture::deviceName() const {
    const juce::ScopedLock lock(nameLock);
    return name;
}

void LoopbackCapture::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (! threadShouldExit()) {
        session();
        { const juce::ScopedLock lock(nameLock); name = {}; }
        wait(500);   // no device, or it went away: try the current default again
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

// One open-capture-until-something-fails cycle.
void LoopbackCapture::session() {
    ComPtr<IMMDevice> device = defaultOutput();
    if (device == nullptr) return;
    const juce::String deviceId = idOf(device.Get());

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())))) return;

    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format)) || format == nullptr) return;
    const SampleKind kind = kindOf(format);
    const int channels = format->nChannels, frameBytes = format->nBlockAlign, sampleBytes = format->wBitsPerSample / 8;
    const double sampleRate = (double) format->nSamplesPerSec;
    const HRESULT initialised = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000 /* 200 ms */, 0, format, nullptr);
    CoTaskMemFree(format);
    if (FAILED(initialised) || kind == SampleKind::unsupported || channels < 1 || sampleRate <= 0.0) return;

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) return;
    { const juce::ScopedLock lock(nameLock); name = friendlyName(device.Get()); }

    std::vector<float> left, right;
    const int silenceBlock = juce::jmax(1, (int) (sampleRate * 0.01));
    juce::uint32 lastData = juce::Time::getMillisecondCounter(), lastDeviceCheck = lastData;
    bool ok = true;

    while (ok && ! threadShouldExit()) {
        wait(10);
        const juce::uint32 now = juce::Time::getMillisecondCounter();

        UINT32 packet = 0;
        while (ok && SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 count = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) { ok = false; break; }
            left.resize(count); right.resize(count);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
            for (UINT32 i = 0; i < count; ++i) {
                const BYTE* frame = data + (size_t) i * (size_t) frameBytes;
                left[i]  = silent ? 0.0f : sampleAt(frame, kind);
                right[i] = silent ? 0.0f : (channels > 1 ? sampleAt(frame + sampleBytes, kind) : left[i]);
            }
            capture->ReleaseBuffer(count);
            if (count > 0) {
                callback(left.data(), right.data(), (int) count, sampleRate);
                frames += count;
                lastData = now;
            }
        }
        if (FAILED(capture->GetNextPacketSize(&packet))) ok = false;   // device invalidated

        // Nothing playing means no packets at all: feed silence so the meters fall back.
        if (ok && now - lastData > 100) {
            left.assign((size_t) silenceBlock, 0.0f); right.assign((size_t) silenceBlock, 0.0f);
            callback(left.data(), right.data(), silenceBlock, sampleRate);
        }

        if (ok && now - lastDeviceCheck > 2000) {
            lastDeviceCheck = now;
            ComPtr<IMMDevice> current = defaultOutput();
            if (current == nullptr || idOf(current.Get()) != deviceId) ok = false;   // default output changed
        }
    }
    client->Stop();
}

#else

LoopbackCapture::LoopbackCapture(Callback onAudio) : juce::Thread("Snitch loopback"), callback(std::move(onAudio)) {}
LoopbackCapture::~LoopbackCapture() {}
void LoopbackCapture::start() {}
void LoopbackCapture::stop() {}
juce::String LoopbackCapture::deviceName() const { return {}; }
void LoopbackCapture::run() {}
void LoopbackCapture::session() {}

#endif
