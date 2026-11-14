// M4A/AAC, ALAC-in-M4A and raw ADTS .aac through the Media Foundation Source Reader, plus the
// tag reader for every format Media Foundation parses (MP3, FLAC, WAV, M4A, AAC).
//
// Media Foundation is delay-loaded and only ever used after MediaFoundationAvailable() loaded
// mfplat.dll and mfreadwrite.dll from System32. On a Windows edition without Media Foundation
// (the "N" editions without the Media Feature Pack) nothing here is called and M4A files are
// reported as "no AAC decoder"; every other format keeps working.
//
// Files are opened with MFCreateFile on the \\?\ path and MFCreateSourceReaderFromByteStream;
// MFCreateSourceReaderFromURL cannot take a \\?\ path.
#include "source.h"

#include "tag_reader.h"
#include "text.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propkey.h>
#include <propsys.h>

#include <cstring>

namespace music {

namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

volatile LONG g_mfState = 0;  // 0 = not tried, 1 = available, 2 = not available
SRWLOCK g_mfLock = SRWLOCK_INIT;

// COM must already be initialised on the calling thread. Both engine threads that use Media
// Foundation (player and I/O) enter the multithreaded apartment at thread start; a test program
// does the same on its own thread.

HRESULT OpenReader(const std::wstring& path, IMFSourceReader** reader) {
    *reader = nullptr;
    IMFByteStream* stream = nullptr;
    const std::wstring pathX = ToExtendedPath(path);
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, pathX.c_str(), &stream);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromByteStream(stream, nullptr, reader);
    SafeRelease(stream);
    return hr;
}

class MfSource : public Source {
public:
    explicit MfSource(IMFSourceReader* reader) : reader_(reader) {}

    ~MfSource() override {
        SafeRelease(sample_);
        SafeRelease(reader_);
    }

    bool Configure() {
        HRESULT hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (SUCCEEDED(hr)) hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
        IMFMediaType* type = nullptr;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);  // native rate and channels
        if (SUCCEEDED(hr)) hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type);
        SafeRelease(type);
        if (FAILED(hr)) return false;
        if (!ReadFormat()) return false;

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0) {
            // An estimate for some formats; the real end is where ReadSample says so.
            info_.lengthFrames = static_cast<int64_t>(duration.uhVal.QuadPart * info_.sampleRate / 10000000ull);
        }
        PropVariantClear(&duration);
        return true;
    }

    SourceInfo Info() const override { return info_; }

    uint32_t Read(float* out, uint32_t frames, bool* failed) override {
        uint32_t done = 0;
        while (done < frames) {
            if (!sample_) {
                if (ended_) break;
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                const HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                                       &timestamp, &sample_);
                if (FAILED(hr)) {
                    *failed = done == 0;
                    ended_ = true;
                    break;
                }
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                    // The caller sized its buffer for the rate and channel count it was given at
                    // open. A stream that changes either mid-way ends here rather than overflow it.
                    const SourceInfo before = info_;
                    if (!ReadFormat() || info_.sampleRate != before.sampleRate || info_.channels != before.channels) {
                        info_.sampleRate = before.sampleRate;
                        info_.channels = before.channels;
                        SafeRelease(sample_);
                        ended_ = true;
                        *failed = done == 0;
                        break;
                    }
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) ended_ = true;
                if (!sample_) continue;
                if (!LockSample()) {
                    SafeRelease(sample_);
                    continue;
                }
                if (dropFrames_ > 0) {
                    // After a seek the first sample starts at or before the target: drop the front.
                    const uint32_t available = (sampleBytes_ - sampleOffset_) / (info_.channels * 4);
                    const uint32_t drop = dropFrames_ < available ? static_cast<uint32_t>(dropFrames_) : available;
                    sampleOffset_ += drop * info_.channels * 4;
                    dropFrames_ -= drop;
                }
            }
            const uint32_t available = (sampleBytes_ - sampleOffset_) / (info_.channels * 4);
            const uint32_t take = available < frames - done ? available : frames - done;
            memcpy(out + static_cast<size_t>(done) * info_.channels, sampleData_ + sampleOffset_, static_cast<size_t>(take) * info_.channels * 4);
            done += take;
            sampleOffset_ += take * info_.channels * 4;
            if (sampleOffset_ >= sampleBytes_) UnlockSample();
        }
        return done;
    }

    bool Seek(int64_t frame) override {
        if (frame < 0) return false;
        UnlockSample();
        ended_ = false;
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(frame) * 10000000ll / info_.sampleRate;
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
        if (FAILED(hr)) return false;
        seekTarget100ns_ = position.hVal.QuadPart;
        pendingSeek_ = true;
        dropFrames_ = 0;
        return true;
    }

