#include "dialogs.h"

#include <commctrl.h>
#include <commdlg.h>
#include <mmc.h>

#include <map>
#include <mutex>
#include <vector>

#include "inventory.h"
#include "maintenance.h"
#include "records.h"
#include "summary.h"

namespace console {

HINSTANCE g_instance = nullptr;

namespace {

const wchar_t* const kTitle = L"Эмулкон";

// --- шаблон окна в памяти: заголовок и шрифт, элементы создаются в WM_INITDIALOG ---

std::vector<WORD> dialogTemplate(const wchar_t* title, short cx, short cy, DWORD style) {
    std::vector<WORD> t;
    auto dword = [&](DWORD v) {
        t.push_back(LOWORD(v));
        t.push_back(HIWORD(v));
    };
    auto text = [&](const wchar_t* s) {
        for (; *s; ++s) t.push_back(*s);
        t.push_back(0);
    };
    dword(style | DS_SETFONT);
    dword(0);         // exStyle
    t.push_back(0);   // элементов нет
    t.push_back(0);   // x
    t.push_back(0);   // y
    t.push_back(cx);
    t.push_back(cy);
    t.push_back(0);  // меню
    t.push_back(0);  // класс
    text(title);
    t.push_back(8);  // кегль
    text(L"MS Shell Dlg 2");
    return t;
}

// control — элемент в единицах диалога (DLU).
HWND control(HWND dlg, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    RECT r{x, y, x + w, y + h};
    MapDialogRect(dlg, &r);
    DWORD ex = wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(c, WM_SETFONT, SendMessageW(dlg, WM_GETFONT, 0, 0), FALSE);
    return c;
}

HWND label(HWND dlg, const wchar_t* text, int x, int y, int w, int h, int id = -1) {
    return control(dlg, L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, h, id);
}

HWND button(HWND dlg, const wchar_t* text, int x, int y, int w, int id, DWORD style = BS_PUSHBUTTON) {
    return control(dlg, L"BUTTON", text, WS_TABSTOP | style, x, y, w, 14, id);
}

void setText(HWND dlg, int id, const std::wstring& s) { SetDlgItemTextW(dlg, id, s.c_str()); }

int ask(HWND parent, const std::wstring& text, UINT icon = MB_ICONWARNING) {
    return MessageBoxW(parent, text.c_str(), kTitle, MB_YESNO | MB_DEFBUTTON2 | icon);
}

void fail(HWND parent, const std::wstring& what, const std::wstring& error) {
    MessageBoxW(parent, (what + L"\n\n" + error).c_str(), kTitle, MB_OK | MB_ICONERROR);
}

struct WaitCursor {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ~WaitCursor() { SetCursor(old); }
};

bool copyToClipboard(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// --- новый токен: показывается один раз ---

struct ShowToken {
    std::wstring head, token, note;
};

enum { kIdTokenEdit = 200, kIdCopy = 201 };

INT_PTR CALLBACK showTokenProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* p = reinterpret_cast<ShowToken*>(lp);
            label(dlg, p->head.c_str(), 7, 7, 286, 18);
            control(dlg, L"EDIT", p->token.c_str(), ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 7, 28, 286, 14, kIdTokenEdit);
            label(dlg, p->note.c_str(), 7, 48, 286, 42);
            button(dlg, L"Копировать", 186, 96, 54, kIdCopy);
            button(dlg, L"Готово", 243, 96, 50, IDOK, BS_DEFPUSHBUTTON);
            SetFocus(GetDlgItem(dlg, kIdTokenEdit));
            SendDlgItemMessageW(dlg, kIdTokenEdit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kIdCopy) {
                wchar_t buf[128];
                GetDlgItemTextW(dlg, kIdTokenEdit, buf, 128);
                if (copyToClipboard(dlg, buf)) SetDlgItemTextW(dlg, kIdCopy, L"Скопировано");
                SecureZeroMemory(buf, sizeof buf);
                return TRUE;
            }
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

void showTokenDialog(HWND parent, const std::wstring& head, const std::string& token, const std::wstring& note) {
    static std::vector<WORD> tmpl =
        dialogTemplate(L"Эмулкон — новый токен", 300, 117, DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);
    ShowToken p{head, widen(token), note};
    DialogBoxIndirectParamW(g_instance, reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()), parent, showTokenProc,
                            reinterpret_cast<LPARAM>(&p));
    SecureZeroMemory(&p.token[0], p.token.size() * sizeof(wchar_t));
}

// --- ввод строки ---

struct AskText {
    std::wstring prompt, value;
    bool password = false;
};

INT_PTR CALLBACK askTextProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            auto* p = reinterpret_cast<AskText*>(lp);
            label(dlg, p->prompt.c_str(), 7, 7, 246, 26);
            control(dlg, L"EDIT", p->value.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | (p->password ? ES_PASSWORD : 0), 7, 36, 246, 14,
                    kIdTokenEdit);
            button(dlg, L"ОК", 149, 58, 50, IDOK, BS_DEFPUSHBUTTON);
            button(dlg, L"Отмена", 203, 58, 50, IDCANCEL);
            SetFocus(GetDlgItem(dlg, kIdTokenEdit));
            SendDlgItemMessageW(dlg, kIdTokenEdit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                auto* p = reinterpret_cast<AskText*>(GetWindowLongPtrW(dlg, DWLP_USER));
                wchar_t buf[512];
                GetDlgItemTextW(dlg, kIdTokenEdit, buf, 512);
                std::wstring w = buf;
                SecureZeroMemory(buf, sizeof buf);
                while (!w.empty() && iswspace(w.back())) w.pop_back();
                while (!w.empty() && iswspace(w.front())) w.erase(0, 1);
                p->value = w;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// --- страница свойств «Общие» ---

enum {
    kIdState = 100,
    kIdAddr,
    kIdCommand,
    kIdNetwork = 110,
    kIdControlState = 120,
    kIdDeviceState,
    kIdControlIssue = 130,
    kIdControlRemove,
    kIdDeviceIssue,
    kIdDeviceRemove,
    kIdStart = 140,
    kIdStop,
    kIdRestart,
};

struct PageCtx {
    LONG_PTR notify = 0;
    bool network = false;   // как сейчас в реестре
    bool readable = false;  // настройки службы видны: консоль от администратора
};

void refreshPage(HWND dlg, PageCtx* ctx) {
    ServiceInfo svc = queryServiceInfo();
    ServerSettings s = readServerSettings();
    const std::wstring listen = listenAddr(s);
    ctx->readable = s.readable;
    ctx->network = !isLocalOnly(listen);
    setText(dlg, kIdState, serviceText(svc.state));
    setText(dlg, kIdAddr, s.readable ? listen : listen + L" (настройки видны администратору)");
    setText(dlg, kIdCommand, svc.configKnown ? svc.command : L"видно администратору");
    CheckDlgButton(dlg, kIdNetwork, ctx->network ? BST_CHECKED : BST_UNCHECKED);
    setText(dlg, kIdControlState, !s.readable ? L"—" : s.controlToken ? L"задан" : L"не задан");
    setText(dlg, kIdDeviceState, !s.readable ? L"—" : s.deviceToken ? L"задан" : L"не задан");
    EnableWindow(GetDlgItem(dlg, kIdNetwork), s.readable);
    EnableWindow(GetDlgItem(dlg, kIdControlIssue), s.readable);
    EnableWindow(GetDlgItem(dlg, kIdDeviceIssue), s.readable);
    EnableWindow(GetDlgItem(dlg, kIdControlRemove), s.readable && s.controlToken);
    EnableWindow(GetDlgItem(dlg, kIdDeviceRemove), s.readable && s.deviceToken);
    const bool exists = svc.state != Service::Missing && svc.state != Service::Unknown;
    EnableWindow(GetDlgItem(dlg, kIdStart), exists && svc.state == Service::Stopped);
    EnableWindow(GetDlgItem(dlg, kIdStop), exists && svc.state == Service::Running);
    EnableWindow(GetDlgItem(dlg, kIdRestart), exists && svc.state == Service::Running);
}

void changed(PageCtx* ctx) {
    if (ctx->notify) MMCPropertyChangeNotify(ctx->notify, 0);  // MMC обновит списки в своём потоке
}

INT_PTR CALLBACK pageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<PageCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<PageCtx*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
            label(dlg, L"Служба Emulkon — сервер эмуляции оборудования для 1С:БПО.", 7, 7, 238, 9);
            label(dlg, L"Состояние:", 7, 21, 50, 9);
            label(dlg, L"", 60, 21, 185, 9, kIdState);
            label(dlg, L"Адрес:", 7, 32, 50, 9);
            label(dlg, L"", 60, 32, 185, 9, kIdAddr);
            label(dlg, L"Команда:", 7, 43, 50, 9);
            control(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 60, 43, 185, 9, kIdCommand);
            button(dlg, L"Запустить", 60, 56, 58, kIdStart);
            button(dlg, L"Остановить", 122, 56, 58, kIdStop);
            button(dlg, L"Перезапустить", 184, 56, 61, kIdRestart);

            control(dlg, L"BUTTON", L"Сервер для касс в сети: подключения с других компьютеров (0.0.0.0:8737) и правило "
                                    L"брандмауэра для TCP 8737",
                    WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP, 7, 78, 238, 20, kIdNetwork);
            label(dlg, L"Применяется кнопкой «Применить» или «ОК»: мастер «Изменить» перенастроит брандмауэр и "
                       L"перезапустит службу.",
                  19, 99, 226, 18);

            control(dlg, L"BUTTON", L"Токены", BS_GROUPBOX, 7, 120, 238, 92, -1);
            label(dlg, L"Управление:", 14, 135, 50, 9);
            label(dlg, L"", 66, 135, 64, 9, kIdControlState);
            button(dlg, L"Выдать…", 134, 132, 52, kIdControlIssue);
            button(dlg, L"Снять", 189, 132, 50, kIdControlRemove);
            label(dlg, L"Устройства:", 14, 153, 50, 9);
            label(dlg, L"", 66, 153, 64, 9, kIdDeviceState);
            button(dlg, L"Выдать…", 134, 150, 52, kIdDeviceIssue);
            button(dlg, L"Снять", 189, 150, 50, kIdDeviceRemove);
            label(dlg, L"Токен показывается один раз; хранится только его хеш SHA-256. После выдачи или снятия служба "
                       L"перезапускается. Токены рабочих мест — в разделе «Рабочие места».",
                  14, 170, 226, 36);
            refreshPage(dlg, ctx);
            return TRUE;
        }
        case WM_COMMAND: {
            if (!ctx || HIWORD(wp) != BN_CLICKED) break;
            HWND sheet = GetParent(dlg);
            bool done = false;
            switch (LOWORD(wp)) {
                case kIdNetwork:
                    PropSheet_Changed(sheet, dlg);
                    return TRUE;
                case kIdControlIssue: done = issueToken(dlg, true); break;
                case kIdControlRemove: done = removeToken(dlg, true); break;
                case kIdDeviceIssue: done = issueToken(dlg, false); break;
                case kIdDeviceRemove: done = removeToken(dlg, false); break;
                case kIdStart: done = serviceAction(dlg, ServiceAction::Start); break;
                case kIdStop: done = serviceAction(dlg, ServiceAction::Stop); break;
                case kIdRestart: done = serviceAction(dlg, ServiceAction::Restart); break;
                default: return FALSE;
            }
            // Флажок, ещё не применённый, не сбрасываем.
            const bool pending = (IsDlgButtonChecked(dlg, kIdNetwork) == BST_CHECKED) != ctx->network;
            refreshPage(dlg, ctx);
            if (pending) CheckDlgButton(dlg, kIdNetwork, ctx->network ? BST_UNCHECKED : BST_CHECKED);
            if (done) changed(ctx);
            return TRUE;
        }
        case WM_NOTIFY: {
            if (!ctx || reinterpret_cast<NMHDR*>(lp)->code != PSN_APPLY) break;
            const bool want = IsDlgButtonChecked(dlg, kIdNetwork) == BST_CHECKED;
            if (ctx->readable && want != ctx->network) {
                if (want && ask(dlg, L"Открыть сервер для касс в сети?\n\nСлужба будет слушать 0.0.0.0:8737, брандмауэр "
                                     L"пропустит TCP 8737. Без токенов подключиться и управлять сможет любой в сети — "
                                     L"выдайте токены управления и устройств.") != IDYES) {
                    SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                    return TRUE;
                }
                std::wstring error;
                bool ok;
                {
                    WaitCursor wait;
                    ok = runModify(want, error);
                }
                if (!ok) {
                    fail(dlg, L"Не удалось изменить «Кассы в сети».", error);
                    SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                    return TRUE;
                }
                refreshPage(dlg, ctx);
                changed(ctx);
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
    }
    return FALSE;
}

UINT CALLBACK pageCallback(HWND, UINT msg, PROPSHEETPAGEW* page) {
    if (msg == PSPCB_RELEASE && page) {
        auto* ctx = reinterpret_cast<PageCtx*>(page->lParam);
        if (ctx) {
            if (ctx->notify) MMCFreeNotifyHandle(ctx->notify);
            delete ctx;
        }
    }
    return 1;
}

const wchar_t* tokenValue(bool control) { return control ? L"ControlTokenHash" : L"DeviceTokenHash"; }

// restartIfRunning — служба читает токены при запуске.
bool restartIfRunning(std::wstring& error) {
    if (queryService() != Service::Running) return true;
    return controlService(true, true, error);
}

}  // namespace

HPROPSHEETPAGE createServicePage(LONG_PTR notify) {
    static std::vector<WORD> tmpl = dialogTemplate(L"Общие", 252, 218, WS_CHILD | WS_DISABLED | WS_CAPTION);
    auto* ctx = new PageCtx{notify};
    PROPSHEETPAGEW psp{sizeof(psp)};
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USECALLBACK;
    psp.hInstance = g_instance;
    psp.pResource = reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data());
    psp.pfnDlgProc = pageProc;
    psp.pfnCallback = pageCallback;
    psp.lParam = reinterpret_cast<LPARAM>(ctx);
    HPROPSHEETPAGE page = CreatePropertySheetPageW(&psp);
    if (!page) delete ctx;  // дескриптор уведомлений освободит вызывающий
    return page;
}

