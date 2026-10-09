// ps5cam-bokehd-tests: the parts of the daemon that need no camera, loopback device or GPU — the
// config file, the calibration file and the open counting of the inotify fallback. Run by ctest.
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "calibration.h"
#include "config.h"
#include "openwatch.h"

using namespace ps5cam;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (ok) return;
    ++g_failed;
    fprintf(stderr, "bokehd_tests.cpp:%d: failed: %s\n", line, what);
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

std::string TempDir()
{
    const char* base = getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/ps5cam-tests-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    return mkdtemp(name.data()) ? std::string(name.data()) : std::string();
}

bool WriteText(const std::string& path, const std::string& text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

// What f writes to stderr (the daemon's log).
std::string CaptureLog(const std::function<void()>& f)
{
    fflush(stderr);
    FILE* temp = tmpfile();
    if (!temp) return "tmpfile failed";
    const int saved = dup(2);
    dup2(fileno(temp), 2);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::string text;
    rewind(temp);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), temp)) > 0) text.append(buf, n);
    fclose(temp);
    return text;
}

size_t Count(const std::string& text, const std::string& what)
{
    size_t count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
    return count;
}

BokehConfig Parse(const std::string& text)
{
    std::set<std::string> warned;
    BokehConfig c;
    CaptureLog([&] { c = ParseConfig(text, "test.conf", warned); });
    return c;
}

void TestDefaults()
{
    const BokehConfig c = Parse("");
    const BokehConfig defaults;
    CHECK(DescribeConfig(c) == DescribeConfig(defaults));
    CHECK(c.mode == 0 && c.blur == 25 && c.autoFocus && c.focus == 50 && c.highlights == 150 && c.temporal == 40);
    CHECK(c.autoBrightness && c.maxGain == 60 && c.denoise == 90 && c.antiFlicker == 0 && c.sharpen == 50);
    CHECK(c.brightness == 50 && c.contrast == 50 && c.saturation == 50);
    CHECK(c.mainsHz == 50 && c.fps == 60 && !c.alwaysOn && c.camera.empty() && c.output.empty());
    EffectSettings e;
    ApplyConfig(c, e);
    CHECK(e.mode == ViewMode::Bokeh && e.blurStrength == 0.6f && e.denoise == 0.9f && e.maxGain == 6.0f);
    CHECK(e.highlights == 1.5f && e.temporal == 0.4f && e.manualFocus == 0.5f && e.sharpen == 0.5f);
#ifdef PS5CAM_BOKEH_CONF
    // The installed file spells out every default.
    std::set<std::string> warned;
    BokehConfig file;
    bool loaded = false;
    const std::string log = CaptureLog([&] { loaded = LoadConfig(PS5CAM_BOKEH_CONF, warned, file); });
    CHECK(loaded);
    CHECK(log.empty());
    CHECK(DescribeConfig(file) == DescribeConfig(defaults));
#endif
}

