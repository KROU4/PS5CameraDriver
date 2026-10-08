#include "depthengine.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

#include "../common/log.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

namespace {

constexpr ULONGLONG kStaleMs = 1000;  // older than this, the picture is black: nothing streams
constexpr uint8_t kBlack = 16;        // studio range, like the planes themselves
constexpr uint8_t kNoChroma = 128;

}  // namespace

DepthEngine::~DepthEngine()
{
    Stop();
}

HRESULT DepthEngine::Start(const OutputRequest& req, FrameSink sink)
{
    Stop();
    m_req = req;
    m_req.fps = std::clamp<uint32_t>(m_req.fps, 1, 240);
    m_sink = std::move(sink);
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop) return HRESULT_FROM_WIN32(GetLastError());
    try {
        m_thread = std::thread([this] { Run(); });
    } catch (...) {
        CloseHandle(m_stop);
        m_stop = nullptr;
        return E_OUTOFMEMORY;
    }
    Log(L"depth camera: started %ux%u@%u", m_req.width, m_req.height, m_req.fps);
    return S_OK;
}

void DepthEngine::Stop()
{
    if (m_thread.joinable()) {
        SetEvent(m_stop);
        m_thread.join();
        Log(L"depth camera: stopped");
    }
    if (m_stop) {
        CloseHandle(m_stop);
        m_stop = nullptr;
    }
}

void DepthEngine::Run()
{
    const LONGLONG period = 10'000'000LL / m_req.fps;  // 100 ns
    LONGLONG next = MFGetSystemTime();
    ULONGLONG lastPlaneTick = 0, lastOpenTry = 0;
    bool lastFresh = true;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (!m_shareOpen && now - lastOpenTry >= 1000) {
            lastOpenTry = now;
            m_shareOpen = m_share.Open();
        }
        if (m_shareOpen) {
            m_share.Touch();  // tells the device MFT that depth is wanted
            uint32_t w = 0, h = 0;
            ULONGLONG published = 0;
            if (m_share.Read(m_plane, &w, &h, &m_frame, &published)) {
                m_planeW = w;
                m_planeH = h;
                lastPlaneTick = published;
            }
        }
        const bool fresh = m_planeW && now - lastPlaneTick < kStaleMs;
        if (fresh != lastFresh) {
            Log(fresh ? L"depth camera: depth arrives" : L"depth camera: no depth (is PS5 Camera in use?)");
            lastFresh = fresh;
        }
        if (ComPtr<IMFSample> sample = MakeFrame(fresh)) m_sink(sample.Get());

        next += period;
        const LONGLONG wait = next - MFGetSystemTime();
        if (wait < -period) next = MFGetSystemTime();  // fell behind (a stall): do not catch up in a burst
        if (WaitForSingleObject(m_stop, wait > 0 ? static_cast<DWORD>(wait / 10'000) : 0) == WAIT_OBJECT_0) return;
    }
}

ComPtr<IMFSample> DepthEngine::MakeFrame(bool fresh)
{
    const uint32_t w = m_req.width, h = m_req.height;
    const bool yuy2 = m_req.format == PixelFormat::YUY2;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer2> b2;
    if (FAILED(MFCreate2DMediaBuffer(w, h, yuy2 ? MFVideoFormat_YUY2.Data1 : MFVideoFormat_NV12.Data1, FALSE, &buffer)) ||
        FAILED(buffer.As(&b2)))
        return nullptr;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    if (FAILED(b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len))) return nullptr;
    const size_t rows = yuy2 ? h : h + h / 2;
    if (pitch <= 0 || size_t(start + len - scan0) < size_t(pitch) * rows) {
        b2->Unlock2D();
        return nullptr;
    }
    // Nearest neighbour when the program asked for another size than "PS5 Camera" streams.
    std::vector<uint32_t> xs(w);
    for (uint32_t x = 0; x < w; ++x) xs[x] = fresh ? std::min(m_planeW - 1, uint32_t(uint64_t(x) * m_planeW / w)) : 0;
    for (uint32_t y = 0; y < h; ++y) {
        BYTE* line = scan0 + size_t(y) * pitch;
        const uint8_t* src = fresh ? m_plane.data() + size_t(std::min(m_planeH - 1, uint32_t(uint64_t(y) * m_planeH / h))) * m_planeW
                                   : nullptr;
        if (yuy2) {
            for (uint32_t x = 0; x < w; ++x) {
                line[x * 2] = src ? src[xs[x]] : kBlack;
                line[x * 2 + 1] = kNoChroma;
            }
        } else if (src && w == m_planeW) {
            memcpy(line, src, w);
        } else {
            for (uint32_t x = 0; x < w; ++x) line[x] = src ? src[xs[x]] : kBlack;
        }
    }
    if (!yuy2)
        for (uint32_t y = 0; y < h / 2; ++y) memset(scan0 + size_t(h + y) * pitch, kNoChroma, w);
    b2->Unlock2D();
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return nullptr;
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(10'000'000LL / m_req.fps);
    return sample;
}

}  // namespace ps5cam