bool askText(HWND parent, const std::wstring& title, const std::wstring& prompt, std::wstring& value, bool password) {
    // Заголовок — в шаблоне: шаблон на каждый вызов.
    std::vector<WORD> tmpl = dialogTemplate(title.c_str(), 260, 79, DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);
    AskText p{prompt, value, password};
    INT_PTR rc = DialogBoxIndirectParamW(g_instance, reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()), parent, askTextProc,
                                         reinterpret_cast<LPARAM>(&p));
    if (rc != IDOK) return false;
    value = p.value;
    if (password) SecureZeroMemory(&p.value[0], p.value.size() * sizeof(wchar_t));
    return true;
}

bool askToken(HWND parent, std::string& token) {
    std::wstring w;
    if (!askText(parent, L"Эмулкон — токен управления для консоли",
                 L"Токен Control API: выданный в консоли или EMULKON_CONTROL_TOKEN сервера. Консоль держит его в памяти "
                 L"до закрытия и никуда не записывает.",
                 w, true))
        return false;
    token = narrow(w);
    if (!w.empty()) SecureZeroMemory(&w[0], w.size() * sizeof(wchar_t));
    return true;
}

void showToken(HWND parent, const std::wstring& head, const std::string& token, const std::wstring& note) {
    showTokenDialog(parent, head, token, note);
}

