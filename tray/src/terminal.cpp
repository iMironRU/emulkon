// Окно устройства, которое ждёт человека (device.prompt): на терминале «прикладывают»
// карту, на весах вводят вес. Ответ уходит на сервер (POST …/prompt), итог приходит
// записью device.prompt_done — окно показывает его и закрывается само.
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cwchar>

#include "ui.h"

namespace tray {

namespace {

const wchar_t* kClass = L"EmulkonTerminal";
const int kIdCombo = 10, kIdEdit = 11, kIdActionBase = 100;
const UINT_PTR kTimerTick = 1, kTimerClose = 2;

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TerminalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<TerminalWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring money(double v) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f ₽", v);
    return buf;
}

std::wstring number3(double v) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f", v);
    return buf;
}

// parseNumber — «1,245» и «1.245»; false — не число.
bool parseNumber(std::wstring s, double& v) {
    for (auto& c : s)
        if (c == L',') c = L'.';
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    v = std::wcstod(s.c_str(), &end);
    while (end && iswspace(*end)) ++end;
    return end && *end == 0;
}

}  // namespace

bool TerminalWindow::create(HINSTANCE inst, OnAnswer onAnswer) {
    inst_ = inst;
    onAnswer_ = std::move(onAnswer);
    dpi_ = static_cast<int>(GetDpiForSystem());
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    auto font = [this](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, dpi_, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    fontUi_ = font(9, FW_NORMAL, L"Segoe UI");
    fontSmall_ = font(9, FW_NORMAL, L"Consolas");
    fontText_ = font(10, FW_NORMAL, L"Consolas");
    fontBig_ = font(22, FW_BOLD, L"Consolas");
    fontVerdict_ = font(15, FW_BOLD, L"Consolas");

    RECT rc{0, 0, scale(380), scale(300)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, static_cast<UINT>(dpi_));
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Эмулкон — терминал", style, 0, 0,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    combo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             scale(12), scale(154), scale(156), scale(200), hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCombo)), inst, nullptr);
    SendMessageW(combo_, WM_SETFONT, reinterpret_cast<WPARAM>(fontUi_), TRUE);
    edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
                            scale(12), scale(153), scale(156), scale(28), hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEdit)), inst, nullptr);
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontText_), TRUE);
    return true;
}

bool TerminalWindow::readValue(double& v) const {
    wchar_t buf[64]{};
    GetWindowTextW(edit_, buf, 64);
    if (!parseNumber(buf, v) || v < p_.inputMin || (p_.inputMax > 0 && v > p_.inputMax)) return false;
    v = static_cast<double>(static_cast<long long>(v * 1000 + 0.5)) / 1000;  // граммы
    return true;
}

void TerminalWindow::clearButtons() {
    for (HWND b : buttons_) DestroyWindow(b);
    buttons_.clear();
}

