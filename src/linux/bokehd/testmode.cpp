#include "testmode.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

namespace ps5cam {

namespace {

double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int RunTest(const TestOptions& o)
{
    StereoFormat sf;
    sf.eyeWidth = 1920;
    sf.eyeHeight = 1080;
    sf.halfSecond = true;
    OutputFormat of;
    of.width = 1920;
    of.height = 1080;
    of.format = o.format;
    EffectSettings es;
    es.mode = static_cast<ViewMode>(o.mode);
    es.blurStrength = o.blur / 100.0f;
    es.denoise = o.denoise / 100.0f;

    const uint32_t pitch = sf.PackedWidth() * 2;
    const size_t frameBytes = size_t(pitch) * sf.PackedHeight();
    std::ifstream in(o.input, std::ios::binary | std::ios::ate);
    if (!in) {
        fprintf(stderr, "cannot open %s\n", o.input.c_str());
        return 2;
    }
    const auto clipBytes = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    if (clipBytes % frameBytes)
        fprintf(stderr, "warning: the clip is not a whole number of %zu-byte frames (not 2448x1088 YUY2?)\n", frameBytes);
    std::vector<uint8_t> frame(frameBytes);

    const bool yuy2 = o.format == PixelFormat::YUY2;
    const uint32_t dstPitch = yuy2 ? of.width * 2 : of.width;
    const size_t planeBytes = size_t(of.width) * of.height;
    std::vector<uint8_t> image(yuy2 ? size_t(dstPitch) * of.height : planeBytes * 3 / 2);
    std::ofstream out(o.output, std::ios::binary);
    if (!out) {
        fprintf(stderr, "cannot create %s\n", o.output.c_str());
        return 2;
    }
    const std::string depthPath = o.output + ".depth";
    std::ofstream depthOut;
    std::vector<uint8_t> depthBuf;
    DepthPlane plane;
    if (o.depthPlane >= 0) {
        es.depthPlane = true;
        es.depthView = static_cast<uint32_t>(o.depthPlane);
        depthOut.open(depthPath, std::ios::binary);
        if (!depthOut) {
            fprintf(stderr, "cannot create %s\n", depthPath.c_str());
            return 2;
        }
        depthBuf.resize(planeBytes);
        plane.data = depthBuf.data();
        plane.pitch = of.width;
    }

    StereoPipeline p;
    HRESULT hr = p.Initialize(sf, of);
    if (FAILED(hr)) {
        fprintf(stderr, "Initialize failed 0x%08X%s\n", static_cast<unsigned>(hr),
            hr == E_NOTIMPL ? ": no GPU with Vulkan 1.1 (for lavapipe set PS5CAM_GPU=llvmpipe)" : "");
        return 3;
    }
    printf("GPU: %s\n", p.GpuName().c_str());

    uint32_t n = 0;
    double cpuTotal = 0, gpuTotal = 0;
    while (n < o.frames && in.read(reinterpret_cast<char*>(frame.data()), std::streamsize(frameBytes))) {
        if (n == 0 && o.haveDy) {
            p.SetRectification({o.dy, 0, 0});
        } else if (n == 0) {
            Rectification r;
            const auto t0 = std::chrono::steady_clock::now();
            hr = p.Calibrate(frame.data(), pitch, &r);
            printf("calibration %s: dy %.2f px, roll %.2f deg, score %.2f (%.0f ms)\n", SUCCEEDED(hr) ? "ok" : "failed",
                r.dy, r.rotation, r.quality, MsSince(t0));
        }
        FrameStats st;
        const auto t0 = std::chrono::steady_clock::now();
        hr = p.Process(frame.data(), pitch, es, image.data(), yuy2 ? nullptr : image.data() + planeBytes, dstPitch, &st,
            es.depthPlane ? &plane : nullptr);
        const double ms = MsSince(t0);
        if (FAILED(hr)) {
            fprintf(stderr, "Process failed 0x%08X\n", static_cast<unsigned>(hr));
            return 4;
        }
        if (hr == S_FALSE) {
            ++n;
            continue;  // pipeline priming
        }
        if (n > 1) cpuTotal += ms, gpuTotal += st.gpuMs;
        if (n % 10 == 0)
            printf("frame %3u: wall %.2f ms, gpu %.2f ms, focus disparity %.1f, noise %.2f/4096\n", n, ms, st.gpuMs,
                st.focusDisparity, st.noise * 4096);
        out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
        if (es.depthPlane && plane.written)
            depthOut.write(reinterpret_cast<const char*>(depthBuf.data()), std::streamsize(depthBuf.size()));
        ++n;
    }
    if (n > 2) printf("%u frames, mean wall %.2f ms, mean gpu %.2f ms\n", n, cpuTotal / (n - 2), gpuTotal / (n - 2));
    out.close();
    if (!out) {
        fprintf(stderr, "cannot write %s\n", o.output.c_str());
        return 5;
    }
    if (es.depthPlane) {
        depthOut.close();
        if (!depthOut) {
            fprintf(stderr, "cannot write %s\n", depthPath.c_str());
            return 5;
        }
    }
    return 0;
}

}  // namespace ps5cam
