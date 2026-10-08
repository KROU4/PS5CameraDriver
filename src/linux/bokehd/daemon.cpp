#include "daemon.h"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <vector>

#include "calibration.h"
#include "calibschedule.h"
#include "capture.h"
#include "config.h"
#include "devices.h"
#include "flicker.h"
#include "log.h"
#include "loopback.h"
#include "openwatch.h"
#include "pipeline.h"
#include "version.h"

namespace ps5cam {

namespace {

constexpr uint64_t kScanMs = 1000;        // missing devices and config changes are looked for this often
constexpr uint64_t kStopDelayMs = 3000;   // programs often stop and start again while setting up a call
constexpr uint64_t kGpuRetryMs = 2000;
constexpr uint64_t kCameraRetryMs = 2000;  // the camera is there but did not start (another program has it?)
constexpr uint64_t kStallMs = 3000;        // no frame for this long: restart the camera's stream
constexpr uint64_t kBlackMs = 1000;        // nothing to show for this long: a black frame keeps programs going
constexpr const char* kNoGpu = "no GPU with Vulkan 1.1 found; lavapipe is used only with PS5CAM_GPU=llvmpipe";

uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

StereoFormat CameraFormat()
{
    StereoFormat sf;
    sf.eyeWidth = 1920;
    sf.eyeHeight = 1080;
    sf.halfSecond = true;  // PackedWidth() 2448, PackedHeight() 1088
    return sf;
}

OutputFormat LoopbackFormat()
{
    OutputFormat of;
    of.width = Loopback::kWidth;
    of.height = Loopback::kHeight;
    of.format = PixelFormat::YUY2;
    return of;
}

// systemd's Type=notify: the unit counts as started once the output is ours, and logins are
// ordered after it (Before=systemd-user-sessions.service), so no user program gets the writer side.
void NotifyReady()
{
    const char* path = getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    const size_t len = strlen(path);
    if ((path[0] != '/' && path[0] != '@') || len >= sizeof(addr.sun_path)) return;
    memcpy(addr.sun_path, path, len);
    if (path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        Log("sd_notify socket: %s", strerror(errno));
        return;
    }
    static constexpr char kReady[] = "READY=1";
    if (sendto(fd, kReady, sizeof(kReady) - 1, 0, reinterpret_cast<const sockaddr*>(&addr),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len)) < 0)
        Log("sd_notify: %s", strerror(errno));
    close(fd);
}

// How the daemon learns that a program watches the output.
enum class Demand {
    Events,  // v4l2loopback's client-usage event (0.13 and newer)
    Opens,   // inotify on the node: opens minus closes (older v4l2loopback)
    Always,  // always_on = 1, or neither works: stream whenever the camera is connected
};

class Daemon {
public:
    explicit Daemon(const std::string& configPath)
        : m_watch(configPath), m_stateDir(StateDirectory()), m_frame(Loopback::kFrameBytes)
    {
    }
    ~Daemon()
    {
        if (m_signalFd >= 0) close(m_signalFd);
    }
    int Run();

private:
    bool SetUpSignals();
    void HandleSignals();
    void Reload(bool force);
    std::string ProbeGpu();
    void Tick(uint64_t now);
    bool Wanted(uint64_t now) const;
    void OpenOutput(uint64_t now);
    void CloseOutput(const std::string& why);
    void SetViewers(int count, uint64_t now);
    void StartStream(uint64_t now);
    void RestartStalledStream(uint64_t now);
    void StopStream(const char* why);
    void OnCameraReady(uint64_t now, bool error);
    void ProcessFrame(const uint8_t* frame, uint64_t now);
    bool EnsurePipeline(uint64_t now);
    void Calibrate(const uint8_t* frame, uint32_t pitch);
    void ApplyPowerLine(int value);
    void LogOnce(std::string& last, const std::string& message);

    FileWatch m_watch;
    std::set<std::string> m_warnedKeys;
    BokehConfig m_config;
    bool m_configLoaded = false;
    EffectSettings m_effect;
    std::string m_stateDir;
    int m_signalFd = -1;
    bool m_quit = false;
    uint64_t m_nextScan = 0;

