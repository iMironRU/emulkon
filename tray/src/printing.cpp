#include <windows.h>
#include <winspool.h>

#include <algorithm>

#include "ui.h"

namespace tray {

std::vector<std::wstring> listPrinters() {
    std::vector<std::wstring> out;
    DWORD needed = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
    if (!needed) return out;
    std::vector<BYTE> buf(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), needed, &needed, &count))
        return out;
    auto* info = reinterpret_cast<PRINTER_INFO_4W*>(buf.data());
    for (DWORD i = 0; i < count; ++i) out.emplace_back(info[i].pPrinterName);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring defaultPrinter() {
    DWORD n = 0;
    GetDefaultPrinterW(nullptr, &n);
    if (!n) return {};
    std::wstring s(n, L'\0');
    if (!GetDefaultPrinterW(&s[0], &n)) return {};
    s.resize(n ? n - 1 : 0);
    return s;
}

void drawQr(HDC dc, int x, int y, int module, const std::vector<std::string>& rows) {
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size(); ++c) {
            if (rows[r][c] != '1') continue;
            RECT m{x + static_cast<int>(c) * module, y + static_cast<int>(r) * module, x + static_cast<int>(c + 1) * module,
                   y + static_cast<int>(r + 1) * module};
            FillRect(dc, &m, black);
        }
    }
    DeleteObject(black);
}

bool printDoc(const PrintedDoc& doc, const std::wstring& printerName, const std::wstring& outputFile, std::wstring& error) {
    std::wstring printer = printerName.empty() ? defaultPrinter() : printerName;
    if (printer.empty()) {
        error = L"в системе нет принтера по умолчанию";
        return false;
    }
    HDC dc = CreateDCW(L"WINSPOOL", printer.c_str(), nullptr, nullptr);
    if (!dc) {
        error = L"принтер «" + printer + L"» недоступен";
        return false;
    }

    // Печатная область и поля 4 мм.
    int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    int pageW = GetDeviceCaps(dc, HORZRES), pageH = GetDeviceCaps(dc, VERTRES);
    int marginX = MulDiv(4, dpiX * 10, 254), marginY = MulDiv(4, dpiY * 10, 254);
    int usableW = std::max(1, pageW - 2 * marginX);

    // Шрифт 10 pt, но не шире печатной области: узкая лента 58/80 мм уменьшит его.
    size_t maxChars = 32;
    for (const auto& l : doc.lines) maxChars = std::max(maxChars, widen(l).size());
    int height = -MulDiv(10, dpiY, 72);
    HFONT font = nullptr;
    int lineH = 0;
    int textW = usableW;  // ширина текста чека — QR центруется под ним
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (font) DeleteObject(font);
        font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ prev = SelectObject(dc, font);
        std::wstring probe(maxChars, L'0');
        SIZE sz{};
        GetTextExtentPoint32W(dc, probe.c_str(), static_cast<int>(probe.size()), &sz);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        lineH = tm.tmHeight + tm.tmExternalLeading;
        SelectObject(dc, prev);
        textW = std::min(usableW, static_cast<int>(sz.cx));
        if (sz.cx <= usableW) break;
        height = MulDiv(height, usableW, sz.cx);  // уменьшаем пропорционально
    }

    std::wstring title = L"Эмулкон: " + kindTitle(doc.kind) + (doc.number ? L" ФД " + std::to_wstring(doc.number) : L"");
    DOCINFOW di{};
    di.cbSize = sizeof di;
    di.lpszDocName = title.c_str();
    di.lpszOutput = outputFile.empty() ? nullptr : outputFile.c_str();
    bool ok = StartDocW(dc, &di) > 0;
    if (!ok) {
        error = L"принтер «" + printer + L"» отказал в печати (StartDoc)";
    } else {
        int perPage = std::max(1, (pageH - 2 * marginY) / std::max(1, lineH));
        // QR-код чека — под текстом, около 30 мм; не помещается на странице — на следующей.
        int qrModule = 0, qrSide = 0;
        bool qrPending = !doc.qrRows.empty();
        if (qrPending) {
            int n = static_cast<int>(doc.qrRows.size()) + 2 * qrQuiet;
            qrModule = std::max(1, std::min(usableW, MulDiv(30, dpiX * 10, 254)) / n);
            qrSide = qrModule * n;
        }
        size_t i = 0;
        while (ok && (i < doc.lines.size() || qrPending)) {
            ok = StartPage(dc) > 0;
            HGDIOBJ prev = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            int y = marginY;
            bool emptyPage = true;
            for (int k = 0; k < perPage && i < doc.lines.size(); ++k, ++i) {
                std::wstring w = widen(doc.lines[i]);
                TextOutW(dc, marginX, y, w.c_str(), static_cast<int>(w.size()));
                y += lineH;
                emptyPage = false;
            }
            if (qrPending && i == doc.lines.size() && (emptyPage || y + qrSide <= pageH - marginY)) {
                int x = marginX + std::max(0, (textW - qrSide) / 2) + qrQuiet * qrModule;
                drawQr(dc, x, y + qrQuiet * qrModule, qrModule, doc.qrRows);
                qrPending = false;
            }
            SelectObject(dc, prev);
            ok = EndPage(dc) > 0 && ok;
        }
        if (ok) {
            ok = EndDoc(dc) > 0;
        } else {
            AbortDoc(dc);
        }
        if (!ok) error = L"ошибка печати на «" + printer + L"»";
    }
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

}  // namespace tray
