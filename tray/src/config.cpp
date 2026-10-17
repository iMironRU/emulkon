#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "doc.h"

namespace tray {

namespace {

const struct {
    const char* key;
    std::wstring Config::*str;
    bool Config::*flag;
    int Config::*num;
} kFields[] = {
    {"ServerURL", &Config::serverUrl, nullptr, nullptr},
    {"ControlToken", &Config::controlToken, nullptr, nullptr},
    {"Workstation", &Config::workstation, nullptr, nullptr},
    {"PopupSeconds", nullptr, nullptr, &Config::popupSeconds},
    {"PrintOutputDir", &Config::printOutputDir, nullptr, nullptr},
    {"ServerExe", &Config::serverExe, nullptr, nullptr},
    {"ServerData", &Config::serverData, nullptr, nullptr},
    {"ServerListen", &Config::serverListen, nullptr, nullptr},
    {"ServerAutostart", nullptr, &Config::serverAutostart, nullptr},
    {"UpdateNotified", &Config::updateNotified, nullptr, nullptr},
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

}  // namespace

std::string serializeConfig(const Config& c) {
    std::string out = "# Настройки трея Эмулкон. Редактируются из меню трея (Настройки…).\n";
    for (const auto& f : kFields) {
        out += f.key;
        out += '=';
        if (f.str) out += narrow(c.*(f.str));
        if (f.flag) out += (c.*(f.flag)) ? "1" : "0";
        if (f.num) out += std::to_string(c.*(f.num));
        out += '\n';
    }
    return out;
}

void parseConfig(const std::string& text, Config& c) {
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // BOM
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (const auto& f : kFields) {
            if (key != f.key) continue;
            if (f.str) c.*(f.str) = widen(value);
            if (f.flag) c.*(f.flag) = value == "1" || value == "true" || value == "yes";
            if (f.num) {
                int n = std::atoi(value.c_str());
                if (n > 0) c.*(f.num) = n;
            }
        }
    }
}

std::wstring defaultConfigPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\Emulkon";
        CoTaskMemFree(appdata);
    } else {
        dir = L".";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\tray.ini";
}

bool loadConfig(const std::wstring& path, Config& c) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    parseConfig(ss.str(), c);
    return true;
}

bool saveConfig(const std::wstring& path, const Config& c) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serializeConfig(c);
    return static_cast<bool>(f);
}

std::wstring computerName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return std::wstring(buf, n);
    return L"WORKSTATION";
}

std::wstring effectiveWorkstation(const Config& c) {
    return c.workstation.empty() ? computerName() : c.workstation;
}

}  // namespace tray
