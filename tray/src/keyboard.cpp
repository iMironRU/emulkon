// Ввод с клавиатуры: сканер или считыватель в режиме клавиатуры (HID) — трей вводит код в
// активное окно, как настоящий сканер (docs/models/input.md).
#include <windows.h>

#include <string>
#include <vector>

#include "ui.h"

namespace tray {

namespace {

void addKey(std::vector<INPUT>& in, WORD vk) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    in.push_back(down);
    in.push_back(up);
}

void addChar(std::vector<INPUT>& in, wchar_t c) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = c;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    in.push_back(down);
    in.push_back(up);
}

}  // namespace

std::vector<INPUT> keyboardInputs(const std::wstring& text, const std::string& suffix) {
    std::vector<INPUT> in;
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n') {
            addKey(in, VK_RETURN);  // суффикс дорожки карты (13) — как Enter сканера
        } else if (c == L'\t') {
            addKey(in, VK_TAB);
        } else {
            addChar(in, c);  // в т.ч. GS (29) кода маркировки — символом, как сканер в режиме Unicode
        }
    }
    if (suffix == "enter") addKey(in, VK_RETURN);
    if (suffix == "tab") addKey(in, VK_TAB);
    return in;
}

bool typeText(const std::wstring& text, const std::string& suffix, std::wstring& error) {
    std::vector<INPUT> in = keyboardInputs(text, suffix);
    if (in.empty()) return true;
    UINT sent = SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
    if (sent != in.size()) {
        // Окно администратора (UIPI) или блокировка рабочего стола — ввод не прошёл.
        error = L"Windows приняла " + std::to_wstring(sent) + L" из " + std::to_wstring(in.size()) +
                L" нажатий (код ошибки " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

}  // namespace tray
