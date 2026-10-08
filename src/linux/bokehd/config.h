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

struct FileStamp;

// Reads the file at path into config: the defaults when it does not exist or cannot be read (logged).
// False, with config untouched, when the file changed while it was read (an editor writing it):
// read it again later. stamp (optional) gets the version that was read.
bool LoadConfig(const std::string& path, std::set<std::string>& warnedKeys, BokehConfig& config,
    FileStamp* stamp = nullptr);

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

// Says when to read a file again: once it differs from the version last read and has stayed the
// same for one poll interval, so that an editor that truncates and then writes it is never caught
// in between (stat may look the same before and after a read that falls into the gap).
class FileWatch {
public:
    explicit FileWatch(std::string path) : m_path(std::move(path)) {}
    const std::string& Path() const { return m_path; }
    // Called on every poll.
    bool Settled();
    // The version that was read (LoadConfig's stamp).
    void MarkRead(const FileStamp& read)
    {
        m_read = read;
        m_haveRead = true;
    }

private:
    std::string m_path;
    FileStamp m_previous;  // at the previous poll
    bool m_havePrevious = false;
    FileStamp m_read;
    bool m_haveRead = false;
};

}  // namespace ps5cam
