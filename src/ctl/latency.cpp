#include "latency.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "../common/frameclock.h"
#include "../common/latencyreader.h"
#include "../common/vcamreg.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr uint32_t kWarmupFrames = 30;

class MfRuntime {
public:
    HRESULT Start()
    {
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(m_com) && m_com != RPC_E_CHANGED_MODE) return m_com;
        m_mf = MFStartup(MF_VERSION);
        return m_mf;
    }
    ~MfRuntime()
    {
        if (SUCCEEDED(m_mf)) MFShutdown();
        if (SUCCEEDED(m_com)) CoUninitialize();
    }

private:
    HRESULT m_com = E_FAIL;
    HRESULT m_mf = E_FAIL;
};

class MediaSource {
public:
    IMFMediaSource** Put() { return &m_source; }
    IMFMediaSource* Get() const { return m_source.Get(); }
    ~MediaSource()
    {
        if (m_source) m_source->Shutdown();
    }

private:
    ComPtr<IMFMediaSource> m_source;
};

bool ParseUnsigned(const wchar_t* text, uint32_t minimum, uint32_t maximum, uint32_t* result)
{
    if (!text || !*text || *text == L'-') return false;
    for (const wchar_t* p = text; *p; ++p)
        if (*p < L'0' || *p > L'9') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || !end || *end || value < minimum || value > maximum) return false;
    *result = static_cast<uint32_t>(value);
    return true;
}

HRESULT OpenPhysicalCamera(MediaSource* source)
{
    const std::wstring link = ps5cam::FindPhysicalCameraLink();
    if (link.empty()) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr))
        hr = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr))
        hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attributes.Get(), source->Put());
    return hr;
}

bool Is1080p60(IMFMediaType* type)
{
    UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
    GUID major = {}, subtype = {};
    return type && SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Video &&
           SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
           (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2) &&
           SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) && width == 1920 && height == 1080 &&
           SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) && denominator &&
           numerator >= uint64_t(59) * denominator && numerator <= uint64_t(61) * denominator;
}

HRESULT Select1080p60(IMFSourceReader* reader, IMFMediaType** selected)
{
    ComPtr<IMFMediaType> yuy2;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(kVideoStream, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) return hr;
        if (Is1080p60(type.Get())) {
            GUID subtype = {};
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                *selected = type.Detach();
                return S_OK;
            }
            if (!yuy2) yuy2 = type;
        }
    }
    if (yuy2) {
        *selected = yuy2.Detach();
        return S_OK;
    }
    return MF_E_INVALIDMEDIATYPE;
}

void PrintSummary(const char* name, const ps5cam::latency::Series& series)
{
    const auto summary = series.Summarize();
    if (!summary) {
        std::printf("%-24s unavailable\n", name);
        return;
    }
    std::printf("%-24s n=%zu mean=%.3f p50=%.3f p95=%.3f max=%.3f ms\n", name, summary->count,
        summary->mean, summary->p50, summary->p95, summary->maximum);
}

void CsvOptional(FILE* file, const std::optional<uint64_t>& value)
{
    if (value) std::fprintf(file, "%llu", static_cast<unsigned long long>(*value));
}

void CsvOptional(FILE* file, const std::optional<double>& value)
{
    if (value) std::fprintf(file, "%.6f", *value);
}

struct Measurements {
    ps5cam::latency::Series deviceToReader;
    ps5cam::latency::Series driverToReader;
    ps5cam::latency::Series processing;
    ps5cam::latency::Series queue;
    ps5cam::latency::Series transport;
    ps5cam::latency::Series gpu;
    ps5cam::latency::Series readWait;
    ps5cam::latency::Series deliveryGap;
    uint64_t dropped = 0;
    size_t droppedSamples = 0;
};

void Add(Measurements* all, const ps5cam::SampleLatency& sample, double readWait, std::optional<double> gap)
{
    if (sample.deviceToApp) all->deviceToReader.Add(*sample.deviceToApp);
    if (sample.driverToApp) all->driverToReader.Add(*sample.driverToApp);
    if (sample.processingSpan) all->processing.Add(*sample.processingSpan);
    if (sample.queue) all->queue.Add(*sample.queue);
    if (sample.transport) all->transport.Add(*sample.transport);
    if (sample.gpuMs) all->gpu.Add(*sample.gpuMs);
    all->readWait.Add(readWait);
    if (gap) all->deliveryGap.Add(*gap);
    if (sample.dropped) {
        all->dropped += *sample.dropped;
        ++all->droppedSamples;
    }
}

void WriteCsv(FILE* file, uint32_t frame, uint64_t arrival, const ps5cam::SampleLatency& sample,
    double readWait, std::optional<double> deliveryGap)
{
    if (!file) return;
    std::fprintf(file, "%u,%llu,", frame, static_cast<unsigned long long>(arrival));
    CsvOptional(file, sample.sequence);
    std::fputc(',', file);
    CsvOptional(file, sample.dropped);
    std::fputc(',', file);
    CsvOptional(file, sample.deviceTime);
    std::fputc(',', file);
    CsvOptional(file, sample.ingressTime);
    std::fputc(',', file);
    CsvOptional(file, sample.processedTime);
    std::fputc(',', file);
    CsvOptional(file, sample.deliveredTime);
    std::fputc(',', file);
    CsvOptional(file, sample.deviceToApp);
    std::fputc(',', file);
    CsvOptional(file, sample.driverToApp);
    std::fputc(',', file);
    CsvOptional(file, sample.processingSpan);
    std::fputc(',', file);
    CsvOptional(file, sample.queue);
    std::fputc(',', file);
    CsvOptional(file, sample.transport);
    std::fputc(',', file);
    CsvOptional(file, sample.gpuMs);
    std::fprintf(file, ",%.6f,", readWait);
    CsvOptional(file, deliveryGap);
    std::fputc('\n', file);
}

}  // namespace

