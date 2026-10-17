// Самообновление драйвера: версии, SHA-256, разбор предложения сервера, кэш и скачивание.
// Файлы и потоки — через платформенный слой (platform.h).
#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>

#include "platform.h"

// Имя файла драйвера этой сборки (EmulkonDriver_win64.dll, libEmulkonDriver_lin64.so) — задаёт CMake.
#ifndef EMULKON_DLL_FILE
#define EMULKON_DLL_FILE "EmulkonDriver.dll"
#endif

namespace emu {

namespace {

bool parseVersion(std::string v, int out[3]) {
    size_t dash = v.find('-');
    if (dash != std::string::npos) v.resize(dash);
    int part = 0;
    size_t i = 0;
    while (part < 3) {
        if (i >= v.size() || v[i] < '0' || v[i] > '9') return false;
        long n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            n = n * 10 + (v[i] - '0');
            if (n > 1000000) return false;
            ++i;
        }
        out[part++] = static_cast<int>(n);
        if (part < 3) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    return i == v.size();
}

// --- SHA-256 (FIPS 180-4) ---

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void block(uint32_t h[8], const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
               uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

}  // namespace

bool validVersion(const std::string& v) {
    int x[3];
    return parseVersion(v, x);
}

int compareVersions(const std::string& a, const std::string& b) {
    int va[3], vb[3];
    bool oa = parseVersion(a, va), ob = parseVersion(b, vb);
    if (!oa && !ob) return 0;
    if (!oa) return -1;
    if (!ob) return 1;
    for (int i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

std::string sha256Hex(const std::string& data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size(), full = n / 64 * 64;
    for (size_t i = 0; i < full; i += 64) block(h, p + i);
    unsigned char tail[128] = {};
    size_t rest = n - full;
    for (size_t i = 0; i < rest; ++i) tail[i] = p[full + i];
    tail[rest] = 0x80;
    size_t tailLen = rest < 56 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    block(h, tail);
    if (tailLen == 128) block(h, tail + 64);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
}

bool UpdateOffer::valid() const {
    return validVersion(version) && url.size() > 1 && url[0] == '/' && url.find("..") == std::string::npos &&
           sha256.size() == 64;
}

UpdateOffer parseUpdateOffer(const Json* j) {
    UpdateOffer o;
    if (!j || !j->isObject()) return o;
    if (const Json* v = j->get("version")) o.version = v->text;
    if (const Json* v = j->get("url")) o.url = v->text;
    if (const Json* v = j->get("sha256")) o.sha256 = v->text;
    if (const Json* v = j->get("size")) o.size = v->asInt();
    std::transform(o.sha256.begin(), o.sha256.end(), o.sha256.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c); });
    return o;
}

// driverArch — архитектура этой сборки для сервера (driver_arch, arch=). По компилятору, а не по разрядности:
// драйвер ARM64 не должен назваться x86_64 и получить чужую библиотеку самообновления (issue #12).
const char* driverArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#else
#error "неизвестная архитектура: добавьте её в driverArch и на сервер (downloads.go)"
#endif
}

std::vector<std::string> newerVersions(const std::vector<std::string>& names, const std::string& own) {
    std::vector<std::string> out;
    for (const auto& n : names) {
        if (validVersion(n) && n.find('-') == std::string::npos && compareVersions(n, own) > 0) out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return compareVersions(a, b) > 0; });
    return out;
}
const char* driverOS() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

// --- кэш обновлений: файлы через платформенный слой (platform.h) ---

namespace {

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// looksLikeDriver — заголовок файла: DLL Windows начинается с «MZ», библиотека Linux — с ELF.
bool looksLikeDriver(const std::string& data) {
#ifdef _WIN32
    return data.size() >= 2 && data[0] == 'M' && data[1] == 'Z';
#else
    return data.size() >= 4 && data.compare(0, 4, "\x7f" "ELF") == 0;
#endif
}

// cleanupOlder удаляет из кэша драйвер этой архитектуры старше version. Загруженную в другом
// процессе 1С DLL Windows удалить не даст — она останется до следующего раза.
void cleanupOlder(const std::string& version) {
    const std::string root = updateCacheRoot();
    for (const std::string& name : sys::listDirs(root)) {
        if (validVersion(name) && compareVersions(name, version) < 0) {
            sys::removeFile(cachedDllPath(name));
            sys::removeDir(root + sys::kSep + name);  // только если каталог опустел
        }
    }
}

std::mutex g_mu;
std::set<std::string> g_started;  // версии, за которыми уже ходили в этом процессе

}  // namespace

std::string updateCacheRoot() {
    std::string custom = sys::env("EMULKON_DRIVER_CACHE");
    return custom.empty() ? sys::defaultCacheRoot() : custom;
}

std::string cachedDllPath(const std::string& version) {
    return updateCacheRoot() + sys::kSep + version + sys::kSep + EMULKON_DLL_FILE;
}

std::vector<std::string> cachedVersions() {
    std::vector<std::string> out;
    const std::string root = updateCacheRoot();
    if (root.empty()) return out;
    for (const std::string& name : sys::listDirs(root)) {
        if (validVersion(name) && sys::isFile(cachedDllPath(name))) out.push_back(name);
    }
    return out;
}

bool updateCached(const UpdateOffer& offer) {
    std::string data;
    return offer.valid() && sys::readFile(cachedDllPath(offer.version), data) && sha256Hex(data) == offer.sha256;
}

bool redirectDisabled() { return sys::env("EMULKON_DRIVER_NO_UPDATE") == "1"; }

