#pragma once
// The camera's anti-flicker control (UVC power line frequency). Its default, 50 Hz, makes the
// camera's auto exposure use whole 10 ms steps, the period of a lamp's flicker on 50 Hz mains: at
// 60 fps that is 10 ms at most, 60% of the light a 16.7 ms frame could gather, and a dim room
// looks darker and noisier at 60 fps than it has to. In Auto, a dim scene runs with the control
// off; if lamps then flicker, the rows of the picture brighten and darken in bands that move from
// frame to frame, which FlickerGuard sees, and the control goes back to 50 Hz.
//
// The bands are told apart from motion by their rhythm: 100 Hz flicker repeats every 3 frames at
// 60 fps (and at 30 fps), so a row changes more between neighbouring frames than over 3 frames,
// while motion and exposure changes build up over 3 frames, and noise affects both alike.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pipeline.h"

namespace ps5cam {

enum class AntiFlicker : uint32_t { Auto = 0, Hz50 = 1, Hz60 = 2, Off = 3 };

// Values of KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY.
constexpr int kPowerLineOff = 0;
constexpr int kPowerLine50 = 1;
constexpr int kPowerLine60 = 2;

// Mean luma (0..1) of every second row of the main sensor in a packed camera frame.
void MainRowMeans(const uint8_t* yuy2, uint32_t pitch, const StereoFormat& format, std::vector<float>& rows);

class FlickerGuard {
public:
    // At stream start or when the setting changes; returns the control value to apply.
    int Start(AntiFlicker setting, bool mains60);
    // Every frame, with MainRowMeans of it; returns a control value to apply now, or -1.
    int Update(const std::vector<float>& rows);
    // Flicker seen while the control was off (Auto keeps 50 Hz for the rest of the stream).
    bool FlickerSeen() const { return m_flickerSeen; }
    // The control value in force.
    int Value() const { return m_value; }
    // An app set the control itself: leave it alone until the next Start.
    void Hold() { m_adaptive = false; }
    // The latest median score (bench / diagnostics).
    float Score() const { return m_lastScore; }
    // Bench: watch for bands as if Auto had just switched the control off.
    void WatchAsIfOff()
    {
        m_adaptive = true;
        m_flickerSeen = false;
        Set(kPowerLineOff);
    }

    static constexpr float kDark = 0.25f;           // mean luma below which Auto tries the control off
    static constexpr uint32_t kDarkFrames = 60;     // ... for this many frames in a row
    static constexpr uint32_t kSettleFrames = 4;    // frames ignored after a change of the control
    static constexpr uint32_t kWindow = 30;         // scores the decision takes the median of
    // (D1 - D3) / mean luma; bands of amplitude a score ~0.65 a, recorded clips without flicker
    // (motion, a dark room, daylight) at most 0.001.
    static constexpr float kFlickerScore = 0.006f;

private:
    int Set(int value);
    float FrameScore() const;

    bool m_adaptive = false;  // Auto on 50 Hz mains: may switch between 50 Hz and off
    int m_value = kPowerLine50;
    bool m_flickerSeen = false;
    uint32_t m_darkFrames = 0;
    uint32_t m_sinceChange = 0;
    std::vector<float> m_history[4];  // row means of the last frames, [0] newest
    uint32_t m_have = 0;
    std::vector<float> m_scores;
    float m_lastScore = 0;
};

}  // namespace ps5cam
