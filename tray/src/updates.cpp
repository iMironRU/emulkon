#include "updates.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <cwctype>

#include "doc.h"
#include "json.h"

namespace tray {

namespace {

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring str(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isString() ? widen(v->text) : std::wstring();
}

// versionOk — X.Y.Z[-метка]: только цифры, буквы, точки и дефис (версия идёт в путь и адрес).
bool versionOk(const std::wstring& v) {
    if (v.empty() || v.size() > 32 || !std::iswdigit(v[0])) return false;
    for (wchar_t c : v)
        if (!(std::iswdigit(c) || (c < 128 && std::iswalpha(c)) || c == L'.' || c == L'-')) return false;
    return v.find(L"..") == std::wstring::npos;
}

std::wstring winError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s + L" (код " + std::to_wstring(code) + L")";
}

bool writeAll(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    const DWORD code = GetLastError();
    CloseHandle(f);
    SetLastError(code);
    return ok;
}

}  // namespace

bool parseUpdateState(const std::string& json, UpdateInfo& out) {
    emu::Json doc;
    std::string err;
    if (!emu::parseJson(json, doc, err) || !doc.isObject()) return false;
    out = UpdateInfo{};
    const emu::Json* avail = doc.get("available");
    out.available = avail && avail->isBool() && avail->b;
    out.current = str(doc, "current");
    out.page = str(doc, "page");
    if (const emu::Json* l = doc.get("latest"); l && l->isObject()) {
        out.version = str(*l, "version");
        out.published = str(*l, "published");
        out.url = str(*l, "url");
        out.sha256 = lower(str(*l, "sha256"));
        if (const emu::Json* s = l->get("size"); s && s->isInteger() && s->asInt() > 0) out.size = s->asInt();
    }
    if (out.version.empty()) out.available = false;
    return true;
}

bool isLocalServer(const std::wstring& url) {
    std::wstring u = lower(url);
    size_t p = u.find(L"://");
    if (p != std::wstring::npos) u = u.substr(p + 3);
    u = u.substr(0, u.find(L'/'));
    std::wstring host = !u.empty() && u[0] == L'[' ? u.substr(0, u.find(L']') + 1) : u.substr(0, u.find(L':'));
    return host == L"127.0.0.1" || host == L"localhost" || host == L"[::1]";
}

bool downloadAllowed(const std::wstring& url, const std::wstring& version, bool localServer) {
    if (!versionOk(version)) return false;
    const std::wstring file = L"/EmulkonSetup_" + version + L".exe";
    if (url.size() <= file.size() || url.compare(url.size() - file.size(), file.size(), file) != 0) return false;
    if (url.find(L"..") != std::wstring::npos || url.find(L'?') != std::wstring::npos || url.find(L'#') != std::wstring::npos) {
        return false;
    }
    if (url == L"https://github.com/iMironRU/emulkon/releases/download/v" + version + file) return true;
    return localServer && (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0);
}

std::wstring sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::wstring out;
    unsigned char digest[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), digest, sizeof digest) == 0) {
        static const wchar_t* hex = L"0123456789abcdef";
        for (unsigned char b : digest) {
            out += hex[b >> 4];
            out += hex[b & 15];
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool saveInstaller(const std::string& body, const UpdateInfo& u, const std::wstring& dir, std::wstring& path,
                   std::wstring& err) {
    if (!versionOk(u.version)) {
        err = L"версия выпуска не по образцу: " + u.version;
        return false;
    }
    if (u.size > 0 && static_cast<long long>(body.size()) != u.size) {
        err = L"размер установщика " + std::to_wstring(body.size()) + L" байт, в описании выпуска — " + std::to_wstring(u.size);
        return false;
    }
    if (u.sha256.size() != 64 || sha256Hex(body) != u.sha256) {
        err = L"контрольная сумма SHA-256 установщика не совпала с описанием выпуска — файл не сохранён";
        return false;
    }
    if (dir.empty()) {
        err = L"не найдена папка «Загрузки»";
        return false;
    }
    path = dir + L"\\EmulkonSetup_" + u.version + L".exe";
    const std::wstring part = path + L".part";
    if (!writeAll(part, body.data(), static_cast<DWORD>(body.size())) ||
        !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = path + L": " + winError(GetLastError());
        DeleteFileW(part.c_str());
        return false;
    }
    // Метка «из Интернета», как у браузера: Windows проверит файл при запуске (SmartScreen, антивирус) как
    // обычную загрузку.
    const std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=" + narrow(u.url) + "\r\n";
    writeAll(path + L":Zone.Identifier", zone.data(), static_cast<DWORD>(zone.size()));
    return true;
}

std::wstring userDownloadsDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}

}  // namespace tray