bool issueToken(HWND parent, bool control) {
    const std::wstring question =
        control ? L"Выдать новый токен управления?\n\nВеб-интерфейс, трей и консоль будут спрашивать его; прежний "
                  L"токен перестанет действовать. Служба будет перезапущена."
                : L"Выдать новый токен устройств?\n\nДрайверу 1С без токена рабочего места понадобится этот токен "
                  L"(параметр «Токен устройства»); прежний перестанет действовать. Служба будет перезапущена.";
    if (ask(parent, question, MB_ICONQUESTION) != IDYES) return false;
    std::string token = newToken();
    std::wstring error;
    if (token.empty()) {
        fail(parent, L"Токен не выдан.", L"нет случайных чисел (BCryptGenRandom)");
        return false;
    }
    if (!writeTokenHash(tokenValue(control), sha256Hex(token), error)) {
        fail(parent, L"Токен не выдан.", error);
        return false;
    }
    if (control) setConsoleToken(token);
    bool restarted;
    {
        WaitCursor wait;
        restarted = restartIfRunning(error);
    }
    const std::wstring note =
        std::wstring(control ? L"Сохраните его: он показывается один раз. Задайте его в трее («Настройки» → токен "
                               L"Control API) и в веб-интерфейсе. Эта консоль уже знает его до закрытия."
                             : L"Сохраните его: он показывается один раз. Задайте его в параметре «Токен устройства» "
                               L"драйвера на кассах без токена рабочего места.") +
        (restarted ? L"" : L"\n\nСлужба не перезапустилась: " + error);
    showTokenDialog(parent, control ? L"Новый токен управления (Control API):" : L"Новый токен устройств (Device API):", token, note);
    SecureZeroMemory(&token[0], token.size());
    return true;
}