void TestClamps()
{
    const BokehConfig c = Parse(
        "mode = 7\nblur = 150\nfocus = 200\nhighlights = 401\ntemporal = 1\nmaxgain = 999\ndenoise = 101\nsharpen = 101\n"
        "antiflicker = 9\nmains = 55\nfps = 25\nalways_on = 2\nautofocus = 0\nautobrightness = 5\n");
    // The limits are ClampPicture's.
    PictureSettings raw;
    raw.mode = 7, raw.blur = 150, raw.focus = 200, raw.highlights = 401, raw.temporal = 1, raw.maxGain = 999;
    raw.denoise = 101, raw.antiFlicker = 9, raw.sharpen = 101;
    ClampPicture(raw);
    CHECK(c.mode == raw.mode && c.blur == raw.blur && c.focus == raw.focus && c.highlights == raw.highlights);
    CHECK(c.temporal == raw.temporal && c.maxGain == raw.maxGain && c.denoise == raw.denoise);
    CHECK(c.antiFlicker == raw.antiFlicker && c.sharpen == raw.sharpen && c.sharpen == 100);
    CHECK(c.mode == 0 && c.blur == 100 && c.temporal == 5 && c.maxGain == 160 && c.antiFlicker == 0);
    CHECK(c.mainsHz == 50 && c.fps == 60 && c.alwaysOn && !c.autoFocus && c.autoBrightness);
    const BokehConfig ok = Parse("mode = 4\nmains = 60\nfps = 30\nantiflicker = 3\ntemporal = 5\nmaxgain = 10\n");
    CHECK(ok.mode == 4 && ok.mainsHz == 60 && ok.fps == 30 && ok.antiFlicker == 3 && ok.temporal == 5);
    CHECK(ok.maxGain == 10);

    std::set<std::string> warned;
    const std::string log = CaptureLog([&] { ParseConfig("blur = 150\nfps = 25\n", "t.conf", warned); });
    CHECK(Count(log, "t.conf:1: blur = 150: out of range, using 100") == 1);
    CHECK(Count(log, "t.conf:2: fps = 25: not 30 or 60, using 60") == 1);
}

void TestSyntax()
{
    const BokehConfig c = Parse(
        "\xEF\xBB\xBF"
        "mode = 2\n"
        "# a comment\n"
        "   \n"
        "Blur = 10   # trailing comment\r\n"
        "no equals sign here\n"
        "= 5\n"
        "focus = abc\n"
        "denoise = -5\n"
        "highlights = 1e3\n"
        "temporal =\n"
        "maxgain = 20 30\n"
        "fps = 30\r\n"
        "camera = /dev/video9\n"
        "output = auto\n");
    CHECK(c.mode == 2);  // after the BOM
    CHECK(c.blur == 10);
    CHECK(c.focus == 50 && c.denoise == 90 && c.highlights == 150 && c.temporal == 40 && c.maxGain == 60);
    CHECK(c.fps == 30);
    CHECK(c.camera == "/dev/video9" && c.output.empty());

    std::set<std::string> warned;
    const std::string text = "foo = 1\nbar = 2\nfoo = 3\nno equals\n";
    std::string log = CaptureLog([&] { ParseConfig(text, "u.conf", warned); });
    CHECK(Count(log, "unknown key \"foo\"") == 1);
    CHECK(Count(log, "unknown key \"bar\"") == 1);
    CHECK(Count(log, "u.conf:4: expected \"key = value\"") == 1);
    log = CaptureLog([&] { ParseConfig(text, "u.conf", warned); });  // a reload of the same file
    CHECK(Count(log, "unknown key") == 0);
    CHECK(Count(log, "expected \"key = value\"") == 1);  // a broken line is reported each time
}

void TestFiles(const std::string& dir)
{
    std::set<std::string> warned;
    BokehConfig c;
    c.blur = 1;
    bool loaded = false;
    const std::string missing = dir + "/missing.conf";
    const std::string log = CaptureLog([&] { loaded = LoadConfig(missing, warned, c); });
    CHECK(loaded && c.blur == 25);
    CHECK(Count(log, "no such file, using the defaults") == 1);

    // Every Settled() call below is one poll of the daemon.
    const std::string path = dir + "/bokeh.conf";
    CHECK(WriteText(path, "blur = 33\n"));
    FileWatch watch(path);
    FileStamp read;
    CHECK(LoadConfig(path, warned, c, &read) && c.blur == 33);
    watch.MarkRead(read);
    CHECK(!watch.Settled());  // the version already read
    CHECK(!watch.Settled());
    CHECK(WriteText(path, "blur = 44\nfps = 30\n"));
    CHECK(!watch.Settled());  // just changed: wait one poll
    CHECK(watch.Settled());   // left alone for a poll: read it
    CHECK(LoadConfig(path, warned, c, &read) && c.blur == 44 && c.fps == 30);
    watch.MarkRead(read);
    CHECK(!watch.Settled());
    // An editor truncating and then writing: the empty version is never read.
    CHECK(WriteText(path, ""));
    CHECK(!watch.Settled());
    CHECK(WriteText(path, "blur = 55\n"));
    CHECK(!watch.Settled());
    CHECK(watch.Settled());
    CHECK(LoadConfig(path, warned, c, &read) && c.blur == 55);
    watch.MarkRead(read);
    CHECK(unlink(path.c_str()) == 0);
    CHECK(!watch.Settled());
    CHECK(watch.Settled());  // removed for good: the defaults
}

