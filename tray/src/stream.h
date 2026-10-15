// Живой поток с сервера: SSE /control/v1/journal/stream с фильтром по рабочему месту и
// method=device.print,device.reprint,device.keyboard,device.prompt,device.prompt_done — «напечатанные» документы и экран
// терминала. Работает в своём потоке, переподключается сам; после обрыва дочитывает
// пропущенное по курсору after_seq.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "doc.h"

namespace tray {

struct HttpResult {
    int status = 0;
    std::string body;
    std::string error;
};

// httpGet — простой GET (проверка связи, последний документ).
HttpResult httpGet(const std::wstring& url, const std::wstring& token, int timeoutMs);
// httpPost — POST JSON (ответ с экрана терминала).
HttpResult httpPost(const std::wstring& url, const std::wstring& token, const std::string& json, int timeoutMs);
// httpPut — PUT JSON (принтеры рабочего места).
HttpResult httpPut(const std::wstring& url, const std::wstring& token, const std::string& json, int timeoutMs);

// urlEncode — для значений query string (рабочее место может быть кириллицей).
std::wstring urlEncode(const std::wstring& s);

class JournalStream {
public:
    // live=false — документ из дочитки (до события ready): показывать не нужно,
    // только запомнить как последний.
    using OnDoc = std::function<void(const PrintedDoc& doc, bool live)>;
    using OnStatus = std::function<void(bool connected, const std::wstring& message)>;
    // OnPrompt — экран устройства ждёт человека или дождался. Только живые события:
    // открытый запрос из дочитки отдаётся, если его срок ещё не вышел.
    using OnPrompt = std::function<void(const PromptEvent& ev)>;
    // OnKeyboard — ввести текст с клавиатуры (сканер в режиме клавиатуры). Только живые записи.
    using OnKeyboard = std::function<void(const KeyboardEvent& ev)>;

    ~JournalStream() { stop(); }

    void start(const std::wstring& serverUrl, const std::wstring& token, const std::wstring& workstation,
               OnDoc onDoc, OnStatus onStatus, OnPrompt onPrompt = nullptr, OnKeyboard onKeyboard = nullptr);
    void stop();

private:
    void run();
    bool session(std::wstring& error);  // одно подключение; false — ошибка

    std::wstring server_, token_, workstation_;
    OnDoc onDoc_;
    OnStatus onStatus_;
    OnPrompt onPrompt_;
    OnKeyboard onKeyboard_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    HINTERNET request_ = nullptr;  // закрытие прерывает блокирующее чтение
    HANDLE wake_ = nullptr;        // прерывает паузу между попытками
    int64_t lastSeq_ = 0;
};

}  // namespace tray