bool removeToken(HWND parent, bool control) {
    const std::wstring question =
        control ? L"Снять токен управления?\n\nУправлять сервером сможет любой, кто до него достучится. Служба будет "
                  L"перезапущена."
                : L"Снять токен устройств?\n\nДрайвер без токена рабочего места подключится без токена. Служба будет "
                  L"перезапущена.";
    if (ask(parent, question) != IDYES) return false;
    std::wstring error;
    if (!writeTokenHash(tokenValue(control), "", error)) {
        fail(parent, L"Токен не снят.", error);
        return false;
    }
    if (control) setConsoleToken("");
    WaitCursor wait;
    if (!restartIfRunning(error)) fail(parent, L"Токен снят, но служба не перезапустилась.", error);
    return true;
}

bool serviceAction(HWND parent, ServiceAction action) {
    std::wstring error;
    bool ok;
    {
        WaitCursor wait;
        ok = controlService(action != ServiceAction::Start, action != ServiceAction::Stop, error);
    }
    if (!ok) {
        const wchar_t* what = action == ServiceAction::Start  ? L"Служба Emulkon не запустилась."
                              : action == ServiceAction::Stop ? L"Служба Emulkon не остановилась."
                                                              : L"Служба Emulkon не перезапустилась.";
        fail(parent, what, error);
    }
    return true;  // состояние могло измениться и при ошибке
}
// --- рабочие места и устройства (этап 3) ---

namespace {

// done — ответ сервера: успех или окно с причиной.
bool done(HWND parent, const Reply& r, const std::wstring& what) {
    if (r.status >= 200 && r.status < 300) return true;
    fail(parent, what, replyError(r));
    return false;
}

// findWorkstation — свежие данные рабочего места с сервера.
bool findWorkstation(const std::string& id, Workstation& out) {
    Reply r = apiRequest("GET", "/control/v1/workstations");
    std::vector<Workstation> list;
    std::wstring err;
    if (r.status != 200 || !parseWorkstations(r.body, list, err)) return false;
    for (auto& w : list)
        if (w.id == id) {
            out = w;
            return true;
        }
    return false;
}

std::wstring lines(const std::vector<std::wstring>& v, const std::wstring& empty) {
    std::wstring s;
    for (const auto& x : v) s += (s.empty() ? L"" : L"\r\n") + x;
    return s.empty() ? empty : s;
}

std::wstring itemText(HWND dlg, int id) {
    int n = GetWindowTextLengthW(GetDlgItem(dlg, id));
    std::wstring s(n + 1, L'\0');
    GetDlgItemTextW(dlg, id, &s[0], n + 1);
    s.resize(n);
    return s;
}

// limitMeta — подпись и заметка: одна строка, сервер принимает до 512 байт UTF-8 (кириллица — 2 байта).
void limitMeta(HWND dlg);

HWND readOnlyText(HWND dlg, const std::wstring& text, int x, int y, int w, int h, int id) {
    return control(dlg, L"EDIT", text.c_str(), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, x, y, w, h, id);
}

enum {
    kIdAlias = 300,
    kIdNote,
    kIdTokenText,
    kIdTokenIssue,
    kIdTokenRevoke,
    kIdBases,
    kIdPrinters,
    kIdDevices,
    kIdStateText,
};

void limitMeta(HWND dlg) {
    SendDlgItemMessageW(dlg, kIdAlias, EM_LIMITTEXT, 250, 0);
    SendDlgItemMessageW(dlg, kIdNote, EM_LIMITTEXT, 250, 0);
}

// --- рабочее место ---

struct WorkstationCtx {
    LONG_PTR notify = 0;
    Workstation w;
};

void fillWorkstationToken(HWND dlg, const Workstation& w) {
    setText(dlg, kIdTokenText, w.token ? L"выдан " + w.tokenIssued + L", оканчивается на …" + w.tokenHint : L"не выдан");
    EnableWindow(GetDlgItem(dlg, kIdTokenRevoke), w.token);
}

INT_PTR CALLBACK workstationProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<WorkstationCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<WorkstationCtx*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
            const Workstation& w = ctx->w;
            label(dlg, L"Код:", 7, 8, 56, 9);
            label(dlg, widen(w.id).c_str(), 66, 8, 179, 9);
            label(dlg, L"Подпись:", 7, 22, 56, 9);
            control(dlg, L"EDIT", w.alias.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 66, 20, 179, 12, kIdAlias);
            label(dlg, L"Заметка:", 7, 37, 56, 9);
            control(dlg, L"EDIT", w.note.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 66, 35, 179, 12, kIdNote);
            limitMeta(dlg);
            label(dlg, L"Подключения:", 7, 62, 56, 9);
            label(dlg, (L"первое " + w.firstSeen + L", последнее " + w.lastSeen + L"; сессий сейчас: " + std::to_wstring(w.sessions)).c_str(),
                  66, 62, 179, 18);
            label(dlg, L"Токен:", 7, 83, 56, 9);
            label(dlg, L"", 66, 83, 179, 9, kIdTokenText);
            button(dlg, L"Выдать новый…", 66, 94, 70, kIdTokenIssue);
            button(dlg, L"Отозвать", 140, 94, 50, kIdTokenRevoke);
            label(dlg, L"Базы 1С:", 7, 113, 56, 9);
            readOnlyText(dlg, lines(w.infobases, L"—"), 66, 112, 179, 30, kIdBases);
            std::vector<std::wstring> printers;
            for (const auto& p : w.printers) printers.push_back(p == w.defaultPrinter ? p + L" (по умолчанию)" : p);
            label(dlg, L"Принтеры:", 7, 147, 56, 9);
            readOnlyText(dlg, lines(printers, L"— (сообщает трей)"), 66, 146, 179, 26, kIdPrinters);
            label(dlg, L"Устройства:", 7, 177, 56, 9);
            readOnlyText(dlg, lines(w.devices, L"—"), 66, 176, 179, 34, kIdDevices);
            fillWorkstationToken(dlg, w);
            return TRUE;
        }
        case WM_COMMAND:
            if (!ctx) break;
            if ((LOWORD(wp) == kIdAlias || LOWORD(wp) == kIdNote) && HIWORD(wp) == EN_CHANGE) {
                PropSheet_Changed(GetParent(dlg), dlg);
                return TRUE;
            }
            if (HIWORD(wp) == BN_CLICKED && (LOWORD(wp) == kIdTokenIssue || LOWORD(wp) == kIdTokenRevoke)) {
                bool changed = LOWORD(wp) == kIdTokenIssue ? issueWorkstationToken(dlg, ctx->w.id) : revokeWorkstationToken(dlg, ctx->w.id);
                if (changed) {
                    Workstation fresh;
                    if (findWorkstation(ctx->w.id, fresh)) {
                        ctx->w.token = fresh.token;
                        ctx->w.tokenHint = fresh.tokenHint;
                        ctx->w.tokenIssued = fresh.tokenIssued;
                    }
                    fillWorkstationToken(dlg, ctx->w);
                    if (ctx->notify) MMCPropertyChangeNotify(ctx->notify, 0);
                }
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            if (!ctx || reinterpret_cast<NMHDR*>(lp)->code != PSN_APPLY) break;
            {
                const std::wstring alias = itemText(dlg, kIdAlias), note = itemText(dlg, kIdNote);
                const std::string body = metaPatch(ctx->w.alias, alias, ctx->w.note, note);
                if (!body.empty()) {
                    Reply r = apiSetWorkstationMeta(ctx->w.id, body);
                    if (!done(dlg, r, L"Подпись и заметка не сохранены.")) {
                        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                        return TRUE;
                    }
                    ctx->w.alias = cleanText(alias);
                    ctx->w.note = cleanText(note);
                    if (ctx->notify) MMCPropertyChangeNotify(ctx->notify, 0);
                }
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
    }
    return FALSE;
}

