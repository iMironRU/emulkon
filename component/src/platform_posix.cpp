// Платформенный слой компоненты — Linux и другие POSIX-системы (platform.h).
#include "platform.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace emu {
namespace sys {

const char kSep = '/';

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void setEnv(const char* name, const char* value) {
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

bool isDir(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ensureDir(const std::string& path) {
    if (isDir(path)) return true;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (!part.empty() && !isDir(part)) mkdir(part.c_str(), 0700);
        }
    }
    return isDir(path);
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > (64u << 20)) {
            ok = false;
            break;
        }
    }
    ok = ok && !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (!ok) unlink(path.c_str());
    return ok;
}

bool replaceFile(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }

void removeFile(const std::string& path) { unlink(path.c_str()); }

void removeDir(const std::string& path) { rmdir(path.c_str()); }

std::vector<std::string> listDirs(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (isDir(path + "/" + name)) out.push_back(name);
    }
    closedir(d);
    return out;
}

std::string tempDir() {
    std::string t = env("TMPDIR");
    if (t.empty()) t = "/tmp";
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    return t;
}

long processId() { return static_cast<long>(getpid()); }

std::string lastError() { return std::strerror(errno); }

std::string defaultCacheRoot() {
    std::string base = env("XDG_CACHE_HOME");
    if (base.empty()) {
        std::string home = env("HOME");
        if (home.empty()) return {};
        base = home + "/.cache";
    }
    return base + "/emulkon/driver";
}

void runDetached(std::function<void()> fn) {
    // Компонента остаётся в памяти до конца процесса: dlopen с RTLD_NODELETE на свою же библиотеку.
    // В отличие от Windows, безопасно отпустить её из собственного потока нельзя.
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&runDetached), &info) && info.dli_fname) {
        dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }
    try {
        std::thread(std::move(fn)).detach();
    } catch (...) {
        // нет потока — нет фонового скачивания; ручная проверка обновления остаётся
    }
}

}  // namespace sys
}  // namespace emu
