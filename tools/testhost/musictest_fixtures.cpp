// Fixture generators: WAV written by hand, FLAC / MP3 / AAC through Windows' own Media Foundation
// encoders (the Sink Writer). Every fixture is our own content: a sine at a known frequency and
// amplitude, so a test can tell tracks apart and check that real audio came out at the right speed.
#include "musictest.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cmath>

namespace mt {

namespace {

constexpr double kTwoPi = 6.283185307179586;

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

int16_t SineSample(double frequency, uint32_t rate, uint64_t index) {
    const double v = kAmplitude * std::sin(kTwoPi * frequency * static_cast<double>(index) / rate);
    return static_cast<int16_t>(std::lround(v * 32767.0));
}

}  // namespace

bool WriteWav(const std::wstring& path, uint32_t rate, uint32_t channels, uint32_t frames, double frequency, uint64_t startSample) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const uint32_t dataBytes = frames * channels * 2;
#pragma pack(push, 1)
    struct Header {
        char riff[4];
        uint32_t size;
        char wave[4];
        char fmt[4];
        uint32_t fmtSize;
        uint16_t tag;
        uint16_t channels;
        uint32_t rate;
        uint32_t byteRate;
        uint16_t align;
        uint16_t bits;
        char data[4];
        uint32_t dataSize;
    } header = {{'R', 'I', 'F', 'F'}, 36 + dataBytes, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16, 1,
                static_cast<uint16_t>(channels), rate, rate * channels * 2, static_cast<uint16_t>(channels * 2), 16,
                {'d', 'a', 't', 'a'}, dataBytes};
#pragma pack(pop)
    std::vector<int16_t> body(static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t v = SineSample(frequency, rate, startSample + i);
        for (uint32_t c = 0; c < channels; ++c) body[static_cast<size_t>(i) * channels + c] = v;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header);
    ok = ok && WriteFile(file, body.data(), dataBytes, &written, nullptr) && written == dataBytes;
    CloseHandle(file);
    return ok;
}

long EncodeWithMediaFoundation(const std::wstring& path, const wchar_t* format, uint32_t rate, double seconds, double frequency) {
    GUID subtype = MFAudioFormat_FLAC;
    UINT32 bits = 16;
    UINT32 avgBytes = 0;
    if (lstrcmpiW(format, L"mp3") == 0) {
        subtype = MFAudioFormat_MP3;
        bits = 0;
        avgBytes = 24000;  // 192 kbit/s
    } else if (lstrcmpiW(format, L"aac") == 0) {
        subtype = MFAudioFormat_AAC;
        avgBytes = 24000;
    }
    constexpr UINT32 kChannels = 2;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* outType = nullptr;
    IMFMediaType* inType = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (SUCCEEDED(hr) && bits) hr = outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    if (SUCCEEDED(hr) && avgBytes) hr = outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
    if (SUCCEEDED(hr)) hr = writer->AddStream(outType, &stream);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&inType);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * kChannels * 2);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, inType, nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();

    const UINT32 total = static_cast<UINT32>(seconds * rate);
    const UINT32 chunk = rate / 10;
    for (UINT32 done = 0; SUCCEEDED(hr) && done < total; done += chunk) {
        const UINT32 frames = total - done < chunk ? total - done : chunk;
        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;
        hr = MFCreateMemoryBuffer(frames * kChannels * 2, &buffer);
        BYTE* data = nullptr;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            auto* out = reinterpret_cast<int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) out[i * 2] = out[i * 2 + 1] = SineSample(frequency, rate, done + i);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(frames * kChannels * 2);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(static_cast<LONGLONG>(done) * 10000000 / rate);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000 / rate);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample);
        SafeRelease(sample);
        SafeRelease(buffer);
    }
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    SafeRelease(inType);
    SafeRelease(outType);
    SafeRelease(writer);
    return hr;
}

}  // namespace mt