UINT CALLBACK workstationCallback(HWND, UINT msg, PROPSHEETPAGEW* page) {
    if (msg == PSPCB_RELEASE && page) {
        auto* ctx = reinterpret_cast<WorkstationCtx*>(page->lParam);
        if (ctx && ctx->notify) MMCFreeNotifyHandle(ctx->notify);
        delete ctx;
    }
    return 1;
}

// --- устройство ---

struct DeviceCtx {
    LONG_PTR notify = 0;
    Device d;
    std::wstring classTitle;
};

INT_PTR CALLBACK deviceProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<DeviceCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<DeviceCtx*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
            const Device& d = ctx->d;
            label(dlg, d.name.c_str(), 7, 7, 238, 9);
            label(dlg, L"Подпись:", 7, 22, 56, 9);
            control(dlg, L"EDIT", d.alias.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 66, 20, 179, 12, kIdAlias);
            label(dlg, L"Заметка:", 7, 37, 56, 9);
            control(dlg, L"EDIT", d.note.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 66, 35, 179, 12, kIdNote);
            limitMeta(dlg);
            const struct {
                const wchar_t* name;
                std::wstring value;
                int height;
            } rows[] = {
                {L"Код:", widen(d.id), 9},
                {L"Класс:", ctx->classTitle + L" (" + d.equipmentType + L")", 9},
                {L"Рабочее место:", d.workstation.empty() ? L"—" : d.workstation, 9},
                {L"База 1С:", d.infobase.empty() ? L"—" : d.infobase, 18},
                {L"Экземпляр:", d.instanceName.empty() ? L"—" : d.instanceName, 18},
                {L"Заведено:", d.created, 9},
                {L"Активность:", d.lastSeen + (d.online ? L" — на связи" : L""), 9},
                {L"Команд:", std::to_wstring(d.commands) + L"; сессий: " + std::to_wstring(d.sessions) + L"; слушают события: " +
                                 std::to_wstring(d.listeners), 9},
                {L"Ошибка:", d.lastError.empty() ? L"—" : d.lastError, 18},
            };
            int y = 64;
            for (const auto& r : rows) {
                label(dlg, r.name, 7, y, 56, 9);
                label(dlg, r.value.c_str(), 66, y, 179, r.height);
                y += r.height + 3;
            }
            label(dlg, L"Состояние модели — на вкладке «Состояние»; менять его, сценарии и действия — в веб-интерфейсе.",
                  7, 199, 238, 18);
            return TRUE;
        }
        case WM_COMMAND:
            if (ctx && (LOWORD(wp) == kIdAlias || LOWORD(wp) == kIdNote) && HIWORD(wp) == EN_CHANGE) {
                PropSheet_Changed(GetParent(dlg), dlg);
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            if (!ctx || reinterpret_cast<NMHDR*>(lp)->code != PSN_APPLY) break;
            {
                const std::wstring alias = itemText(dlg, kIdAlias), note = itemText(dlg, kIdNote);
                const std::string body = metaPatch(ctx->d.alias, alias, ctx->d.note, note);
                if (!body.empty()) {
                    Reply r = apiSetDeviceMeta(ctx->d.id, body);
                    if (!done(dlg, r, L"Подпись и заметка не сохранены.")) {
                        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                        return TRUE;
                    }
                    ctx->d.alias = cleanText(alias);
                    ctx->d.note = cleanText(note);
                    if (ctx->notify) MMCPropertyChangeNotify(ctx->notify, 0);
                }
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
    }
    return FALSE;
}

