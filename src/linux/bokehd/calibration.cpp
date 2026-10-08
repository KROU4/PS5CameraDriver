#include "calibration.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "config.h"
#include "log.h"

namespace ps5cam {

namespace {

constexpr const char* kFileName = "calibration";

bool WriteAll(int fd, const char* data, size_t size)
{
    while (size > 0) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

std::string StateDirectory()
{
    // systemd passes a colon-separated list when a unit has several state directories.
    const char* env = getenv("STATE_DIRECTORY");
    if (env && *env) {
        const std::string dirs = env;
        return dirs.substr(0, dirs.find(':'));
    }
    return "/var/lib/ps5cam";
}

bool LoadCalibration(const std::string& dir, Rectification& r)
{
    const std::string path = dir + "/" + kFileName;
    FILE* f = fopen(path.c_str(), "re");
    if (!f) {
        if (errno != ENOENT) Log("%s: %s, calibrating again", path.c_str(), strerror(errno));
        return false;
    }
    char line[128] = {};
    const bool read = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    const size_t space = text.find(' ');
    Rectification loaded;
    if (!read || space == std::string::npos || !ParseFloat(text.substr(0, space), loaded.dy) ||
        !ParseFloat(text.substr(space + 1), loaded.rotation)) {
        Log("%s: not \"dy rotation\", calibrating again", path.c_str());
        return false;
    }
    if (std::fabs(loaded.dy) > kMaxCalibrationDy || std::fabs(loaded.rotation) > kMaxCalibrationRotation) {
        Log("%s: dy %.3f rotation %.3f is out of range, calibrating again", path.c_str(), loaded.dy, loaded.rotation);
        return false;
    }
    r = loaded;
    return true;
}

bool SaveCalibration(const std::string& dir, const Rectification& r)
{
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        Log("cannot create %s: %s", dir.c_str(), strerror(errno));
        return false;
    }
    const std::string path = dir + "/" + kFileName;
    std::string temp = path + ".XXXXXX";
    std::vector<char> name(temp.begin(), temp.end());
    name.push_back('\0');
    const int fd = mkostemp(name.data(), O_CLOEXEC);
    if (fd < 0) {
        Log("cannot save the calibration in %s: %s", dir.c_str(), strerror(errno));
        return false;
    }
    temp = name.data();
    char text[64];
    const int len = snprintf(text, sizeof(text), "%.3f %.3f\n", r.dy, r.rotation);
    // mkostemp makes the file 0600; it is no secret, and readable it helps when reporting problems.
    bool ok = fchmod(fd, 0644) == 0 && len > 0 && WriteAll(fd, text, static_cast<size_t>(len)) && fsync(fd) == 0;
    int error = ok ? 0 : errno;
    if (close(fd) != 0 && ok) ok = false, error = errno;
    if (ok && rename(temp.c_str(), path.c_str()) != 0) ok = false, error = errno;
    if (!ok) {
        Log("cannot save the calibration to %s: %s", path.c_str(), strerror(error ? error : EIO));
        unlink(temp.c_str());
        return false;
    }
    // The rename lasts through a power cut only once the directory is on disk too. Best effort:
    // the worst case is calibrating again.
    if (const int dirFd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dirFd >= 0) {
        fsync(dirFd);
        close(dirFd);
    }
    return true;
}

}  // namespace ps5cam
