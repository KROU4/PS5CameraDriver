#include "flicker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace ps5cam {

namespace {

// Flicker seen lately by this module (the device MFT and the virtual camera each have their own
// copy, both in Frame Server): the next streams keep 50 Hz instead of showing the bands again.
std::atomic<uint64_t> g_flickerTick = 0;
constexpr uint64_t kRememberMs = 30ULL * 60 * 1000;

uint64_t NowMs()
{
    // Never 0, which g_flickerTick reserves for "not seen".
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()) + 1;
}

}  // namespace

void MainRowMeans(const uint8_t* yuy2, uint32_t pitch, const StereoFormat& f, std::vector<float>& rows)
{
    const size_t offset = f.halfSecond ? StereoFormat::kHalfHeaderPixels * 2 : f.MainIsRightStream() ? f.eyeWidth * 2 : 0;
    const uint32_t step = 16;  // pixels; Y of every pixel is at an even byte
    const uint32_t samples = f.eyeWidth / step;
    rows.resize(f.eyeHeight / 2);
    for (uint32_t i = 0; i < rows.size(); ++i) {
        const uint8_t* line = yuy2 + size_t(i) * 2 * pitch + offset;
        uint32_t sum = 0;
        for (uint32_t x = 0; x < samples; ++x) sum += line[size_t(x) * step * 2];
        rows[i] = sum / (255.0f * samples);
    }
}

int FlickerGuard::Start(AntiFlicker setting, bool mains60)
{
    m_adaptive = setting == AntiFlicker::Auto && !mains60;
    const uint64_t seen = g_flickerTick;
    m_flickerSeen = seen && NowMs() - seen < kRememberMs;
    switch (setting) {
    case AntiFlicker::Hz50: return Set(kPowerLine50);
    case AntiFlicker::Hz60: return Set(kPowerLine60);
    case AntiFlicker::Off: return Set(kPowerLineOff);
    default: return Set(mains60 ? kPowerLine60 : kPowerLine50);  // Auto starts safe
    }
}

int FlickerGuard::Set(int value)
{
    m_value = value;
    m_sinceChange = 0;
    m_have = 0;
    m_darkFrames = 0;
    m_scores.clear();
    return value;
}

float FlickerGuard::FrameScore() const
{
    const auto& now = m_history[0];
    const auto& prev = m_history[1];
    const auto& back3 = m_history[3];
    double d1 = 0, d3 = 0, level = 0;
    for (size_t i = 0; i < now.size(); ++i) {
        d1 += std::fabs(now[i] - prev[i]);
        d3 += std::fabs(now[i] - back3[i]);
        level += now[i];
    }
    return float((d1 - d3) / std::max(level, now.size() * 0.02));
}

int FlickerGuard::Update(const std::vector<float>& rows)
{
    if (!m_adaptive || rows.empty()) return -1;
    if (m_value == kPowerLine50) {
        if (m_flickerSeen) return -1;
        double level = 0;
        for (float r : rows) level += r;
        level /= rows.size();
        m_darkFrames = level < kDark ? m_darkFrames + 1 : 0;
        return m_darkFrames >= kDarkFrames ? Set(kPowerLineOff) : -1;
    }
    // Control off: watch for bands, using only frames taken after the change.
    if (++m_sinceChange <= kSettleFrames) return -1;
    for (int i = 3; i > 0; --i) m_history[i].swap(m_history[i - 1]);
    m_history[0] = rows;
    for (const auto& h : m_history)
        if (h.size() != rows.size()) return -1;  // the frame size changed lately
    if (++m_have < 4) return -1;
    m_scores.push_back(FrameScore());
    if (m_scores.size() > kWindow) m_scores.erase(m_scores.begin());
    if (m_scores.size() < kWindow / 3) return -1;
    std::vector<float> sorted = m_scores;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    m_lastScore = sorted[sorted.size() / 2];
    if (m_lastScore <= kFlickerScore) return -1;
    m_flickerSeen = true;
    g_flickerTick = NowMs();
    return Set(kPowerLine50);
}

}  // namespace ps5cam