void TerminalWindow::show(const PromptEvent& p) {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerClose);
    p_ = p;
    active_ = true;
    finished_ = false;
    approved_ = false;
    status_.clear();

    // Карты: выбор по умолчанию — как на сервере (card).
    SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < p.cards.size(); ++i) {
        SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.cards[i].title.c_str()));
        if (p.cards[i].id == p.defaultCard) sel = static_cast<int>(i);
    }
    SendMessageW(combo_, CB_SETCURSEL, sel, 0);
    ShowWindow(combo_, p.cards.empty() ? SW_HIDE : SW_SHOW);
    // Поле ввода (вес): значение по умолчанию — с сервера.
    SetWindowTextW(edit_, number3(p.inputValue).c_str());
    ShowWindow(edit_, p.hasInput ? SW_SHOW : SW_HIDE);

    // Кнопки: главная («Приложить карту», «Взвесить») — рядом со списком карт или полем,
    // остальные — сеткой по две.
    clearButtons();
    int other = 0;
    bool primaryPlaced = false;
    for (size_t i = 0; i < p.actions.size(); ++i) {
        const PromptOption& a = p.actions[i];
        int x, y, w;
        bool primary = !primaryPlaced && (a.id == "card" || a.kind == "primary");
        if (primary) {
            primaryPlaced = true;
            x = 176, y = 152, w = 192;
        } else {
            x = 12 + (other % 2) * 180, y = 192 + (other / 2) * 34, w = 176;
            ++other;
        }
        HWND b = CreateWindowExW(0, L"BUTTON", a.title.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                 scale(x), scale(y), scale(w), scale(30), hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdActionBase + i)), inst_, nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(fontUi_), TRUE);
        buttons_.push_back(b);
    }

    std::wstring caption = (p.hasInput ? L"Эмулкон — весы " : L"Эмулкон — терминал ") + widen(p.deviceId);
    SetWindowTextW(hwnd_, caption.c_str());
    // По центру рабочей области: операция ждёт кассира, окно должно быть на виду.
    RECT work{}, wr{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    GetWindowRect(hwnd_, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd_, HWND_TOPMOST, work.left + (work.right - work.left - w) / 2,
                 work.top + (work.bottom - work.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(hwnd_, kTimerTick, 1000, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void TerminalWindow::reopen() {
    if (active_ && hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void TerminalWindow::finish(const PromptEvent& done) {
    if (!hwnd_ || !active_ || done.promptId != p_.promptId) return;
    finished_ = true;
    approved_ = done.action == "card" || done.action == "weigh";
    status_ = done.resultText.empty() ? (approved_ ? L"ОДОБРЕНО" : L"ОТКАЗ") : done.resultText;
    if (done.hasValue) p_.inputValue = done.value;
    clearButtons();
    ShowWindow(combo_, SW_HIDE);
    ShowWindow(edit_, SW_HIDE);
    KillTimer(hwnd_, kTimerTick);
    if (IsWindowVisible(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        SetTimer(hwnd_, kTimerClose, approved_ ? 2500 : 5000, nullptr);
    } else {
        active_ = false;
    }
}

void TerminalWindow::answerSent(bool ok, const std::wstring& message) {
    if (!hwnd_ || !active_ || finished_) return;
    status_ = message;
    if (!ok) {  // ответ не дошёл — кнопки снова доступны
        for (HWND b : buttons_) EnableWindow(b, TRUE);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void TerminalWindow::paint(HDC dc, int width, int) {
    RECT disp{scale(12), scale(12), width - scale(12), scale(142)};
    COLORREF bg = RGB(0x0f, 0x1a, 0x14), fg = RGB(0xd7, 0xf5, 0xdf), border = RGB(0x23, 0x35, 0x2a);
    if (finished_ && !approved_) bg = RGB(0x1d, 0x12, 0x12), fg = RGB(0xff, 0xd9, 0xd9), border = RGB(0xa3, 0x3a, 0x3a);
    if (finished_ && approved_) border = RGB(0x2e, 0x7d, 0x46);
    HBRUSH b = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, scale(2), border);
    HGDIOBJ oldB = SelectObject(dc, b), oldP = SelectObject(dc, pen);
    RoundRect(dc, disp.left, disp.top, disp.right, disp.bottom, scale(12), scale(12));
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(b);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    int x = disp.left + scale(14), y = disp.top + scale(10);
    auto line = [&](HFONT f, const std::wstring& s, int h, COLORREF c) {
        SelectObject(dc, f);
        SetTextColor(dc, c);
        RECT r{x, y, disp.right - scale(10), y + scale(h)};
        DrawTextW(dc, s.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += scale(h);
    };
    line(fontSmall_, p_.title, 18, fg);
    if (p_.hasInput) {  // весы: на табло — вес из поля
        wchar_t buf[64]{};
        GetWindowTextW(edit_, buf, 64);
        double v = 0;
        std::wstring shown = finished_ ? number3(p_.inputValue) : (parseNumber(buf, v) ? number3(v) : L"—");
        line(fontBig_, shown + L" " + p_.inputUnit, 38, fg);
    } else {
        line(fontBig_, money(p_.amount), 38, fg);
    }
    if (finished_) {
        line(fontVerdict_, status_, 30, fg);
    } else {
        if (p_.amountCash > 0) line(fontSmall_, L"наличными: " + money(p_.amountCash), 18, fg);
        line(fontText_, p_.text, 22, fg);
        int left = secondsUntil(p_.deadline);
        line(fontSmall_, left > 0 ? L"осталось " + std::to_wstring(left) + L" с" : L"время ожидания вышло", 18,
             RGB(0xf2, 0xc4, 0x6d));
    }
    if (!finished_ && !status_.empty()) {
        SelectObject(dc, fontUi_);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        RECT r{scale(12), scale(264), width - scale(12), scale(292)};
        DrawTextW(dc, status_.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}

LRESULT TerminalWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            paint(dc, rc.right, rc.bottom);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerClose) {
                KillTimer(hwnd, kTimerClose);
                active_ = false;
                ShowWindow(hwnd, SW_HIDE);
            } else {
                RECT r{0, 0, scale(380), scale(142)};
                InvalidateRect(hwnd, &r, TRUE);  // обратный отсчёт
            }
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == kIdEdit) {
                if (HIWORD(wp) == EN_CHANGE) {
                    RECT r{0, 0, scale(380), scale(142)};
                    InvalidateRect(hwnd, &r, TRUE);  // вес на табло
                } else if (HIWORD(wp) == EN_SETFOCUS) {
                    PostMessageW(edit_, EM_SETSEL, 0, -1);
                }
                return 0;
            }
            if (id >= kIdActionBase && id < kIdActionBase + static_cast<int>(p_.actions.size()) && active_ && !finished_) {
                const PromptOption& a = p_.actions[static_cast<size_t>(id - kIdActionBase)];
                PromptReply reply;
                reply.action = a.id;
                int sel = static_cast<int>(SendMessageW(combo_, CB_GETCURSEL, 0, 0));
                if (a.id == "card" && sel >= 0 && sel < static_cast<int>(p_.cards.size())) reply.card = p_.cards[sel].id;
                if (p_.hasInput && a.id != "cancel") {
                    if (!readValue(reply.value)) {
                        status_ = p_.inputLabel + L": число от " + number3(p_.inputMin) + L" до " + number3(p_.inputMax) +
                                  L" " + p_.inputUnit;
                        InvalidateRect(hwnd, nullptr, TRUE);
                        SetFocus(edit_);
                        return 0;
                    }
                    reply.hasValue = true;
                }
                for (HWND b : buttons_) EnableWindow(b, FALSE);  // одно нажатие — один ответ
                status_ = L"Отправлено: " + a.title + L"…";
                InvalidateRect(hwnd, nullptr, TRUE);
                if (onAnswer_) onAnswer_(p_, reply);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_CLOSE:  // закрыть — не отменить: операция ждёт, окно вернёт меню трея
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool TerminalWindow::renderToBmp(const PromptEvent& p, const std::wstring& path) {
    show(p);
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd_, nullptr, -10000, -10000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    bool ok = PrintWindow(hwnd_, mem, PW_RENDERFULLCONTENT) != FALSE;
    SelectObject(mem, old);
    if (ok) {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
        fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w * h * 4);
        bi.bmiHeader.biHeight = h;  // в файле — снизу вверх
        FILE* f = _wfopen(path.c_str(), L"wb");
        ok = f != nullptr;
        if (f) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
            const auto* px = static_cast<const unsigned char*>(bits);
            for (int y = h - 1; y >= 0; --y) fwrite(px + static_cast<size_t>(y) * w * 4, 4, static_cast<size_t>(w), f);
            fclose(f);
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace tray
