#include <windowsx.h>

#include <algorithm>
#include <cstdio>

#include "ui.h"

namespace tray {

namespace {

const wchar_t* kPopupClass = L"EmulkonReceiptPopup";
const UINT_PTR kHideTimer = 1;

const COLORREF kAccent = RGB(0xC2, 0x41, 0x0C);
const COLORREF kPaper = RGB(0xFF, 0xFD, 0xF6);
const COLORREF kInk = RGB(0x2B, 0x2B, 0x2B);
const COLORREF kBorder = RGB(0xD9, 0xD4, 0xC7);

LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ReceiptPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

int headerHeight(int dpi) { return MulDiv(46, dpi, 96); }
int padding(int dpi) { return MulDiv(12, dpi, 96); }

}  // namespace

bool ReceiptPopup::create(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = popupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    HDC screen = GetDC(nullptr);
    dpi_ = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    mono_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    title_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    small_ = CreateFontW(-MulDiv(8, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPopupClass, L"Эмулкон: чек",
                            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

void ReceiptPopup::layout(HDC dc) {
    HGDIOBJ old = SelectObject(dc, mono_);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"0000000000", 10, &sz);
    SelectObject(dc, old);
    charWidth_ = std::max(1, static_cast<int>(sz.cx / 10));
    lineHeight_ = tm.tmHeight + tm.tmExternalLeading + scale(1);

    size_t maxChars = 32;
    for (const auto& l : doc_.lines) maxChars = std::max(maxChars, widen(l).size());
    width_ = static_cast<int>(maxChars) * charWidth_ + 2 * padding(dpi_);
    width_ = std::max(width_, scale(300));
    contentHeight_ = static_cast<int>(doc_.lines.size()) * lineHeight_ + 2 * padding(dpi_);
    if (!doc_.qrRows.empty()) contentHeight_ += qrSide();  // QR-код чека под текстом

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int maxHeight = (work.bottom - work.top) * 85 / 100;
    height_ = std::min(headerHeight(dpi_) + contentHeight_, maxHeight);
}

void ReceiptPopup::show(const PrintedDoc& doc, int seconds) {
    doc_ = doc;
    seconds_ = seconds > 0 ? seconds : 10;
    scroll_ = 0;
    HDC dc = GetDC(hwnd_);
    layout(dc);
    ReleaseDC(hwnd_, dc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int margin = scale(12);
    SetWindowPos(hwnd_, HWND_TOPMOST, work.right - width_ - margin, work.bottom - height_ - margin, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    KillTimer(hwnd_, kHideTimer);
    if (!hover_) SetTimer(hwnd_, kHideTimer, static_cast<UINT>(seconds_) * 1000, nullptr);
}

void ReceiptPopup::hide() {
    KillTimer(hwnd_, kHideTimer);
    ShowWindow(hwnd_, SW_HIDE);
}

void ReceiptPopup::paint(HDC dc, int width, int height) {
    RECT all{0, 0, width, height};
    HBRUSH paper = CreateSolidBrush(kPaper);
    FillRect(dc, &all, paper);
    DeleteObject(paper);

    // Заголовок: вид документа, ФД, устройство, время; справа — «✕».
    int hh = headerHeight(dpi_);
    RECT head{0, 0, width, hh};
    HBRUSH accent = CreateSolidBrush(kAccent);
    FillRect(dc, &head, accent);
    DeleteObject(accent);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, title_);
    std::wstring title = kindTitle(doc_.kind);
    if (doc_.number) title += L" · ФД " + std::to_wstring(doc_.number);
    if (doc_.electronic) title += L" · только электронно";
    if (doc_.printError) title += L" · сбой печати";
    // Две строки: вид документа и ФД; ниже мелко — устройство, рабочее место, время.
    RECT tr{padding(dpi_), scale(5), width - scale(34), scale(25)};
    DrawTextW(dc, title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, small_);
    SetTextColor(dc, RGB(0xFF, 0xE4, 0xD1));
    std::wstring sub = widen(doc_.deviceId);
    if (!doc_.workstation.empty()) sub += L" · " + widen(doc_.workstation);
    sub += L" · " + shortTime(doc_.time);
    RECT sr{padding(dpi_), scale(25), width - scale(34), hh - scale(4)};
    DrawTextW(dc, sub.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, title_);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT xr{width - scale(32), 0, width, scale(30)};
    DrawTextW(dc, L"✕", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Лента чека.
    SelectObject(dc, mono_);
    SetTextColor(dc, kInk);
    HRGN clip = CreateRectRgn(0, hh, width, height);
    SelectClipRgn(dc, clip);
    int y = hh + padding(dpi_) - scroll_;
    for (const auto& line : doc_.lines) {
        if (y + lineHeight_ >= hh && y <= height) {
            std::wstring w = widen(line);
            TextOutW(dc, padding(dpi_), y, w.c_str(), static_cast<int>(w.size()));
        }
        y += lineHeight_;
    }
    if (!doc_.qrRows.empty()) {
        int module = qrSide() / (static_cast<int>(doc_.qrRows.size()) + 2 * qrQuiet);
        drawQr(dc, (width - qrSide()) / 2 + qrQuiet * module, y + qrQuiet * module, module, doc_.qrRows);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // Полоса прокрутки, если чек не помещается.
    int view = height - hh;
    if (contentHeight_ > view) {
        int barH = std::max(scale(24), view * view / contentHeight_);
        int barY = hh + (view - barH) * scroll_ / std::max(1, contentHeight_ - view);
        RECT bar{width - scale(5), barY, width - scale(2), barY + barH};
        HBRUSH b = CreateSolidBrush(kBorder);
        FillRect(dc, &bar, b);
        DeleteObject(b);
    }
    // Рамка.
    HBRUSH border = CreateSolidBrush(kBorder);
    FrameRect(dc, &all, border);
    DeleteObject(border);
    SelectObject(dc, old);
}

LRESULT ReceiptPopup::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Двойная буферизация — без мерцания при прокрутке.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            paint(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kHideTimer) hide();
            return 0;
        case WM_MOUSEMOVE:
            if (!hover_) {
                hover_ = true;
                KillTimer(hwnd, kHideTimer);
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            hover_ = false;
            SetTimer(hwnd, kHideTimer, 4000, nullptr);
            return 0;
        case WM_MOUSEWHEEL: {
            int view = height_ - headerHeight(dpi_);
            int maxScroll = std::max(0, contentHeight_ - view);
            scroll_ -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * lineHeight_ * 3;
            scroll_ = std::max(0, std::min(scroll_, maxScroll));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
            hide();  // щелчок — закрыть
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // не забирать фокус у кассы
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ReceiptPopup::renderToBmp(const PrintedDoc& doc, const std::wstring& path) {
    doc_ = doc;
    scroll_ = 0;
    HDC screen = GetDC(nullptr);
    layout(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = height_;  // снизу вверх, как в файле BMP
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(mem, width_, height_);
    GdiFlush();

    BITMAPFILEHEADER fh{};
    DWORD imageSize = static_cast<DWORD>(width_) * height_ * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + imageSize;
    bool ok = false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        ok = std::fwrite(&fh, sizeof fh, 1, f) == 1 && std::fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f) == 1 &&
             std::fwrite(bits, imageSize, 1, f) == 1;
        std::fclose(f);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return ok;
}

}  // namespace tray
