#include <cstdlib>
#include <string>
#include <vector>

#include "ui.h"

namespace tray {

namespace {

const wchar_t* kSettingsClass = L"EmulkonTraySettings";
const int kIdOk = 1, kIdCancel = 2, kIdEditBase = 100;

struct Field {
    const wchar_t* label;
    std::wstring* value;
    bool password;
};

struct SettingsState {
    Config* cfg = nullptr;
    std::wstring popupSeconds;
    std::vector<Field> fields;
    std::vector<HWND> edits;
    bool saved = false;
    bool done = false;
    HFONT font = nullptr;
};

LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kIdOk && st) {
                for (size_t i = 0; i < st->fields.size(); ++i) {
                    int n = GetWindowTextLengthW(st->edits[i]);
                    std::wstring v(static_cast<size_t>(n) + 1, L'\0');
                    GetWindowTextW(st->edits[i], &v[0], n + 1);
                    v.resize(static_cast<size_t>(n));
                    *st->fields[i].value = v;
                }
                int secs = _wtoi(st->popupSeconds.c_str());
                if (secs > 0) st->cfg->popupSeconds = secs;
                st->saved = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == kIdCancel) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool showSettings(HINSTANCE inst, HWND owner, Config& cfg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = settingsProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    SettingsState st;
    st.cfg = &cfg;
    st.popupSeconds = std::to_wstring(cfg.popupSeconds);
    std::wstring wsHint = L"Рабочее место (пусто — " + computerName() + L"; * — все)";
    st.fields = {
        {L"Адрес сервера Эмулкон", &cfg.serverUrl, false},
        {L"Токен Control API (EMULKON_CONTROL_TOKEN), если задан", &cfg.controlToken, true},
        {wsHint.c_str(), &cfg.workstation, false},
        {L"Показывать чек на экране, секунд", &st.popupSeconds, false},
        {L"Каталог для PDF/XPS-принтера (сохранять без диалога; необязательно)", &cfg.printOutputDir, false},
        {L"Локальный сервер: путь к emulkon.exe (для «Запустить сервер»)", &cfg.serverExe, false},
        {L"Локальный сервер: каталог данных", &cfg.serverData, false},
        {L"Локальный сервер: слушать адрес (пусто — из адреса сервера; 0.0.0.0:8737 — кассы в сети)", &cfg.serverListen, false},
    };

    HDC screen = GetDC(nullptr);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    auto S = [dpi](int px) { return MulDiv(px, dpi, 96); };
    st.font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    int width = S(520), rowH = S(50), top = S(14);
    int height = top + static_cast<int>(st.fields.size()) * rowH + S(60);
    RECT rc{0, 0, width, height};
    AdjustWindowRectEx(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kSettingsClass, L"Эмулкон — настройки трея",
                                WS_CAPTION | WS_SYSMENU, x, y, ww, wh, owner, nullptr, inst, &st);
    if (!hwnd) return false;

    for (size_t i = 0; i < st.fields.size(); ++i) {
        int rowY = top + static_cast<int>(i) * rowH;
        HWND label = CreateWindowExW(0, L"STATIC", st.fields[i].label, WS_CHILD | WS_VISIBLE, S(14), rowY, width - S(28),
                                     S(18), hwnd, nullptr, inst, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (st.fields[i].password ? ES_PASSWORD : 0);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st.fields[i].value->c_str(), style, S(14), rowY + S(19),
                                    width - S(28), S(24), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditBase + i)),
                                    inst, nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        st.edits.push_back(edit);
    }
    int by = height - S(44);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Сохранить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              width - S(234), by, S(106), S(30), hwnd, reinterpret_cast<HMENU>(kIdOk), inst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP, width - S(120), by, S(106),
                                  S(30), hwnd, reinterpret_cast<HMENU>(kIdCancel), inst, nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(st.edits[0]);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {  // Tab, Enter, Esc
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(st.font);
    return st.saved;
}

}  // namespace tray