void TestCalibration(const std::string& dir)
{
    CHECK(setenv("STATE_DIRECTORY", (dir + ":/elsewhere").c_str(), 1) == 0);
    CHECK(StateDirectory() == dir);
    const mode_t oldMask = umask(077);  // like the service (UMask=0077)
    Rectification r;
    CHECK(!LoadCalibration(dir, r));
    CHECK(SaveCalibration(dir, {-2.345f, 0.1234f, 0.5f, 0.2f}));
    umask(oldMask);
    Rectification back;
    CHECK(LoadCalibration(dir, back));
    CHECK(std::fabs(back.dy + 2.345f) < 0.001f && std::fabs(back.rotation - 0.123f) < 0.001f);
    struct stat st = {};
    CHECK(stat((dir + "/calibration").c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
    // Nothing but the file itself: the temporary one was renamed over it.
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
        closedir(d);
    }
    CHECK(names.size() == 1 && names[0] == "calibration");

    const std::string path = dir + "/calibration";
    const struct {
        const char* text;
        bool usable;
    } cases[] = {{"-13 1.25\n", true}, {"13.5 0\n", false}, {"1 -1.5\n", false}, {"abc\n", false},
        {"1.5\n", false}, {"1.5 x\n", false}, {"", false}, {"0.250 -0.750\r\n", true}};
    for (const auto& k : cases) {
        CHECK(WriteText(path, k.text));
        Rectification loaded;
        bool ok = false;
        CaptureLog([&] { ok = LoadCalibration(dir, loaded); });
        if (ok != k.usable) fprintf(stderr, "calibration file \"%s\"\n", k.text);
        CHECK(ok == k.usable);
    }
    unlink(path.c_str());
}

void TestOpenCounting(const std::string& dir)
{
    CHECK(CountOpens(0, IN_OPEN) == 1);
    CHECK(CountOpens(1, IN_OPEN) == 2);
    CHECK(CountOpens(2, IN_CLOSE_NOWRITE) == 1);
    CHECK(CountOpens(1, IN_CLOSE_WRITE) == 0);
    CHECK(CountOpens(0, IN_CLOSE_WRITE) == 0);  // a program that opened before the watch
    CHECK(CountOpens(0, IN_Q_OVERFLOW) == 1);   // lost events: assume a viewer
    CHECK(CountOpens(3, IN_Q_OVERFLOW) == 3);
    CHECK(CountOpens(2, IN_IGNORED) == 2);

    // The real thing on a file.
    const std::string path = dir + "/node";
    CHECK(WriteText(path, "x"));
    OpenWatch watch;
    std::string error;
    CHECK(watch.Start(path, error));
    CHECK(watch.Read() == 0);
    const int a = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(watch.Read() == 1);
    const int b = open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(watch.Read() == 2);
    close(a);
    CHECK(watch.Read() == 1);
    close(b);
    CHECK(watch.Read() == 0);
    watch.Stop();
    CHECK(!watch.IsActive());
    CHECK(!watch.Start(dir + "/missing", error) && !error.empty());
    unlink(path.c_str());
}

}  // namespace

int main()
{
    const std::string dir = TempDir();
    if (dir.empty()) {
        fprintf(stderr, "cannot create a temporary directory\n");
        return 1;
    }
    TestDefaults();
    TestClamps();
    TestSyntax();
    TestFiles(dir);
    TestCalibration(dir);
    TestOpenCounting(dir);
    rmdir(dir.c_str());
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