    Loopback m_output;
    OpenWatch m_opens;
    Demand m_demand = Demand::Always;
    std::string m_outputProblem;  // the last one logged, so that retries log nothing new
    int m_clients = 0;            // programs watching the output, per the event or the open count
    uint64_t m_stopAt = 0;        // when the stream stops after the last program left; 0: not pending
    uint64_t m_lastWrite = 0;

    Capture m_capture;
    std::string m_cameraProblem;
    uint64_t m_nextCameraTry = 0;
    uint64_t m_lastFrameAt = 0;
    uint32_t m_stalls = 0;

    std::unique_ptr<StereoPipeline> m_pipeline;
    uint64_t m_gpuRetryAt = 0;
    uint32_t m_gpuInitFailures = 0;
    uint32_t m_gpuProcessFailures = 0;

    CalibrationSchedule m_calibration;
    uint32_t m_frameCount = 0;  // frames given to the pipeline, for the calibration schedule

    FlickerGuard m_flicker;
    std::vector<float> m_rowMeans;
    bool m_powerLineFailed = false;

    std::vector<uint8_t> m_frame;  // the output frame
    // The current stream, for the line logged when it stops.
    uint64_t m_streamStart = 0;
    uint64_t m_framesOut = 0;
    uint32_t m_late = 0;
    uint32_t m_incomplete = 0;
    double m_gpuMsTotal = 0;
};

int Daemon::Run()
{
    // Before anything starts threads (GPU drivers do), so that every thread blocks these signals.
    if (!SetUpSignals()) return 1;
    Reload(true);
    // Right after boot udev may still be giving the node its group: a few tries before telling
    // systemd that the service is up.
    for (int attempt = 0; attempt < 15; ++attempt) {
        OpenOutput(NowMs());
        if (m_output.IsOpen() || (m_config.output.empty() && LoopbackCandidates().empty())) break;
        poll(nullptr, 0, 200);
    }
    NotifyReady();
    m_nextScan = NowMs() + kScanMs;
    const std::string gpu = ProbeGpu();
    const std::string camera = m_config.camera.empty() ? FindCamera() : m_config.camera;
    Log("ps5cam-bokehd %s: GPU %s, camera %s, output %s, mode %u, %u fps", PS5CAM_VERSION, gpu.c_str(),
        camera.empty() ? "not connected" : camera.c_str(), m_output.IsOpen() ? m_output.Path().c_str() : "not found",
        m_config.mode, m_config.fps);
    Log("settings from %s: %s; calibration in %s", m_watch.Path().c_str(), DescribeConfig(m_config).c_str(),
        m_stateDir.c_str());

    while (!m_quit) {
        uint64_t now = NowMs();
        Tick(now);
        pollfd fds[3] = {};
        nfds_t count = 0;
        fds[count++] = {m_signalFd, POLLIN, 0};
        int demandIndex = -1;
        if (m_output.IsOpen() && m_demand == Demand::Events) {
            demandIndex = int(count);
            fds[count++] = {m_output.Fd(), POLLPRI, 0};
        } else if (m_output.IsOpen() && m_demand == Demand::Opens) {
            demandIndex = int(count);
            fds[count++] = {m_opens.Fd(), POLLIN, 0};
        }
        const int cameraIndex = m_capture.IsOpen() ? int(count) : -1;
        if (cameraIndex >= 0) fds[count++] = {m_capture.Fd(), POLLIN, 0};
        // Tick runs at least every kScanMs, and on time for a pending stop.
        uint64_t timeout = kScanMs;
        if (m_stopAt) timeout = std::min(timeout, m_stopAt > now ? m_stopAt - now : 0);
        const int ready = poll(fds, count, static_cast<int>(timeout));
        if (ready < 0 && errno != EINTR) {
            Log("poll: %s", strerror(errno));
            break;
        }
        if (ready <= 0) continue;
        now = NowMs();
        if (fds[0].revents) HandleSignals();
        constexpr short kBroken = POLLERR | POLLHUP | POLLNVAL;
        if (demandIndex >= 0 && m_output.IsOpen() && fds[demandIndex].revents) {
            if (fds[demandIndex].revents & kBroken) CloseOutput("the output device reported an error");
            else if (m_demand == Demand::Events) SetViewers(m_output.TakeUsage(), now);
            else if (m_demand == Demand::Opens) SetViewers(m_opens.Read(), now);
        }
        if (cameraIndex >= 0 && m_capture.IsOpen() && fds[cameraIndex].revents)
            OnCameraReady(now, (fds[cameraIndex].revents & kBroken) != 0);
    }
    StopStream("the service stops");
    m_opens.Stop();
    m_output.Close();
    return m_quit ? 0 : 1;
}

bool Daemon::SetUpSignals()
{
    // A log reader that went away (journald restarting, a closed pipe) must not end the service.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        Log("signal(SIGPIPE): %s", strerror(errno));
        return false;
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &set, nullptr) != 0) {
        Log("sigprocmask: %s", strerror(errno));
        return false;
    }
    m_signalFd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_signalFd < 0) {
        Log("signalfd: %s", strerror(errno));
        return false;
    }
    return true;
}

