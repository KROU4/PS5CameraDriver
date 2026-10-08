#include "config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace ps5cam {

namespace {

// The picture keys; their limits are ClampPicture's. A flag is on for any value but 0.
struct PictureKey {
    std::string_view name;
    uint32_t PictureSettings::*number;
    bool PictureSettings::*flag;
};
constexpr PictureKey kPictureKeys[] = {
    {"mode", &PictureSettings::mode, nullptr},
    {"blur", &PictureSettings::blur, nullptr},
    {"autofocus", nullptr, &PictureSettings::autoFocus},
    {"focus", &PictureSettings::focus, nullptr},
    {"highlights", &PictureSettings::highlights, nullptr},
    {"temporal", &PictureSettings::temporal, nullptr},
    {"autobrightness", nullptr, &PictureSettings::autoBrightness},
    {"maxgain", &PictureSettings::maxGain, nullptr},
    {"denoise", &PictureSettings::denoise, nullptr},
    {"antiflicker", &PictureSettings::antiFlicker, nullptr},
};
constexpr std::string_view kLinuxKeys[] = {"mains", "fps", "always_on"};

std::string_view Trim(std::string_view s)
{
    const size_t begin = s.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool IsNumericKey(const std::string& key)
{
    const auto named = [&](const PictureKey& k) { return k.name == key; };
    return std::any_of(std::begin(kPictureKeys), std::end(kPictureKeys), named) ||
           std::find(std::begin(kLinuxKeys), std::end(kLinuxKeys), key) != std::end(kLinuxKeys);
}

// Stores v under a numeric key; note says what was stored instead of v, if anything.
void SetNumber(BokehConfig& c, const std::string& key, uint32_t v, std::string& note)
{
    for (const PictureKey& k : kPictureKeys) {
        if (k.name != key) continue;
        if (k.flag) {
            c.*k.flag = v != 0;
            return;
        }
        PictureSettings probe;  // ClampPicture limits every value on its own
        probe.*k.number = v;
        ClampPicture(probe);
        c.*k.number = probe.*k.number;
        if (c.*k.number != v) note = "out of range, using " + std::to_string(c.*k.number);
        return;
    }
    auto choice = [&](uint32_t& field, bool allowed, uint32_t fallback, const char* what) {
        field = allowed ? v : fallback;
        if (!allowed) note = std::string("not ") + what + ", using " + std::to_string(fallback);
    };
    if (key == "mains") choice(c.mainsHz, v == 50 || v == 60, 50, "50 or 60");
    else if (key == "fps") choice(c.fps, v == 30 || v == 60, 60, "30 or 60");
    else if (key == "always_on") c.alwaysOn = v != 0;
}

bool ReadFile(const std::string& path, std::string& text, int& error)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = errno;
        return false;
    }
    text.clear();
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            error = errno;
            close(fd);
            return false;
        }
        if (n == 0) break;
        text.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return true;
}

const char* ModeName(uint32_t mode)
{
    static const char* const kNames[] = {"bokeh", "main sensor", "second sensor", "depth", "side by side"};
    return mode < 5 ? kNames[mode] : "?";
}

const char* AntiFlickerName(uint32_t v)
{
    static const char* const kNames[] = {"auto", "50 Hz", "60 Hz", "off"};
    return v < 4 ? kNames[v] : "?";
}

}  // namespace

bool ParseUint(std::string_view text, uint32_t& value)
{
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return !text.empty() && ec == std::errc() && ptr == end;
}

bool ParseFloat(const std::string& text, float& value)
{
    // strtof follows the C locale, which this program never changes: '.' is the decimal point.
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(text.c_str(), &end);
    if (text.empty() || errno != 0 || end != text.c_str() + text.size() || !std::isfinite(v)) return false;
    value = v;
    return true;
}

BokehConfig ParseConfig(std::string_view text, const std::string& path, std::set<std::string>& warnedKeys)
{
    BokehConfig c;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // UTF-8 BOM from Windows editors
    unsigned lineNo = 0;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        ++lineNo;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        const std::string key = eq == std::string_view::npos ? std::string() : Lower(Trim(line.substr(0, eq)));
        if (key.empty()) {
            Log("%s:%u: expected \"key = value\", ignored: %.*s", path.c_str(), lineNo, int(line.size()), line.data());
            continue;
        }
        const std::string_view value = Trim(line.substr(eq + 1));
        if (key == "camera" || key == "output") {
            (key == "camera" ? c.camera : c.output) = value == "auto" ? std::string() : std::string(value);
            continue;
        }
        if (!IsNumericKey(key)) {
            if (warnedKeys.insert(key).second) Log("%s:%u: unknown key \"%s\", ignored", path.c_str(), lineNo, key.c_str());
            continue;
        }
        uint32_t v = 0;
        if (!ParseUint(value, v)) {
            Log("%s:%u: %s = %.*s: not a whole number, ignored", path.c_str(), lineNo, key.c_str(),
                int(value.size()), value.data());
            continue;
        }
        std::string note;
        SetNumber(c, key, v, note);
        if (!note.empty()) Log("%s:%u: %s = %u: %s", path.c_str(), lineNo, key.c_str(), v, note.c_str());
    }
    return c;
}

bool LoadConfig(const std::string& path, std::set<std::string>& warnedKeys, BokehConfig& config)
{
    const FileStamp before = StatFile(path);
    std::string text;
    int error = 0;
    const bool read = ReadFile(path, text, error);
    // An editor that truncates and then writes must not give the defaults for a moment.
    if (!(StatFile(path) == before)) return false;
    if (!read) {
        Log("%s: %s, using the defaults", path.c_str(), error == ENOENT ? "no such file" : strerror(error));
        config = BokehConfig();
        return true;
    }
    config = ParseConfig(text, path, warnedKeys);
    return true;
}

std::string DescribeConfig(const BokehConfig& c)
{
    char text[512];
    snprintf(text, sizeof(text),
        "mode %u (%s), blur %u, autofocus %u, focus %u, highlights %u, temporal %u, autobrightness %u, maxgain %u, "
        "denoise %u, antiflicker %u (%s), mains %u Hz, fps %u, always_on %u, camera %s, output %s",
        c.mode, ModeName(c.mode), c.blur, c.autoFocus ? 1 : 0, c.focus, c.highlights, c.temporal,
        c.autoBrightness ? 1 : 0, c.maxGain, c.denoise, c.antiFlicker, AntiFlickerName(c.antiFlicker), c.mainsHz,
        c.fps, c.alwaysOn ? 1 : 0, c.camera.empty() ? "auto" : c.camera.c_str(),
        c.output.empty() ? "auto" : c.output.c_str());
    return text;
}

FileStamp StatFile(const std::string& path)
{
    FileStamp s;
    struct stat st = {};
    if (stat(path.c_str(), &st) == 0) {
        s.exists = true;
        s.mtimeNs = int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        s.size = st.st_size;
        s.inode = st.st_ino;
    }
    return s;
}

bool FileWatch::Changed()
{
    const FileStamp now = StatFile(m_path);
    if (!m_first && now == m_last) return false;
    m_first = false;
    m_last = now;
    return true;
}

}  // namespace ps5cam
