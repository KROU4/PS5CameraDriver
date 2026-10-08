#pragma once
// The daemon's settings, /etc/ps5cam/bokeh.conf: "key = value" lines and # comments. The picture
// settings are PictureSettings (src/core/picture.h): the names, units, defaults and limits of the
// Windows "ps5cam-ctl set", so the same values give the same picture.
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "picture.h"

namespace ps5cam {

constexpr const char* kDefaultConfigPath = "/etc/ps5cam/bokeh.conf";

struct BokehConfig : PictureSettings {
    uint32_t mainsHz = 50;  // 50 or 60
    uint32_t fps = 60;      // camera and output frame rate, 30 or 60
    bool alwaysOn = false;  // stream whenever the camera is connected, watched or not
    std::string camera;     // capture node; empty: the PS5 camera found by its USB id
    std::string output;     // v4l2loopback node; empty: the one named "PS5 Camera"
};

// Parses a config file's text. Problems are logged with the file name and line; an unknown key only
// the first time (warnedKeys remembers them across reloads).
BokehConfig ParseConfig(std::string_view text, const std::string& path, std::set<std::string>& warnedKeys);

// Reads the file at path into config: the defaults when it does not exist or cannot be read (logged).
// False, with config untouched, when the file changed while it was read (an editor writing it):
// read it again later.
bool LoadConfig(const std::string& path, std::set<std::string>& warnedKeys, BokehConfig& config);

// The settings as the GPU pipeline takes them.
inline void ApplyConfig(const BokehConfig& c, EffectSettings& e)
{
    ApplyPicture(c, e);
}

// The effective values on one line, for the log (and to tell whether a re-read changed anything).
std::string DescribeConfig(const BokehConfig& c);

// Whole-string numbers only: false for a sign, spaces, trailing text or overflow.
bool ParseUint(std::string_view text, uint32_t& value);
bool ParseFloat(const std::string& text, float& value);

// What stat says about a file, enough to see that it was written, replaced, created or removed.
struct FileStamp {
    bool exists = false;
    int64_t mtimeNs = 0;
    int64_t size = 0;
    uint64_t inode = 0;
    bool operator==(const FileStamp&) const = default;
};
FileStamp StatFile(const std::string& path);

// Says when a file changed since the last call.
class FileWatch {
public:
    explicit FileWatch(std::string path) : m_path(std::move(path)) {}
    const std::string& Path() const { return m_path; }
    // True on the first call and whenever the file differs from the previous call.
    bool Changed();

private:
    std::string m_path;
    bool m_first = true;
    FileStamp m_last;
};

}  // namespace ps5cam
