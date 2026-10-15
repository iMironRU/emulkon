#include <cstring>

#include "ui.h"

namespace tray {

HICON makeIcon(bool connected, int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;  // сверху вниз
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(dc, color);

    // Фон — прозрачный; рисуем скруглённый квадрат и букву, затем выставляем альфу.
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    COLORREF bg = connected ? RGB(0xC2, 0x41, 0x0C) : RGB(0x8A, 0x8F, 0x94);
    HBRUSH brush = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, bg);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
    int r = size / 4;
    RoundRect(dc, 0, 0, size, size, r, r);
    HFONT font = CreateFontW(-MulDiv(size, 80, 100), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc{0, 0, size, size};
    DrawTextW(dc, L"Э", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    // GDI не пишет альфу: всё, что нарисовано (ненулевой цвет), делаем непрозрачным.
    auto* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; ++i) {
        if (px[i] & 0x00FFFFFF) px[i] |= 0xFF000000;
    }

    SelectObject(dc, old);
    DeleteObject(font);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace tray