void Daemon::HandleSignals()
{
    signalfd_siginfo info;
    while (read(m_signalFd, &info, sizeof(info)) == ssize_t(sizeof(info))) {
        if (info.ssi_signo == SIGHUP) {
            Log("SIGHUP: reading %s again", m_watch.Path().c_str());
            Reload(true);
        } else {
            Log("%s: stopping", strsignal(static_cast<int>(info.ssi_signo)));
            m_quit = true;
        }
    }
}

void Daemon::Reload(bool force)
{
    const bool changed = m_watch.Changed();
    if (!changed && !force) return;
    BokehConfig c;
    if (!LoadConfig(m_watch.Path(), m_warnedKeys, c)) return;  // being written: FileWatch sees it again
    const BokehConfig old = m_config;
    const bool first = !m_configLoaded;
    m_config = c;
    m_configLoaded = true;
    ApplyConfig(m_config, m_effect);
    if (first) return;  // Run logs the settings with the start
    if (DescribeConfig(c) == DescribeConfig(old)) return;
    Log("settings changed: %s", DescribeConfig(c).c_str());
    if (m_output.IsOpen() && (c.output != old.output || c.alwaysOn != old.alwaysOn)) {
        CloseOutput("its settings changed");  // Tick opens it again
    } else if (m_output.IsOpen() && c.fps != old.fps) {
        std::string error;
        if (!m_output.SetFps(c.fps, error)) Log("%s: %s", m_output.Path().c_str(), error.c_str());
    }
    if (m_capture.IsOpen() && (c.fps != old.fps || c.camera != old.camera)) {
        StopStream("the camera settings changed");  // Tick starts it again
        m_nextCameraTry = 0;
    } else if (m_capture.IsOpen() && (c.antiFlicker != old.antiFlicker || c.mainsHz != old.mainsHz)) {
        ApplyPowerLine(m_flicker.Start(static_cast<AntiFlicker>(c.antiFlicker), c.mainsHz == 60));
    }
}

std::string Daemon::ProbeGpu()
{
    // Tells at once in the log whether there is a usable GPU; the stream makes its own pipeline.
    StereoPipeline probe;
    const HRESULT hr = probe.Initialize(CameraFormat(), LoopbackFormat());
    if (SUCCEEDED(hr)) return probe.GpuName();
    if (hr == E_NOTIMPL) return std::string("none (") + kNoGpu + ")";
    char text[64];
    snprintf(text, sizeof(text), "unusable (pipeline init failed 0x%08X)", static_cast<unsigned>(hr));
    return text;
}

