// Видимая часть трея: иконка, всплывающее окно с чеком, печать, окно настроек.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "doc.h"

namespace tray {

// drawQr — QR-код по матрице сервера: тёмные модули чёрным, (x, y) — левый верхний модуль без поля.
void drawQr(HDC dc, int x, int y, int module, const std::vector<std::string>& rows);
// qrQuiet — поле вокруг QR-кода, модулей (стандарт — 4).
constexpr int qrQuiet = 4;

// keyboardInputs — нажатия для текста и суффикса (enter, tab, none): CR и LF — Enter, TAB — Tab,
// остальное — символами Unicode. typeText вводит их в активное окно (SendInput).
std::vector<INPUT> keyboardInputs(const std::wstring& text, const std::string& suffix);
bool typeText(const std::wstring& text, const std::string& suffix, std::wstring& error);

// makeIcon — значок «Э» в цветах веб-интерфейса; без связи — серый.
HICON makeIcon(bool connected, int size);

// Всплывающее окно чека: поверх всех окон, не забирает фокус у 1С, само
// скрывается через заданное время (пока мышь над окном — не скрывается).
class ReceiptPopup {
public:
    bool create(HINSTANCE inst);
    void show(const PrintedDoc& doc, int seconds);
    void hide();
    // qrSide — сторона QR-кода чека с полем, пикселей (модуль 3 px при 96 dpi).
    int qrSide() const { return MulDiv(3, dpi_, 96) * (static_cast<int>(doc_.qrRows.size()) + 2 * qrQuiet); }
    // renderToBmp — нарисовать окно для документа в BMP (самопроверка без экрана).
    bool renderToBmp(const PrintedDoc& doc, const std::wstring& path);

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void layout(HDC dc);
    void paint(HDC dc, int width, int height);
    int scale(int px) const { return MulDiv(px, dpi_, 96); }

    HWND hwnd_ = nullptr;
    PrintedDoc doc_;
    int dpi_ = 96;
    int seconds_ = 10;
    int scroll_ = 0;
    int contentHeight_ = 0;
    int width_ = 0, height_ = 0;
    int lineHeight_ = 0, charWidth_ = 0;
    bool hover_ = false;
    HFONT mono_ = nullptr, title_ = nullptr, small_ = nullptr;
};

// PromptReply — ответ с экрана устройства: кнопка, карта (терминал), значение поля (весы).
struct PromptReply {
    std::string action, card;
    bool hasValue = false;
    double value = 0;
};

// Окно устройства, которое ждёт человека (device.prompt): эквайринговый терминал
// «приложите карту», весы «положите товар» с полем веса. Поверх всех окон, без захвата
// клавиатуры; ответ отдаёт onAnswer (POST на сервер), итог (device.prompt_done)
// показывает и закрывается само. Закрытие не отменяет операцию.
class TerminalWindow {
public:
    using OnAnswer = std::function<void(const PromptEvent& p, const PromptReply& reply)>;
    bool create(HINSTANCE inst, OnAnswer onAnswer);
    void show(const PromptEvent& p);
    void finish(const PromptEvent& done);
    void answerSent(bool ok, const std::wstring& message);
    void reopen();
    bool active() const { return active_ && !finished_; }
    // renderToBmp — показать запрос вне экрана и снять окно в BMP (самопроверка).
    bool renderToBmp(const PromptEvent& p, const std::wstring& path);

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void paint(HDC dc, int width, int height);
    void clearButtons();
    bool readValue(double& v) const;
    int scale(int px) const { return MulDiv(px, dpi_, 96); }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, combo_ = nullptr, edit_ = nullptr;
    std::vector<HWND> buttons_;
    OnAnswer onAnswer_;
    PromptEvent p_;
    bool active_ = false, finished_ = false, approved_ = false;
    std::wstring status_;
    int dpi_ = 96;
    HFONT fontUi_ = nullptr, fontSmall_ = nullptr, fontText_ = nullptr, fontBig_ = nullptr, fontVerdict_ = nullptr;
};

// Печать документа на принтер Windows: моноширинный шрифт, подогнанный под ширину
// печатной области (узкий чековый принтер и A4 — одинаково). outputFile — для
// PDF/XPS-принтеров: сохранить без диалога.
bool printDoc(const PrintedDoc& doc, const std::wstring& printer, const std::wstring& outputFile, std::wstring& error);
std::vector<std::wstring> listPrinters();
std::wstring defaultPrinter();

// showSettings — модальное окно настроек. true — нажато «Сохранить».
bool showSettings(HINSTANCE inst, HWND owner, Config& cfg);

}  // namespace tray