UINT CALLBACK deviceCallback(HWND, UINT msg, PROPSHEETPAGEW* page) {
    if (msg == PSPCB_RELEASE && page) {
        auto* ctx = reinterpret_cast<DeviceCtx*>(page->lParam);
        if (ctx && ctx->notify) MMCFreeNotifyHandle(ctx->notify);
        delete ctx;
    }
    return 1;
}

std::wstring pairsText(const std::vector<Row>& rows) {
    std::wstring s;
    for (const Row& r : rows) s += (s.empty() ? L"" : L"\r\n") + r.name + L" = " + r.value;
    return s.empty() ? L"—" : s;
}

// --- страница «только чтение»: заголовок и текст ---

struct TextPage {
    std::wstring head, text;
};

INT_PTR CALLBACK textPageProc(HWND dlg, UINT msg, WPARAM, LPARAM lp) {
    if (msg != WM_INITDIALOG) return FALSE;
    auto* p = reinterpret_cast<TextPage*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
    label(dlg, p->head.c_str(), 7, 7, 238, 18);
    readOnlyText(dlg, p->text, 7, 27, 238, 184, kIdStateText);
    return TRUE;
}

UINT CALLBACK textPageCallback(HWND, UINT msg, PROPSHEETPAGEW* page) {
    if (msg == PSPCB_RELEASE && page) delete reinterpret_cast<TextPage*>(page->lParam);
    return 1;
}

// makePage — страница свойств по шаблону с заголовком title (шаблоны живут, пока загружена DLL:
// страница создаётся, когда её впервые показывают).
HPROPSHEETPAGE makePage(const wchar_t* title, DLGPROC proc, LPFNPSPCALLBACKW callback, void* ctx) {
    static std::mutex mu;
    static std::map<std::wstring, std::vector<WORD>> templates;
    const WORD* tmpl;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto& t = templates[title];
        if (t.empty()) t = dialogTemplate(title, 252, 218, WS_CHILD | WS_DISABLED | WS_CAPTION);
        tmpl = t.data();
    }
    PROPSHEETPAGEW psp{sizeof(psp)};
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USECALLBACK;
    psp.hInstance = g_instance;
    psp.pResource = reinterpret_cast<LPCDLGTEMPLATEW>(tmpl);
    psp.pfnDlgProc = proc;
    psp.pfnCallback = callback;
    psp.lParam = reinterpret_cast<LPARAM>(ctx);
    return CreatePropertySheetPageW(&psp);
}

}  // namespace

bool issueWorkstationToken(HWND parent, const std::string& id, bool isNew) {
    const std::wstring wid = widen(id);
    if (!isNew && ask(parent, L"Выдать рабочему месту «" + wid + L"» новый токен?\n\nПрежний токен перестанет действовать: драйвер на "
                                  L"этом рабочем месте не подключится, пока в нём не задан новый.",
                      MB_ICONQUESTION) != IDYES)
        return false;
    std::string token;
    if (!done(parent, apiIssueWorkstationToken(id, token), L"Токен рабочего места не выдан.")) return false;
    if (token.empty()) {
        fail(parent, L"Токен рабочего места выдан, но сервер его не вернул.", L"выдайте ещё раз");
        return true;
    }
    showTokenDialog(parent, L"Токен рабочего места «" + wid + L"»:", token,
                    L"Сохраните его: он показывается один раз, сервер хранит только хеш. Задайте его в параметре «Токен "
                    L"устройства» драйвера на этом рабочем месте (1С: «Подключение и настройка оборудования»).");
    SecureZeroMemory(&token[0], token.size());
    return true;
}

bool revokeWorkstationToken(HWND parent, const std::string& id) {
    if (ask(parent, L"Отозвать токен рабочего места «" + widen(id) + L"»?\n\nДрайвер с этим токеном больше не подключится; "
                        L"без токена рабочего места действует общий токен устройств или открытый режим.") != IDYES)
        return false;
    return done(parent, apiRevokeWorkstationToken(id), L"Токен не отозван.");
}

bool newWorkstation(HWND parent) {
    std::wstring id;
    if (!askText(parent, L"Эмулкон — новое рабочее место",
                 L"Код рабочего места — как в параметре «Рабочее место» драйвера (обычно имя компьютера кассы). "
                 L"Сразу будет выдан токен для драйвера.",
                 id))
        return false;
    if (id.empty()) return false;
    return issueWorkstationToken(parent, narrow(id), true);
}

bool resetDevice(HWND parent, const Device& d) {
    if (ask(parent, L"Сбросить настройки устройства «" + deviceTitle(d) + L"» к настройкам класса?\n\nСостояние модели "
                        L"(смена, чеки, бумага) не меняется.") != IDYES)
        return false;
    return done(parent, apiResetDevice(d.id), L"Настройки не сброшены.");
}

bool deleteDevice(HWND parent, const Device& d) {
    if (ask(parent, L"Удалить устройство «" + deviceTitle(d) + L"» (" + widen(d.id) + L")?\n\nСостояние, настройки, подпись и "
                        L"заметка пропадут. Драйвер при следующем подключении заведёт устройство заново.") != IDYES)
        return false;
    return done(parent, apiDeleteDevice(d.id), L"Устройство не удалено.");
}

HPROPSHEETPAGE createWorkstationPage(LONG_PTR notify, const Workstation& w) {
    auto* ctx = new WorkstationCtx{notify, w};
    HPROPSHEETPAGE p = makePage(L"Общие", workstationProc, workstationCallback, ctx);
    if (!p) delete ctx;
    return p;
}