void Daemon::Tick(uint64_t now)
{
    if (now >= m_nextScan) {
        m_nextScan = now + kScanMs;
        Reload(false);
        if (!m_output.IsOpen()) OpenOutput(now);
    }
    if (m_stopAt && now >= m_stopAt) m_stopAt = 0;
    const bool wanted = Wanted(now);
    if (!wanted && m_capture.IsOpen()) StopStream("no program watches");
    if (!wanted) return;
    if (!m_capture.IsOpen() && now >= m_nextCameraTry) StartStream(now);
    if (m_capture.IsOpen() && now - m_lastFrameAt >= kStallMs) RestartStalledStream(now);
    // A program watching a missing camera (or a GPU being re-created) gets black instead of a
    // frozen picture, and does not give up waiting.
    if (m_output.IsOpen() && now - m_lastWrite >= kBlackMs) {
        std::string error;
        if (m_output.WriteBlack(error)) m_lastWrite = now;
        else CloseOutput(error);
    }
}

bool Daemon::Wanted(uint64_t now) const
{
    if (!m_output.IsOpen()) return false;
    if (m_demand == Demand::Always) return true;
    return m_clients > 0 || (m_stopAt && now < m_stopAt);
}

void Daemon::OpenOutput(uint64_t now)
{
    const std::vector<std::string> candidates =
        m_config.output.empty() ? LoopbackCandidates() : std::vector<std::string>{m_config.output};
    if (candidates.empty()) {
        LogOnce(m_outputProblem, std::string("waiting for the v4l2loopback device \"") + kLoopbackLabel +
                                     "\" (modprobe v4l2loopback with /etc/modprobe.d/ps5camera-v4l2loopback.conf)");
        return;
    }
    std::string problems;
    for (const std::string& path : candidates) {
        std::string error;
        if (m_output.Open(path, m_config.fps, error)) break;
        problems += (problems.empty() ? "output " : "; ") + path + ": " + error;
    }
    if (!m_output.IsOpen()) {
        LogOnce(m_outputProblem, problems + "; retrying");
        return;
    }
    m_outputProblem.clear();
    m_clients = 0;
    m_stopAt = 0;
    m_lastWrite = now;
    // The watch starts after our own open, so that it counts other programs only.
    std::string error;
    if (m_config.alwaysOn) {
        m_demand = Demand::Always;
        Log("output %s ready; always_on = 1: the camera runs whenever it is connected", m_output.Path().c_str());
    } else if (m_output.UsageEvents()) {
        m_demand = Demand::Events;
        Log("output %s ready; the camera runs while a program watches it (v4l2loopback's client-usage event)",
            m_output.Path().c_str());
    } else if (m_opens.Start(m_output.Path(), error)) {
        m_demand = Demand::Opens;
        Log("output %s ready; this v4l2loopback (older than 0.13) does not report its readers, so the camera runs "
            "while a program has the device open (inotify)", m_output.Path().c_str());
    } else {
        m_demand = Demand::Always;
        Log("output %s ready; %s: the camera runs whenever it is connected", m_output.Path().c_str(), error.c_str());
    }
}

void Daemon::CloseOutput(const std::string& why)
{
    Log("output %s closed: %s", m_output.Path().c_str(), why.c_str());
    m_opens.Stop();
    m_output.Close();
    StopStream("no output device");
    m_clients = 0;
    m_stopAt = 0;
}

void Daemon::SetViewers(int count, uint64_t now)
{
    if (count < 0 || count == m_clients) return;
    if (count > 0 && m_clients == 0) Log("a program watches %s", m_output.Path().c_str());
    if (count == 0) Log("no program watches %s now; stopping in %llu s", m_output.Path().c_str(), kStopDelayMs / 1000ULL);
    m_clients = count;
    m_stopAt = count == 0 ? now + kStopDelayMs : 0;
}

void Daemon::StartStream(uint64_t now)
{
    const std::string path = m_config.camera.empty() ? FindCamera() : m_config.camera;
    if (path.empty()) {
        LogOnce(m_cameraProblem, "waiting for the camera (USB 05a9:058c offering YUYV 2448x1088)");
        m_nextCameraTry = now + kScanMs;
        return;
    }
    std::string error;
    if (!m_capture.Open(path, m_config.fps, error)) {
        LogOnce(m_cameraProblem, "camera " + path + ": " + error + "; retrying");
        m_nextCameraTry = now + kCameraRetryMs;
        return;
    }
    m_cameraProblem.clear();
    m_lastFrameAt = now;
    m_stalls = 0;
    m_streamStart = now;
    m_framesOut = 0;
    m_late = 0;
    m_incomplete = 0;
    m_gpuMsTotal = 0;
    m_powerLineFailed = false;
    ApplyPowerLine(m_flicker.Start(static_cast<AntiFlicker>(m_config.antiFlicker), m_config.mainsHz == 60));
    Log("streaming: camera %s YUYV 2448x1088 at %.0f fps -> %s", path.c_str(), m_capture.Fps(),
        m_output.Path().c_str());
}

