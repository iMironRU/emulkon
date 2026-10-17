#include "updates.h"

#include <windows.h>
#include <bcrypt.h>

#include <cwctype>
#include <vector>

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
    out.installer = str(doc, "installer");
    if (const emu::Json* l = doc.get("latest"); l && l->isObject()) {
        out.version = str(*l, "version");
        out.published = str(*l, "published");
        out.sha256 = lower(str(*l, "sha256"));
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

bool installerAllowed(const std::wstring& path, const std::wstring& version, const std::wstring& programData) {
    if (path.empty() || version.empty() || programData.empty()) return false;
    if (path.find(L"..") != std::wstring::npos || path.find(L'/') != std::wstring::npos) return false;
    for (wchar_t c : version)
        if (!(std::iswdigit(c) || c == L'.' || c == L'-' || std::iswalpha(c))) return false;
    std::wstring dir = programData;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    const std::wstring want = lower(dir + L"\\Emulkon\\updates\\EmulkonSetup_" + version + L".exe");
    return lower(path) == want;
}

std::wstring fileSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16);
        DWORD n = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            if (BCryptHashData(hash, buf.data(), n, 0) != 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            static const wchar_t* hex = L"0123456789abcdef";
            for (unsigned char b : digest) {
                out += hex[b >> 4];
                out += hex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

}  // namespace tray