HPROPSHEETPAGE createDevicePage(LONG_PTR notify, const Device& d, const std::wstring& classTitle) {
    auto* ctx = new DeviceCtx{notify, d, classTitle};
    HPROPSHEETPAGE p = makePage(L"Общие", deviceProc, deviceCallback, ctx);
    if (!p) delete ctx;
    return p;
}

HPROPSHEETPAGE createDeviceStatePage(const Device& d) {
    return createTextPage(L"Состояние", L"Состояние модели и настройки устройства на момент открытия (docs/models/).",
                          L"Состояние:\r\n" + pairsText(d.state) + L"\r\n\r\nНастройки:\r\n" + pairsText(d.settings));
}

HPROPSHEETPAGE createTextPage(const wchar_t* title, const std::wstring& head, const std::wstring& text) {
    auto* p = new TextPage{head, text};
    HPROPSHEETPAGE page = makePage(title, textPageProc, textPageCallback, p);
    if (!page) delete p;
    return page;
}

// --- сценарии и журнал (этап 4) ---

namespace {

// fileDialog — выбрать файл для записи (save) или чтения. filter — пары «название\0маска\0», в конце \0.
bool fileDialog(HWND parent, bool save, const wchar_t* title, const wchar_t* filter, const wchar_t* ext, std::wstring& path) {
    wchar_t buf[MAX_PATH * 2] = {};
    wcsncpy_s(buf, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    path = buf;
    return true;
}

std::wstring today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u%02u%02u-%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

bool writeText(const std::wstring& path, const std::string& text, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    if (!ok) err = L"не записать " + path + L" (код " + std::to_wstring(GetLastError()) + L")";
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

bool readText(const std::wstring& path, std::string& text, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    bool ok = h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        text.assign((size_t)size.QuadPart, '\0');
        DWORD read = 0;
        ok = text.empty() || (ReadFile(h, &text[0], (DWORD)text.size(), &read, nullptr) && read == text.size());
    }
    if (!ok) err = L"не прочитать " + path + L" (файл больше 16 МБ или недоступен)";
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

bool fetchScenarios(HWND parent, std::vector<Scenario>& list) {
    Reply r = apiRequest("GET", "/control/v1/scenarios");
    std::wstring err;
    if (r.status != 200) {
        fail(parent, L"Сценарии не получены.", replyError(r));
        return false;
    }
    if (!parseScenarios(r.body, list, err)) {
        fail(parent, L"Сценарии не получены.", err);
        return false;
    }
    return true;
}

}  // namespace

bool toggleScenario(HWND parent, const Scenario& s) {
    const std::string body = ruleForPut(s.raw, !s.enabled);
    if (body.empty()) {
        fail(parent, L"Правило не изменено.", L"непонятное правило");
        return false;
    }
    return done(parent, apiPutScenario(s.id, body), s.enabled ? L"Правило не выключено." : L"Правило не включено.");
}

bool deleteScenario(HWND parent, const Scenario& s) {
    if (ask(parent, L"Удалить правило «" + (s.name.empty() ? widen(s.id) : s.name) + L"»?\n\nЧтобы вернуть его, понадобится "
                        L"выгрузка сценариев или веб-интерфейс.") != IDYES)
        return false;
    return done(parent, apiDeleteScenario(s.id), L"Правило не удалено.");
}

bool exportScenariosDialog(HWND parent) {
    std::vector<Scenario> list;
    if (!fetchScenarios(parent, list)) return false;
    std::wstring path = L"emulkon-scenarios-" + today() + L".json", err;
    if (!fileDialog(parent, true, L"Выгрузить сценарии", L"Сценарии Эмулкона (*.json)\0*.json\0Все файлы\0*.*\0", L"json", path))
        return false;
    if (!writeText(path, scenariosFile(list), err)) {
        fail(parent, L"Сценарии не выгружены.", err);
        return false;
    }
    MessageBoxW(parent, (L"Выгружено правил: " + std::to_wstring(list.size()) + L"\n" + path).c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
    return false;  // на сервере ничего не изменилось
}

bool importScenariosDialog(HWND parent) {
    std::wstring path, err;
    if (!fileDialog(parent, false, L"Загрузить сценарии", L"Сценарии Эмулкона (*.json)\0*.json\0Все файлы\0*.*\0", L"json", path))
        return false;
    std::string text;
    if (!readText(path, text, err)) {
        fail(parent, L"Сценарии не загружены.", err);
        return false;
    }
    std::vector<Scenario> current;
    if (!fetchScenarios(parent, current)) return false;
    if (ask(parent, L"Загрузить сценарии из файла?\n\n" + path + L"\n\nПравила с тем же кодом будут заменены, новые — добавлены; "
                        L"остальные правила сервера останутся.",
            MB_ICONQUESTION) != IDYES)
        return false;
    ImportResult r = importScenarios(text, current);
    std::wstring msg = L"Добавлено правил: " + std::to_wstring(r.created) + L", заменено: " + std::to_wstring(r.replaced) + L".";
    for (size_t i = 0; i < r.errors.size() && i < 10; ++i) msg += L"\n• " + r.errors[i];
    if (r.errors.size() > 10) msg += L"\n… и ещё " + std::to_wstring(r.errors.size() - 10);
    MessageBoxW(parent, msg.c_str(), kTitle, MB_OK | (r.errors.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
    return r.created + r.replaced > 0;
}

bool exportJournalDialog(HWND parent) {
    const std::wstring filter = journalFilter();
    std::wstring path = L"emulkon-journal-" + today() + L".jsonl", err;
    if (!fileDialog(parent, true, filter.empty() ? L"Выгрузить журнал" : L"Выгрузить журнал (с отбором)",
                    L"Журнал Эмулкона (*.jsonl)\0*.jsonl\0Все файлы\0*.*\0", L"jsonl", path))
        return false;
    long long lines = 0;
    bool ok;
    {
        WaitCursor wait;
        ok = exportJournal(path, filter, lines, err);
    }
    if (!ok) {
        fail(parent, L"Журнал не выгружен.", err);
        return false;
    }
    MessageBoxW(parent,
                (L"Выгружено записей: " + std::to_wstring(lines) + (filter.empty() ? L"" : L" (отбор «" + filter + L"»)") + L"\n" + path)
                    .c_str(),
                kTitle, MB_OK | MB_ICONINFORMATION);
    return false;
}

bool askJournalFilter(HWND parent) {
    std::wstring text = journalFilter();
    if (!askText(parent, L"Эмулкон — отбор журнала",
                 L"Текст для поиска в записях: метод, устройство, рабочее место, данные. Пусто — без отбора.", text))
        return false;
    setJournalFilter(text);
    return true;
}

// --- обновления, выгрузка и загрузка настроек (этап 5) ---

bool checkUpdatesDialog(HWND parent) {
    Release rel;
    std::wstring err;
    bool ok;
    {
        WaitCursor wait;
        ok = fetchRelease(rel, err);
    }
    if (!ok) {
        fail(parent, L"Не удалось проверить обновления.", err);
        return false;
    }
    const std::wstring installed = widen(EMULKON_VERSION), latest = widen(rel.version);
    const std::wstring when = rel.published.empty() ? L"" : L" от " + widen(rel.published);
    if (compareVersions(rel.version, EMULKON_VERSION) <= 0) {
        MessageBoxW(parent, (L"Установлена последняя версия: " + installed + L".\nПоследний выпуск — " + latest + when + L".").c_str(),
                    kTitle, MB_OK | MB_ICONINFORMATION);
        return false;
    }
    std::wstring text = L"Вышла версия " + latest + when + L". Установлена " + installed + L".";
    const std::wstring notes = plainNotes(rel.notes);
    if (!notes.empty()) text += L"\n\nЧто нового:\n" + notes;
    text += L"\n\nСкачать и установить? Установщик проверяется по SHA-256 из описания выпуска; откроется мастер "
            L"обновления — он перезапустит службу. Драйверы в базах 1С обновятся сами при подключении.";
    if (ask(parent, text, MB_ICONQUESTION) != IDYES) return false;
    std::wstring path;
    {
        WaitCursor wait;
        // Не в ProgramData: exe, скачанный в общий каталог и запущенный оттуда, антивирус считает подозрительным.
        ok = downloadRelease(rel, userDownloadsDir(), path, err);
    }
    if (!ok) {
        fail(parent, L"Обновление не скачано.", err);
        return false;
    }
    if (!runInstaller(path, err)) {
        fail(parent, L"Установщик не запустился.", err);
        return false;
    }
    MessageBoxW(parent,
                (L"Запущен мастер обновления до " + latest + L".\n" + path +
                 L"\n\nЗакройте консоль: мастер заменит и её файлы.")
                    .c_str(),
                kTitle, MB_OK | MB_ICONINFORMATION);
    return false;
}

bool exportSettingsDialog(HWND parent) {
    std::vector<Setting> values;
    std::wstring err;
    if (!readSettings(values, err)) {
        fail(parent, L"Настройки не выгружены.", err);
        return false;
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computer, &n);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring path = L"emulkon-settings-" + today() + L".json";
    if (!fileDialog(parent, true, L"Выгрузить настройки службы", L"Настройки Эмулкона (*.json)\0*.json\0Все файлы\0*.*\0",
                    L"json", path))
        return false;
    if (!writeText(path, settingsFile(values, narrow(computer), stamp), err)) {
        fail(parent, L"Настройки не выгружены.", err);
        return false;
    }
    MessageBoxW(parent,
                (L"Настройки службы выгружены:\n" + path + L"\n\n" + settingsSummary(values) +
                 L"\nТокенов в файле нет — только их SHA-256: после загрузки на другом компьютере подойдут те же токены.")
                    .c_str(),
                kTitle, MB_OK | MB_ICONINFORMATION);
    return false;
}

bool importSettingsDialog(HWND parent) {
    std::wstring path, err;
    if (!fileDialog(parent, false, L"Загрузить настройки службы", L"Настройки Эмулкона (*.json)\0*.json\0Все файлы\0*.*\0",
                    L"json", path))
        return false;
    std::string text;
    std::vector<Setting> values;
    std::vector<std::wstring> warnings;
    if (!readText(path, text, err) || !parseSettingsFile(text, values, warnings, err)) {
        fail(parent, L"Настройки не загружены.", err);
        return false;
    }
    std::wstring q = L"Загрузить настройки службы из файла?\n\n" + path + L"\n\n" + settingsSummary(values) +
                     L"\nЗначения, которых нет в файле, станут по умолчанию. Служба будет перезапущена.";
    if (readServerSettings().policy) q += L"\n\nЕсть групповая политика Эмулкона: её значения важнее файла.";
    for (const std::wstring& w : warnings) q += L"\n• " + w;
    if (ask(parent, q, MB_ICONQUESTION) != IDYES) return false;
    bool ok;
    {
        WaitCursor wait;
        ok = applySettings(values, err);
    }
    if (!ok) {
        fail(parent, L"Настройки загружены не полностью.", err);
        return true;
    }
    MessageBoxW(parent, L"Настройки службы загружены.", kTitle, MB_OK | MB_ICONINFORMATION);
    return true;
}

}  // namespace console