void Daemon::RestartStalledStream(uint64_t now)
{
    const std::string path = m_capture.Path();
    if (m_stalls++ % 20 == 0)
        Log("camera %s sent no frame for %llu s, restarting its stream", path.c_str(), kStallMs / 1000ULL);
    std::string error;
    m_capture.Close();
    if (!m_capture.Open(path, m_config.fps, error)) {
        LogOnce(m_cameraProblem, "camera " + path + ": " + error + "; retrying");
        StopStream("the camera stopped sending frames");
        m_nextCameraTry = now + kCameraRetryMs;
        return;
    }
    m_lastFrameAt = now;
    if (m_pipeline) m_pipeline->Reset();
    ApplyPowerLine(m_flicker.Value());
}

void Daemon::StopStream(const char* why)
{
    if (m_capture.IsOpen()) {
        // Auto may have switched the anti-flicker off in a dark room: other programs using the camera
        // directly would get flickering lamps. Fails on an unplugged camera, which forgets it anyway.
        std::string ignored;
        m_capture.SetPowerLine(m_config.mainsHz == 60 ? kPowerLine60 : kPowerLine50, ignored);
        const double seconds = (NowMs() - m_streamStart) / 1000.0;
        Log("stream stopped (%s): %llu frames in %.0f s, GPU %.2f ms per frame, %u dropped late, %u incomplete", why,
            static_cast<unsigned long long>(m_framesOut), seconds, m_framesOut ? m_gpuMsTotal / m_framesOut : 0.0,
            m_late, m_incomplete);
        m_capture.Close();
        // The next program to open "PS5 Camera" must not see the last frame of this session.
        std::string error;
        if (m_output.IsOpen() && m_output.WriteBlack(error)) m_lastWrite = NowMs();
        else if (m_output.IsOpen()) Log("%s", error.c_str());
    }
    m_pipeline.reset();  // frees the GPU and its memory while nobody watches
}

void Daemon::OnCameraReady(uint64_t now, bool error)
{
    const uint8_t* frame = nullptr;
    std::string problem;
    Capture::Drops drops;
    Capture::Result result = m_capture.Dequeue(&frame, drops, problem);
    m_late += drops.late;
    if (drops.incomplete) {
        const uint32_t before = m_incomplete;
        m_incomplete += drops.incomplete;
        if (before == 0 || before / 300 != m_incomplete / 300)
            Log("dropped incomplete frames from the camera (%u so far)", m_incomplete);
    }
    if (result == Capture::Result::None && error) {
        result = Capture::Result::Lost;  // poll keeps reporting the error: do not spin on it
        problem = "the device reported an error";
    }
    switch (result) {
    case Capture::Result::None:
        return;
    case Capture::Result::Skipped:
        m_lastFrameAt = now;
        return;
    case Capture::Result::Lost:
        Log("camera %s lost: %s", m_capture.Path().c_str(), problem.c_str());
        StopStream("camera lost");
        m_nextCameraTry = now;
        return;
    case Capture::Result::Frame:
        break;
    }
    m_stalls = 0;
    ProcessFrame(frame, now);
    // After the processing: making the GPU pipeline (on the first frame) can take seconds.
    m_lastFrameAt = NowMs();
    if (m_capture.IsOpen() && !m_capture.Requeue(problem)) {
        Log("camera %s: %s", m_capture.Path().c_str(), problem.c_str());
        StopStream("camera error");
        m_nextCameraTry = now + kCameraRetryMs;
    }
}