int RunLatency(int argc, wchar_t** argv)
{
    uint32_t frames = 360;
    uint32_t consumerDelayMs = 0;
    if (argc > 3 || (argc >= 1 && !ParseUnsigned(argv[0], 1, 18'000, &frames)) ||
        (argc >= 2 && !ParseUnsigned(argv[1], 0, 1'000, &consumerDelayMs)) ||
        (argc >= 3 && (!argv[2] || !*argv[2]))) {
        std::fwprintf(stderr, L"usage: ps5cam-ctl latency [frames=360] [consumer-delay-ms=0] [csv-path]\n");
        return 1;
    }

    MfRuntime runtime;
    HRESULT hr = runtime.Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "Media Foundation startup failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }

    MediaSource source;
    hr = OpenPhysicalCamera(&source);
    if (FAILED(hr)) {
        std::fprintf(stderr, "physical PS5 camera (05A9:058C) unavailable: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return 2;
    }

    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader);
    if (FAILED(hr)) {
        std::fprintf(stderr, "source reader creation failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 3;
    }

    ComPtr<IMFMediaType> type;
    hr = Select1080p60(reader.Get(), &type);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "1920x1080 at 60 fps is unavailable: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 4;
    }
    hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(kVideoStream, TRUE);
    if (FAILED(hr)) {
        std::fprintf(stderr, "video stream selection failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 4;
    }

    FILE* csv = nullptr;
    if (argc == 3) {
        const errno_t error = _wfopen_s(&csv, argv[2], L"w");
        if (error || !csv) {
            std::fwprintf(stderr, L"cannot create CSV %ls (error %d)\n", argv[2], int(error));
            return 5;
        }
        std::fprintf(csv, "frame,arrival_100ns,sequence,dropped,device_100ns,ingress_100ns,processed_100ns,"
                          "delivered_100ns,device_to_reader_ms,driver_to_reader_ms,processing_ms,queue_ms,"
                          "transport_ms,gpu_ms,read_wait_ms,delivery_gap_ms\n");
    }

    std::printf("capturing %u measured frames after %u warm-up frames at 1920x1080 60 fps; consumer delay %u ms\n",
        frames, kWarmupFrames, consumerDelayMs);
    Measurements measurements;
    uint32_t received = 0;
    uint32_t measured = 0;
    uint32_t emptyReads = 0;
    uint64_t previousArrival = 0;
    const uint64_t required = uint64_t(frames) + kWarmupFrames;
    while (received < required) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        const uint64_t readBegin = ps5cam::FrameClockNow();
        hr = reader->ReadSample(kVideoStream, 0, nullptr, &flags, nullptr, &sample);
        const uint64_t arrival = ps5cam::FrameClockNow();
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM |
                                      MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))) {
            if (SUCCEEDED(hr)) hr = MF_E_END_OF_STREAM;
            std::fprintf(stderr, "ReadSample stopped after %u/%llu frames: 0x%08lX flags 0x%08lX\n", received,
                static_cast<unsigned long long>(required), static_cast<unsigned long>(hr),
                static_cast<unsigned long>(flags));
            if (csv) std::fclose(csv);
            return 6;
        }
        if (!sample) {
            if (++emptyReads > 300) {
                std::fprintf(stderr, "capture incomplete: 300 consecutive reads returned no frame\n");
                if (csv) std::fclose(csv);
                return 6;
            }
            continue;
        }
        emptyReads = 0;
        ++received;
        if (received > kWarmupFrames) {
            ++measured;
            const ps5cam::SampleLatency latency = ps5cam::ReadLatency(sample.Get(), arrival);
            const auto readWait = ps5cam::latency::ElapsedMs(readBegin, arrival);
            const auto gap = previousArrival ? ps5cam::latency::ElapsedMs(previousArrival, arrival) : std::nullopt;
            if (!readWait) {
                std::fprintf(stderr, "non-monotonic Media Foundation clock during capture\n");
                if (csv) std::fclose(csv);
                return 6;
            }
            Add(&measurements, latency, *readWait, gap);
            WriteCsv(csv, measured, arrival, latency, *readWait, gap);
            previousArrival = arrival;
        }
        if (consumerDelayMs) Sleep(consumerDelayMs);
    }

    if (csv && std::fclose(csv) != 0) {
        std::fprintf(stderr, "failed to finish CSV output\n");
        return 5;
    }
    if (measured != frames) {
        std::fprintf(stderr, "capture incomplete: measured %u/%u frames\n", measured, frames);
        return 6;
    }

    PrintSummary("device -> reader", measurements.deviceToReader);
    PrintSummary("driver -> reader", measurements.driverToReader);
    PrintSummary("driver processing span", measurements.processing);
    PrintSummary("driver output queue", measurements.queue);
    PrintSummary("delivery -> reader", measurements.transport);
    PrintSummary("GPU compute", measurements.gpu);
    PrintSummary("ReadSample wait", measurements.readWait);
    PrintSummary("delivery interval", measurements.deliveryGap);
    if (const auto interval = measurements.deliveryGap.Summarize(); interval && interval->mean > 0)
        std::printf("%-24s %.3f fps (n=%zu)\n", "reader frame rate", 1000.0 / interval->mean, interval->count);
    else
        std::printf("%-24s unavailable\n", "reader frame rate");
    if (measurements.droppedSamples)
        std::printf("reported dropped frames  %llu (n=%zu)\n",
            static_cast<unsigned long long>(measurements.dropped), measurements.droppedSamples);
    else
        std::printf("reported dropped frames  unavailable\n");
    return 0;
}