private:
    bool ReadFormat() {
        IMFMediaType* actual = nullptr;
        if (FAILED(reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual))) return false;
        UINT32 rate = 0;
        UINT32 channels = 0;
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actual->Release();
        if (rate == 0 || channels == 0 || channels > 32) return false;
        info_.sampleRate = rate;
        info_.channels = channels;
        return true;
    }

    bool LockSample() {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample_->ConvertToContiguousBuffer(&buffer))) return false;
        BYTE* data = nullptr;
        DWORD bytes = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &bytes))) {
            buffer->Release();
            return false;
        }
        buffer_ = buffer;
        sampleData_ = data;
        sampleBytes_ = bytes - bytes % (info_.channels * 4);
        sampleOffset_ = 0;
        if (pendingSeek_) {
            pendingSeek_ = false;
            LONGLONG timestamp = 0;
            if (SUCCEEDED(sample_->GetSampleTime(&timestamp)) && timestamp < seekTarget100ns_) {
                dropFrames_ = static_cast<int64_t>((seekTarget100ns_ - timestamp) * info_.sampleRate / 10000000ll);
            }
        }
        return true;
    }

    void UnlockSample() {
        if (buffer_) {
            buffer_->Unlock();
            buffer_->Release();
            buffer_ = nullptr;
        }
        SafeRelease(sample_);
        sampleData_ = nullptr;
        sampleBytes_ = 0;
        sampleOffset_ = 0;
    }

    IMFSourceReader* reader_;
    IMFSample* sample_ = nullptr;
    IMFMediaBuffer* buffer_ = nullptr;
    BYTE* sampleData_ = nullptr;
    DWORD sampleBytes_ = 0;
    DWORD sampleOffset_ = 0;
    bool ended_ = false;
    bool pendingSeek_ = false;
    LONGLONG seekTarget100ns_ = 0;
    int64_t dropFrames_ = 0;
    SourceInfo info_;
};

}  // namespace

bool MediaFoundationAvailable() {
    if (g_mfState != 0) return g_mfState == 1;
    AcquireSRWLockExclusive(&g_mfLock);
    if (g_mfState == 0) {
        LONG state = 2;
        // System32 only: a same-named file in the game folder must not be taken.
        const HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const HMODULE readWrite = plat ? LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        if (plat && readWrite && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            state = 1;  // MFShutdown is never called: the engine lives as long as the process
        }
        InterlockedExchange(&g_mfState, state);
    }
    ReleaseSRWLockExclusive(&g_mfLock);
    return g_mfState == 1;
}

std::unique_ptr<Source> OpenMediaFoundationSource(const std::wstring& path, OpenError* error) {
    if (!MediaFoundationAvailable()) {
        *error = OpenError::NoAacDecoder;
        return nullptr;
    }
    IMFSourceReader* reader = nullptr;
    const HRESULT hr = OpenReader(path, &reader);
    if (FAILED(hr)) {
        *error = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
                     ? OpenError::Missing
                     : OpenError::Damaged;
        return nullptr;
    }
    std::unique_ptr<MfSource> source(new MfSource(reader));  // owns the reader from here
    if (!source->Configure()) {
        *error = OpenError::Damaged;
        return nullptr;
    }
    *error = OpenError::None;
    return source;
}

bool ReadMediaFoundationTags(const std::wstring& path, Tags* tags) {
    if (!MediaFoundationAvailable()) return false;
    IMFSourceReader* reader = nullptr;
    if (FAILED(OpenReader(path, &reader))) return false;

    // The media source's own property store (not the shell's): no shell extension is loaded, and
    // no output type is selected, so no decoder is loaded either.
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(reader->GetServiceForStream(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PROPERTY_HANDLER_SERVICE,
                                              IID_PPV_ARGS(&store)))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Title, &value)) && value.vt == VT_LPWSTR && value.pwszVal) tags->title = value.pwszVal;
        PropVariantClear(&value);
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Music_Artist, &value))) {
            if (value.vt == (VT_VECTOR | VT_LPWSTR) && value.calpwstr.cElems > 0 && value.calpwstr.pElems[0]) {
                tags->artist = value.calpwstr.pElems[0];
            } else if (value.vt == VT_LPWSTR && value.pwszVal) {
                tags->artist = value.pwszVal;
            }
        }
        PropVariantClear(&value);
        store->Release();
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0) {
        tags->durationSeconds = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&duration);
    reader->Release();
    return true;
}

}  // namespace music