void Daemon::ProcessFrame(const uint8_t* frame, uint64_t now)
{
    if (!EnsurePipeline(now)) return;
    const uint32_t pitch = m_capture.Pitch();
    ++m_frameCount;
    Calibrate(frame, pitch);
    MainRowMeans(frame, pitch, m_pipeline->Stereo(), m_rowMeans);
    if (const int powerLine = m_flicker.Update(m_rowMeans); powerLine >= 0) {
        if (powerLine == kPowerLineOff) Log("dim scene: anti-flicker off for a longer exposure");
        else Log("lamp flicker seen (score %.4f): anti-flicker back to 50 Hz", m_flicker.Score());
        ApplyPowerLine(powerLine);
    }
    FrameStats stats;
    const HRESULT hr = m_pipeline->Process(frame, pitch, m_effect, m_frame.data(), nullptr, Loopback::kPitch, &stats);
    if (FAILED(hr)) {
        // A lost device (driver reset, GPU hang) never recovers by itself: make a new pipeline.
        if (m_gpuProcessFailures++ % 30 == 0)
            Log("GPU pipeline failed 0x%08X, re-creating it", static_cast<unsigned>(hr));
        m_pipeline.reset();
        m_gpuRetryAt = now + kGpuRetryMs;
        return;
    }
    if (hr != S_OK) return;  // S_FALSE: the first frame only primes the pipeline
    m_gpuProcessFailures = 0;
    std::string error;
    if (!m_output.Write(m_frame.data(), error)) {
        CloseOutput(error);
        return;
    }
    // Now, not when the frame came: a slow frame must not make Tick add a black one after it.
    m_lastWrite = NowMs();
    ++m_framesOut;
    m_gpuMsTotal += stats.gpuMs;
}

bool Daemon::EnsurePipeline(uint64_t now)
{
    if (m_pipeline) return true;
    if (now < m_gpuRetryAt) return false;
    auto p = std::unique_ptr<StereoPipeline>(new (std::nothrow) StereoPipeline());
    const HRESULT hr = p ? p->Initialize(CameraFormat(), LoopbackFormat()) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        if (m_gpuInitFailures++ % 30 == 0) {
            if (hr == E_NOTIMPL) Log("%s; retrying every %llu s", kNoGpu, kGpuRetryMs / 1000ULL);
            else Log("GPU pipeline init failed 0x%08X, retrying every %llu s", static_cast<unsigned>(hr),
                     kGpuRetryMs / 1000ULL);
        }
        m_gpuRetryAt = now + kGpuRetryMs;
        return false;
    }
    if (m_gpuInitFailures) Log("GPU pipeline ready on %s", p->GpuName().c_str());
    m_gpuInitFailures = 0;
    Rectification saved;
    const bool calibrated = LoadCalibration(m_stateDir, saved);
    if (calibrated) p->SetRectification({saved.dy, saved.rotation, 0});
    m_calibration.Start(!calibrated, m_frameCount);
    m_pipeline = std::move(p);
    return true;
}

void Daemon::Calibrate(const uint8_t* frame, uint32_t pitch)
{
    if (!m_calibration.Due(m_frameCount)) return;
    Rectification r;
    if (SUCCEEDED(m_pipeline->Calibrate(frame, pitch, &r))) {
        m_calibration.Succeeded();
        SaveCalibration(m_stateDir, r);
        Log("calibrated: dy %.2f roll %.2f (score %.2f)", r.dy, r.rotation, r.quality);
    } else {
        m_calibration.Failed(m_frameCount, m_config.fps);  // dark or featureless scene: try again later
    }
}

void Daemon::ApplyPowerLine(int value)
{
    std::string error;
    if (!m_capture.IsOpen() || m_capture.SetPowerLine(value, error) || m_powerLineFailed) return;
    m_powerLineFailed = true;  // once per stream: the camera keeps its own setting
    Log("camera %s: %s", m_capture.Path().c_str(), error.c_str());
}

void Daemon::LogOnce(std::string& last, const std::string& message)
{
    if (message == last) return;
    Log("%s", message.c_str());
    last = message;
}

}  // namespace

int RunDaemon(const std::string& configPath)
{
    Daemon daemon(configPath);
    return daemon.Run();
}

}  // namespace ps5cam
