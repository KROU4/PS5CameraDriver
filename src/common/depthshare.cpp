#include "depthshare.h"

#include <cstring>

namespace ps5cam {

namespace {

constexpr wchar_t kName[] = L"Local\\PS5CameraDepth";
constexpr uint32_t kMagic = 0x48545044;  // "DPTH"
constexpr uint32_t kVersion = 1;
constexpr size_t kPlaneBytes = size_t(DepthShare::kMaxWidth) * DepthShare::kMaxHeight;

}  // namespace

struct DepthShare::Header {
    uint32_t magic;
    uint32_t version;
    volatile LONG64 frame;        // frames published; frame & 1 is the buffer of the newest one
    volatile LONG64 started;      // the frame being written (set before its buffer is touched)
    volatile LONG64 readerTick;   // GetTickCount64 of the last reader heartbeat
    struct {
        uint32_t width, height;
        LONG64 time;              // the frame's time stamp (100 ns)
        LONG64 tick;              // GetTickCount64 when it was published
    } planes[2];
};

DepthShare::~DepthShare()
{
    if (m_header) UnmapViewOfFile(m_header);
    if (m_mapping) CloseHandle(m_mapping);
}

bool DepthShare::Open()
{
    if (m_header) return true;
    const size_t total = sizeof(Header) + 2 * kPlaneBytes;
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(total), kName);
    if (!m_mapping) return false;
    void* view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!view) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }
    m_header = static_cast<Header*>(view);
    m_planes = static_cast<uint8_t*>(view) + sizeof(Header);
    // A new mapping is zero-filled; whoever comes first stamps it (a second opener may race here
    // only with the same values).
    if (m_header->magic != kMagic) {
        m_header->version = kVersion;
        m_header->magic = kMagic;
    }
    return m_header->version == kVersion;
}

bool DepthShare::ReaderActive()
{
    if (!m_header) return false;
    const ULONGLONG last = static_cast<ULONGLONG>(InterlockedCompareExchange64(&m_header->readerTick, 0, 0));
    return last && GetTickCount64() - last < kReaderTimeoutMs;
}

void DepthShare::Publish(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t pitch, LONGLONG time)
{
    if (!m_header || width > kMaxWidth || height > kMaxHeight) return;
    const LONG64 next = InterlockedCompareExchange64(&m_header->frame, 0, 0) + 1;
    const int buffer = static_cast<int>(next & 1);
    InterlockedExchange64(&m_header->started, next);  // readers of frame next - 2 retry
    uint8_t* dst = m_planes + buffer * kPlaneBytes;
    for (uint32_t y = 0; y < height; ++y) memcpy(dst + size_t(y) * width, plane + size_t(y) * pitch, width);
    m_header->planes[buffer].width = width;
    m_header->planes[buffer].height = height;
    m_header->planes[buffer].time = time;
    m_header->planes[buffer].tick = static_cast<LONG64>(GetTickCount64());
    InterlockedExchange64(&m_header->frame, next);  // full barrier: the plane is complete before
}

void DepthShare::Touch()
{
    if (m_header) InterlockedExchange64(&m_header->readerTick, static_cast<LONG64>(GetTickCount64()));
}

bool DepthShare::Read(std::vector<uint8_t>& plane, uint32_t* width, uint32_t* height, uint64_t* frame,
    ULONGLONG* publishedTick)
{
    if (!m_header) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG64 newest = InterlockedCompareExchange64(&m_header->frame, 0, 0);
        if (newest <= 0 || static_cast<uint64_t>(newest) == *frame) return false;
        const int buffer = static_cast<int>(newest & 1);
        const uint32_t w = m_header->planes[buffer].width, h = m_header->planes[buffer].height;
        if (!w || !h || w > kMaxWidth || h > kMaxHeight) return false;
        const ULONGLONG tick = static_cast<ULONGLONG>(m_header->planes[buffer].tick);
        plane.resize(size_t(w) * h);
        MemoryBarrier();
        memcpy(plane.data(), m_planes + buffer * kPlaneBytes, plane.size());
        MemoryBarrier();
        // The writer fills the other buffer next; writing frame newest + 2 would reach this one.
        if (InterlockedCompareExchange64(&m_header->started, 0, 0) - newest >= 2) continue;
        *width = w;
        *height = h;
        *frame = static_cast<uint64_t>(newest);
        if (publishedTick) *publishedTick = tick;
        return true;
    }
    return false;
}

}  // namespace ps5cam