void rememberServer(const std::string& url) {
    const std::string root = updateCacheRoot();
    if (root.empty() || url.empty()) return;
    if (rememberedServer() == url) return;
    if (sys::ensureDir(root)) sys::writeFile(root + sys::kSep + "server.txt", url);
}

std::string rememberedServer() {
    std::string s;
    const std::string root = updateCacheRoot();
    if (root.empty() || !sys::readFile(root + sys::kSep + "server.txt", s)) return {};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

bool installUpdate(HttpClient& http, const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                   const std::string& workstation, std::string& message) {
    if (!offer.valid()) {
        message = "сервер прислал неверное описание обновления";
        return false;
    }
    const std::string root = updateCacheRoot();
    if (root.empty()) {
        message = "не определить каталог кэша обновлений (LOCALAPPDATA, XDG_CACHE_HOME или HOME)";
        return false;
    }
    if (updateCached(offer)) return true;

    Headers headers;
    if (!workstation.empty()) headers.emplace_back("X-Emulkon-Workstation", workstation);
    HttpResponse r = http.get(trimSlash(serverUrl) + offer.url, headers, timeoutMs > 0 ? timeoutMs : 60000);
    if (!r.error.empty()) {
        message = "скачать " + offer.url + ": " + r.error;
        return false;
    }
    if (r.status != 200) {
        message = "скачать " + offer.url + ": HTTP " + std::to_string(r.status);
        return false;
    }
    if (offer.size > 0 && static_cast<int64_t>(r.body.size()) != offer.size) {
        message = "скачано " + std::to_string(r.body.size()) + " байт вместо " + std::to_string(offer.size);
        return false;
    }
    if (sha256Hex(r.body) != offer.sha256) {
        message = "контрольная сумма скачанного файла не совпала с объявленной сервером";
        return false;
    }
    if (!looksLikeDriver(r.body)) {
        message = "скачанный файл — не библиотека драйвера для этой ОС";
        return false;
    }

    const std::string dir = root + sys::kSep + offer.version;
    if (!sys::ensureDir(dir)) {
        message = "создать каталог " + dir + ": " + sys::lastError();
        return false;
    }
    // Сначала во временный файл, потом переименование: недокачанный драйвер не получит рабочее имя.
    const std::string path = cachedDllPath(offer.version);
    const std::string tmp = path + ".tmp" + std::to_string(sys::processId());
    if (!sys::writeFile(tmp, r.body)) {
        message = "записать " + tmp + ": " + sys::lastError();
        return false;
    }
    if (!sys::replaceFile(tmp, path)) {
        std::string why = sys::lastError();
        sys::removeFile(tmp);
        if (updateCached(offer)) return true;  // параллельно скачал другой процесс 1С
        message = "сохранить " + path + ": " + why;
        return false;
    }
    cleanupOlder(offer.version);
    return true;
}

UpdateCheck checkForUpdate(HttpClient& http, const std::string& serverUrl, int timeoutMs, const std::string& workstation,
                           std::string& message) {
    const std::string server = trimSlash(serverUrl);
    HttpResponse r = http.get(server + "/api/v1/driver/update?version=" EMULKON_VERSION "&arch=" + driverArch() +
                                  "&os=" + driverOS(),
                              {}, timeoutMs);
    if (!r.error.empty()) {
        message = "Нет связи с сервером эмуляции " + server + ": " + r.error;
        return UpdateCheck::Failed;
    }
    if (r.status == 404) {
        message = "Сервер эмуляции " + server + " не раздаёт обновления драйвера (сервер старше версии 0.2.0)";
        return UpdateCheck::Failed;
    }
    Json resp;
    std::string err;
    if (r.status != 200 || !parseJson(r.body, resp, err)) {
        message = "Сервер эмуляции ответил на проверку обновления не по протоколу (HTTP " + std::to_string(r.status) + ")";
        return UpdateCheck::Failed;
    }
    const Json* avail = resp.get("update_available");
    UpdateOffer offer = parseUpdateOffer(resp.get("latest"));
    if (!(avail && avail->isBool() && avail->b)) {
        message = "Установлена последняя версия драйвера: " EMULKON_VERSION ".";
        if (!offer.valid()) message += " На сервере " + server + " файлы драйвера не опубликованы.";
        return UpdateCheck::UpToDate;
    }
    if (!offer.valid()) {
        message = "Сервер эмуляции прислал неверное описание обновления";
        return UpdateCheck::Failed;
    }
    if (updateCached(offer)) {
        message = "Версия драйвера " + offer.version + " уже загружена и начнёт работать после перезапуска 1С (сейчас " +
                  EMULKON_VERSION + ").";
        return UpdateCheck::Pending;
    }
    std::string why;
    if (!installUpdate(http, server, offer, timeoutMs, workstation, why)) {
        message = "Не удалось загрузить версию драйвера " + offer.version + ": " + why;
        return UpdateCheck::Failed;
    }
    message = "Загружена версия драйвера " + offer.version + ". Она начнёт работать после перезапуска 1С (сейчас " +
              EMULKON_VERSION + ").";
    return UpdateCheck::Downloaded;
}

void startBackgroundUpdate(const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                           const std::string& workstation) {
    if (!offer.valid()) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_started.insert(offer.version).second) return;
    }
    if (updateCached(offer)) return;
    sys::runDetached([serverUrl, offer, timeoutMs, workstation]() {
        std::unique_ptr<HttpClient> http = makeHttpClient(std::string("Emulkon-Driver/") + EMULKON_VERSION);
        std::string message;
        installUpdate(*http, serverUrl, offer, timeoutMs, workstation, message);
    });
}

}  // namespace emu
