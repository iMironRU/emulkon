// EmulkonSetup — установщик Эмулкона для Windows (см. setup.h).
//
//   EmulkonSetup.exe                 мастер установки или обновления
//   EmulkonSetup.exe /maintain       мастер обслуживания: изменить, восстановить, удалить («Изменить» в
//                                    «Установленных приложениях», docs/local-install.md §3)
//   EmulkonSetup.exe /modify         изменить параметры установленного (с /quiet — сразу)
//   EmulkonSetup.exe /repair         восстановить установленное: файлы, служба, ярлыки; данные не трогаются
//   EmulkonSetup.exe /check          проверка работы установленного Эмулкона
//   EmulkonSetup.exe /uninstall      удаление
//
// Ключи: /quiet — без окон (итог — код возврата и /log), /dir <каталог>, /autostart | /noautostart,
//        /network | /nonetwork (сервер для касс в сети), /norun, /purge (удалить и данные), /log <файл>.
//        Для /modify и /repair непереданные параметры берутся из установленного.
// Код возврата: 0 — успешно, 1 — есть ошибки, 2 — неверные ключи.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doc.h"
#include "setup.h"

using namespace setup;

namespace {

// Maintain — экран обслуживания; выбранное действие на нём — Modify, Repair или Uninstall.
enum class Mode { Install, Uninstall, Check, Maintain, Modify, Repair };

struct Args {
    Mode mode = Mode::Install;
    bool quiet = false;
    bool dirGiven = false;
    bool autostartGiven = false;
    bool networkGiven = false;
    bool bad = false;
    Options o;
    std::wstring log;
};

Args parseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring s = argv[i];
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        if (!s.empty() && s[0] == L'-') s[0] = L'/';
        if (s == L"/check") a.mode = Mode::Check;
        else if (s == L"/uninstall") a.mode = Mode::Uninstall;
        else if (s == L"/maintain") a.mode = Mode::Maintain;
        else if (s == L"/modify") a.mode = Mode::Modify;
        else if (s == L"/repair") a.mode = Mode::Repair;
        else if (s == L"/quiet" || s == L"/s") a.quiet = true;
        else if (s == L"/autostart" || s == L"/noautostart") {
            a.o.autostart = s == L"/autostart";
            a.autostartGiven = true;
        } else if (s == L"/network" || s == L"/nonetwork") {
            a.o.network = s == L"/network";
            a.networkGiven = true;
        } else if (s == L"/norun") a.o.run = false;
        else if (s == L"/purge") a.o.purge = true;
        else if (s == L"/dir" && i + 1 < argc) {
            a.o.dir = argv[++i];
            a.dirGiven = true;
        } else if (s == L"/log" && i + 1 < argc) {
            a.log = argv[++i];
        } else {
            a.bad = true;
        }
    }
    LocalFree(argv);
    while (a.o.dir.size() > 3 && (a.o.dir.back() == L'\\' || a.o.dir.back() == L'/')) a.o.dir.pop_back();
    // Обслуживание: параметры — как у установленного, если ключом не сказано иное.
    if (a.mode == Mode::Maintain || a.mode == Mode::Modify || a.mode == Mode::Repair) {
        Options cur = currentOptions();
        if (!a.autostartGiven) a.o.autostart = cur.autostart;
        if (!a.networkGiven) a.o.network = cur.network;
        if (!a.dirGiven) a.o.dir = cur.dir;
    }
    return a;
}

// --- журнал в файл (/log) ---

std::mutex g_logMu;
FILE* g_log = nullptr;

void logItem(const Item& it) {
    std::lock_guard<std::mutex> lock(g_logMu);
    if (!g_log) return;
    const char* tag = it.status == Status::Ok ? "OK  " : it.status == Status::Warn ? "WARN" : it.status == Status::Fail ? "FAIL" : "INFO";
    std::fprintf(g_log, "[%s] %s — %s\n", tag, narrow(it.title).c_str(), narrow(it.detail).c_str());
    std::fflush(g_log);
}

void logLine(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(g_logMu);
    if (g_log) std::fprintf(g_log, "%s\n", narrow(s).c_str()), std::fflush(g_log);
}

std::wstring installDir(const Options& o) {
    if (!o.dir.empty()) return o.dir;
    Installed inst = findInstalled();
    return inst.present ? inst.dir : defaultInstallDir();
}

// --- окно ---

const UINT WM_APP_ITEM = WM_APP + 1;     // lParam = Item*
const UINT WM_APP_SECTION = WM_APP + 2;  // lParam = std::wstring*
const UINT WM_APP_DONE = WM_APP + 3;     // wParam = успех

enum Id : int {
    kIdDirEdit = 100,
    kIdBrowse,
    kIdAutostart,
    kIdNetwork,
    kIdPurge,
    kIdChoiceModify,
    kIdChoiceRepair,
    kIdChoiceRemove,
    kIdList,
    kIdDetail,
    kIdPrimary,
    kIdSecondary,
    kIdClose,
};

enum class Phase { Checking, Ready, Working, Done, Failed };

struct Row {
    Item item;
    bool section = false;
};

struct Ui {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Mode mode = Mode::Install;
    Mode choice = Mode::Modify;  // на экране обслуживания (Maintain): Modify, Repair или Uninstall
    Options o;
    Installed installed;
    Phase phase = Phase::Checking;
    Tally tally;
    std::vector<Row> rows;
    int dpi = 96;
    HFONT font = nullptr, fontTitle = nullptr;
    HICON logo = nullptr;
    HWND title = nullptr, subtitle = nullptr, dirLabel = nullptr, dirEdit = nullptr, browse = nullptr;
    HWND autostart = nullptr, network = nullptr, purge = nullptr;
    HWND choiceModify = nullptr, choiceRepair = nullptr, choiceRemove = nullptr;
    HWND list = nullptr, detail = nullptr, status = nullptr;
    HWND primary = nullptr, secondary = nullptr, close = nullptr;
};

Ui g;

int S(int px) { return MulDiv(px, g.dpi, 96); }

HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0,
             DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

void setText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

void show(HWND h, bool on) { ShowWindow(h, on ? SW_SHOW : SW_HIDE); }

void addRow(const Row& r) {
    g.rows.push_back(r);
    int index = static_cast<int>(g.rows.size()) - 1;
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = index;
    it.lParam = index;
    std::wstring status = r.section ? std::wstring() : statusText(r.item.status);
    it.pszText = const_cast<wchar_t*>(status.c_str());
    ListView_InsertItem(g.list, &it);
    ListView_SetItemText(g.list, index, 1, const_cast<wchar_t*>(r.item.title.c_str()));
    ListView_SetItemText(g.list, index, 2, const_cast<wchar_t*>(r.item.detail.c_str()));
    ListView_EnsureVisible(g.list, index, FALSE);
    if (!r.section) {
        g.tally.add(r.item);
        if (r.item.status == Status::Fail || r.item.status == Status::Warn) {
            ListView_SetItemState(g.list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
}

void clearRows() {
    g.rows.clear();
    g.tally = Tally{};
    ListView_DeleteAllItems(g.list);
    setText(g.detail, L"Щёлкните строку — здесь будут подробности целиком.");
}

std::wstring tallyText(const Tally& t) {
    std::wstring s = L"готово: " + std::to_wstring(t.ok);
    if (t.warn) s += L" · внимание: " + std::to_wstring(t.warn);
    if (t.fail) s += L" · ошибок: " + std::to_wstring(t.fail);
    return s;
}

// --- работа в фоне ---

Sink uiSink() {
    HWND hwnd = g.hwnd;
    return [hwnd](const Item& it) {
        logItem(it);
        PostMessageW(hwnd, WM_APP_ITEM, 0, reinterpret_cast<LPARAM>(new Item(it)));
    };
}

void section(const std::wstring& s) {
    logLine(L"== " + s);
    PostMessageW(g.hwnd, WM_APP_SECTION, 0, reinterpret_cast<LPARAM>(new std::wstring(s)));
}

void updateButtons();

void startChecks() {
    g.phase = Phase::Checking;
    clearRows();
    updateButtons();
    Mode mode = g.mode;
    Options o = g.o;
    HWND hwnd = g.hwnd;
    Sink sink = uiSink();
    std::thread([=] {
        bool ok;
        if (mode == Mode::Check) {
            section(L"Проверка работы");
            ok = workChecks(installDir(o), sink).fail == 0;
        } else {
            section(L"Перед установкой");
            ok = preChecks(o, sink).fail == 0;
        }
        PostMessageW(hwnd, WM_APP_DONE, ok, 0);
    }).detach();
}

// action — что делает кнопка: на экране обслуживания — выбранный вариант.
Mode action() { return g.mode == Mode::Maintain ? g.choice : g.mode; }

// runAction — установка, изменение, восстановление или удаление; section пишет заголовки (окно или лог).
bool runAction(Mode mode, Options o, const Sink& sink, const std::function<void(const std::wstring&)>& section) {
    bool ok = false;
    switch (mode) {
        case Mode::Uninstall:
            section(L"Удаление");
            return uninstall(o, sink);
        case Mode::Modify:
            section(L"Изменение");
            ok = modify(o, sink);
            break;
        case Mode::Repair:
            section(L"Восстановление");
            o.run = true;  // трей у этого пользователя — заново
            ok = install(o, sink);
            break;
        default:
            section(L"Установка");
            ok = install(o, sink);
            if (!o.run) return ok;
            break;
    }
    if (!ok) return false;
    section(L"Проверка работы");
    return workChecks(installDir(o), sink).fail == 0;
}

void startWork() {
    g.phase = Phase::Working;
    updateButtons();
    Mode mode = action();
    Options o = g.o;
    HWND hwnd = g.hwnd;
    Sink sink = uiSink();
    std::thread([=] {
        bool ok = runAction(mode, o, sink, section);
        PostMessageW(hwnd, WM_APP_DONE, ok, 1);
    }).detach();
}

// startFixAndWork — «Исправить и обновить»: устранить помехи, что проверки пометили исправимыми (fixBlockers),
// проверить заново и, если ошибок нет, ставить.
void startFixAndWork() {
    g.phase = Phase::Working;
    clearRows();
    updateButtons();
    Options o = g.o;
    HWND hwnd = g.hwnd;
    Sink sink = uiSink();
    std::thread([=] {
        section(L"Исправление");
        fixBlockers(o, sink);
        section(L"Перед установкой");
        if (preChecks(o, sink).fail) {
            PostMessageW(hwnd, WM_APP_DONE, 0, 0);  // осталось что-то, что исправит только человек
            return;
        }
        bool ok = runAction(Mode::Install, o, sink, section);
        PostMessageW(hwnd, WM_APP_DONE, ok, 1);
    }).detach();
}

// confirmFix — что мастер сделает сам: строки проверок с Item::fix; «Да» — исправлять.
bool confirmFix() {
    std::wstring list;
    for (const Row& r : g.rows) {
        if (!r.section && !r.item.fix.empty()) list += L"\n• " + r.item.fix;
    }
    const std::wstring text = L"Мастер исправит сам:" + list + L"\n\nЗатем проверит всё заново и " +
                              (!g.installed.present ? L"установит Эмулкон." : g.installed.version == version() ? L"переустановит Эмулкон." : L"обновит Эмулкон.") + L" Продолжить?";
    return MessageBoxW(g.hwnd, text.c_str(), L"Эмулкон: исправить и продолжить", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void layout();

void updateButtons() {
    bool busy = g.phase == Phase::Checking || g.phase == Phase::Working;
    bool done = g.phase == Phase::Done;
    bool maint = g.mode == Mode::Maintain;
    Mode act = action();
    bool editable = g.mode == Mode::Install && !busy && !done && !g.installed.present;
    EnableWindow(g.dirEdit, editable);
    EnableWindow(g.browse, editable);
    bool optionsOn = !busy && !done && (g.mode == Mode::Install || (maint && act == Mode::Modify));
    EnableWindow(g.autostart, optionsOn);
    EnableWindow(g.network, optionsOn);
    EnableWindow(g.purge, act == Mode::Uninstall && !busy && !done);
    for (HWND r : {g.choiceModify, g.choiceRepair, g.choiceRemove}) EnableWindow(r, maint && !busy && !done);
    if (maint) {
        show(g.autostart, act == Mode::Modify);
        show(g.network, act == Mode::Modify);
        show(g.purge, act == Mode::Uninstall);
        layout();
    }
    EnableWindow(g.close, !busy);

    std::wstring p, s, c = L"Закрыть";
    bool pOn = !busy, sOn = !busy;
    switch (g.mode) {
        case Mode::Maintain:
            show(g.secondary, false);
            show(g.primary, !(done && act == Mode::Uninstall));
            if (done) {
                p = L"Открыть веб-интерфейс";
                c = L"Готово";
            } else {
                p = act == Mode::Modify ? L"Изменить" : act == Mode::Repair ? L"Восстановить" : L"Удалить";
                c = g.phase == Phase::Failed ? L"Закрыть" : L"Отмена";
            }
            break;
        case Mode::Modify:
        case Mode::Repair:
            break;  // в окне — только через экран обслуживания (runUi)
        case Mode::Install:
            if (g.phase == Phase::Done) {
                p = L"Открыть веб-интерфейс";
                s = L"Загрузки для 1С";
                c = L"Готово";
            } else {
                p = !g.installed.present           ? std::wstring(L"Установить")
                    : g.installed.version == version() ? std::wstring(L"Переустановить")
                                                       : L"Обновить до " + version();
                s = L"Проверить снова";
                c = L"Отмена";
                pOn = !busy && g.tally.fail == 0;
                if (g.phase == Phase::Failed) p = L"Повторить установку";
                if (g.phase == Phase::Ready && g.tally.canFix()) {  // помехи, что мастер устранит сам
                    p = !g.installed.present               ? L"Исправить и установить"
                        : g.installed.version == version() ? L"Исправить и переустановить"
                                                           : L"Исправить и обновить";
                    pOn = true;
                }
            }
            break;
        case Mode::Uninstall:
            p = L"Удалить";
            c = g.phase == Phase::Done || g.phase == Phase::Failed ? L"Закрыть" : L"Отмена";
            show(g.primary, g.phase != Phase::Done);
            show(g.secondary, false);
            break;
        case Mode::Check:
            p = L"Открыть веб-интерфейс";
            s = L"Проверить снова";
            break;
    }
    setText(g.primary, p);
    setText(g.secondary, s);
    setText(g.close, c);
    EnableWindow(g.primary, pOn);
    EnableWindow(g.secondary, sOn);

    std::wstring st;
    switch (g.phase) {
        case Phase::Checking: st = L"Проверка…"; break;
        case Phase::Working:
            st = act == Mode::Uninstall ? L"Удаление…"
                 : act == Mode::Modify  ? L"Изменение…"
                 : act == Mode::Repair  ? L"Восстановление…"
                                        : L"Установка…";
            break;
        case Phase::Ready:
            st = maint ? L"Выберите действие"
                       : tallyText(g.tally) + (g.tally.canFix() ? L" — исправимо"
                                               : g.tally.fail   ? L" — исправьте ошибки и проверьте снова"
                                                                : L"");
            break;
        case Phase::Done:
            st = act == Mode::Uninstall ? L"Эмулкон удалён"
                 : act == Mode::Check   ? L"Эмулкон работает · " + tallyText(g.tally)
                 : act == Mode::Modify  ? L"Параметры изменены, Эмулкон работает"
                 : act == Mode::Repair  ? L"Эмулкон восстановлен и работает"
                                        : L"Эмулкон " + version() + L" установлен и работает";
            break;
        case Phase::Failed:
            st = (g.mode == Mode::Check ? L"Есть проблемы · " : L"Не получилось · ") + tallyText(g.tally);
            break;
    }
    setText(g.status, st);
}

void openUrl(const wchar_t* path) {
    std::wstring url = std::wstring(kServerUrl) + path;
    ShellExecuteW(g.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void browseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Каталог установки Эмулкона");
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring p = path;
                if (_wcsicmp(fileName(p).c_str(), L"Emulkon") != 0) p += L"\\Emulkon";
                setText(g.dirEdit, p);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

std::wstring editText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void readOptions() {
    if (g.mode == Mode::Install && !g.installed.present) g.o.dir = editText(g.dirEdit);
    g.o.autostart = SendMessageW(g.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.o.network = SendMessageW(g.network, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.o.purge = SendMessageW(g.purge, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void onCommand(int id, int code) {
    switch (id) {
        case kIdBrowse:
            browseFolder();
            return;
        case kIdNetwork:
            if (code == BN_CLICKED && g.mode == Mode::Install && g.phase == Phase::Ready) {  // предупреждение — в проверках
                readOptions();
                startChecks();
            }
            return;
        case kIdChoiceModify:
        case kIdChoiceRepair:
        case kIdChoiceRemove:
            if (code == BN_CLICKED) {
                g.choice = id == kIdChoiceModify ? Mode::Modify : id == kIdChoiceRepair ? Mode::Repair : Mode::Uninstall;
                setText(g.detail, g.choice == Mode::Modify   ? L"Автозапуск трея и сервер для касс в сети. Файлы и данные не трогаются; "
                                                               L"при смене адреса служба перезапустится."
                                  : g.choice == Mode::Repair ? L"Файлы программы заново из этого мастера, служба, права, ярлыки, "
                                                               L"записи реестра, затем проверка работы. Данные и настройки не трогаются."
                                                             : L"Служба, программа, ярлыки и записи будут удалены. Данные службы "
                                                               L"остаются, если не отмечено «Удалить и данные».");
                updateButtons();
            }
            return;
        case kIdPrimary:
            if (g.mode == Mode::Check || ((g.mode == Mode::Install || g.mode == Mode::Maintain) && g.phase == Phase::Done)) {
                openUrl(L"/");
                return;
            }
            readOptions();
            if (g.mode == Mode::Install && g.phase == Phase::Ready && g.tally.canFix()) {
                if (confirmFix()) startFixAndWork();
                return;
            }
            if (action() != Mode::Install) clearRows();
            startWork();
            return;
        case kIdSecondary:
            if (g.mode == Mode::Install && g.phase == Phase::Done) {
                openUrl(L"/downloads");
                return;
            }
            readOptions();
            startChecks();
            return;
        case kIdClose:
            DestroyWindow(g.hwnd);
            return;
    }
}

COLORREF statusColor(Status s) {
    switch (s) {
        case Status::Ok: return RGB(0x1a, 0x7f, 0x37);
        case Status::Warn: return RGB(0xb4, 0x53, 0x09);
        case Status::Fail: return RGB(0xc6, 0x28, 0x28);
        case Status::Info: return RGB(0x68, 0x70, 0x76);
    }
    return GetSysColor(COLOR_WINDOWTEXT);
}

LRESULT onCustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            size_t i = static_cast<size_t>(cd->nmcd.lItemlParam);
            if (i >= g.rows.size()) return CDRF_DODEFAULT;
            const Row& r = g.rows[i];
            if (r.section) {
                cd->clrTextBk = RGB(0xf1, 0xf3, 0xf5);
                cd->clrText = RGB(0x1c, 0x20, 0x24);
            } else {
                cd->clrTextBk = GetSysColor(COLOR_WINDOW);
                cd->clrText = cd->iSubItem == 0 ? statusColor(r.item.status) : GetSysColor(COLOR_WINDOWTEXT);
            }
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

void layout() {
    bool install = g.mode == Mode::Install;
    int y = 76;
    if (install) {
        SetWindowPos(g.dirLabel, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        y += 20;
        SetWindowPos(g.dirEdit, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetWindowPos(g.browse, nullptr, S(604), S(y - 1), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        y += 36;
        SetWindowPos(g.autostart, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        y += 24;
        SetWindowPos(g.network, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        y += 32;
    } else if (g.mode == Mode::Uninstall) {
        SetWindowPos(g.purge, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        y += 32;
    } else if (g.mode == Mode::Maintain) {
        for (HWND r : {g.choiceModify, g.choiceRepair, g.choiceRemove}) {
            SetWindowPos(r, nullptr, S(20), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            y += 24;
        }
        y += 8;
        if (g.choice == Mode::Modify) {
            SetWindowPos(g.autostart, nullptr, S(40), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            SetWindowPos(g.network, nullptr, S(40), S(y + 24), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            y += 56;
        } else if (g.choice == Mode::Uninstall) {
            SetWindowPos(g.purge, nullptr, S(40), S(y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            y += 32;
        }
    }
    int listH = 520 - y - 66;
    SetWindowPos(g.list, nullptr, S(20), S(y), S(680), S(listH), SWP_NOZORDER);
    SetWindowPos(g.detail, nullptr, S(20), S(y + listH + 8), S(680), S(58), SWP_NOZORDER);
}

void createControls() {
    g.logo = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(40), S(40), LR_DEFAULTCOLOR));
    HWND logo = control(L"STATIC", nullptr, SS_ICON | SS_REALSIZEIMAGE, 20, 16, 40, 40);
    SendMessageW(logo, STM_SETICON, reinterpret_cast<WPARAM>(g.logo), 0);
    g.title = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 72, 14, 620, 28);
    SendMessageW(g.title, WM_SETFONT, reinterpret_cast<WPARAM>(g.fontTitle), TRUE);
    g.subtitle = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 72, 42, 630, 34);

    g.dirLabel = control(L"STATIC", L"Каталог установки (для всех пользователей компьютера; служба Emulkon, данные — в ProgramData\\Emulkon):",
                         SS_LEFT, 20, 76, 680, 18);
    g.dirEdit = control(L"EDIT", g.o.dir.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 20, 96, 576, 26, kIdDirEdit, WS_EX_CLIENTEDGE);
    g.browse = control(L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, 604, 95, 96, 28, kIdBrowse);
    g.autostart = control(L"BUTTON", L"Запускать трей при входе в Windows у каждого пользователя (значок у часов; сервер работает службой)",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 20, 132, 680, 22, kIdAutostart);
    g.network = control(L"BUTTON", L"Сервер для касс в сети: принимать подключения с других компьютеров (порт 8737 в брандмауэре)",
                        BS_AUTOCHECKBOX | WS_TABSTOP, 20, 156, 680, 22, kIdNetwork);
    g.purge = control(L"BUTTON", L"Удалить и данные: устройства, журнал и копии службы (ProgramData\\Emulkon), настройки трея, кэш драйвера",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 20, 76, 680, 22, kIdPurge);
    SendMessageW(g.autostart, BM_SETCHECK, g.o.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.network, BM_SETCHECK, g.o.network ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.purge, BM_SETCHECK, g.o.purge ? BST_CHECKED : BST_UNCHECKED, 0);
    // Экран обслуживания: три варианта (docs/local-install.md §3); подписи флажков сжаты под отступ.
    g.choiceModify = control(L"BUTTON", L"Изменить — автозапуск трея, сервер для касс в сети",
                             BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 20, 76, 680, 22, kIdChoiceModify);
    g.choiceRepair = control(L"BUTTON", L"Восстановить — файлы, служба, права и ярлыки заново; данные не трогаются",
                             BS_AUTORADIOBUTTON, 20, 100, 680, 22, kIdChoiceRepair);
    g.choiceRemove = control(L"BUTTON", L"Удалить Эмулкон с этого компьютера", BS_AUTORADIOBUTTON, 20, 124, 680, 22,
                             kIdChoiceRemove);
    SendMessageW(g.choice == Mode::Modify ? g.choiceModify : g.choice == Mode::Repair ? g.choiceRepair : g.choiceRemove,
                 BM_SETCHECK, BST_CHECKED, 0);
    if (g.mode == Mode::Maintain) {
        SetWindowPos(g.autostart, nullptr, 0, 0, S(660), S(22), SWP_NOMOVE | SWP_NOZORDER);
        SetWindowPos(g.network, nullptr, 0, 0, S(660), S(22), SWP_NOMOVE | SWP_NOZORDER);
        SetWindowPos(g.purge, nullptr, 0, 0, S(660), S(22), SWP_NOMOVE | SWP_NOZORDER);
    }

    g.list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                     20, 190, 680, 250, kIdList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const struct {
        const wchar_t* text;
        int width;
    } cols[] = {{L"Итог", 96}, {L"Шаг", 190}, {L"Подробности", 370}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i].text);
        c.cx = S(cols[i].width);
        ListView_InsertColumn(g.list, i, &c);
    }
    g.detail = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 20, 448, 680, 58, kIdDetail,
                       WS_EX_CLIENTEDGE);
    g.status = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 20, 534, 250, 20);
    g.primary = control(L"BUTTON", L"", BS_DEFPUSHBUTTON | WS_TABSTOP, 278, 526, 196, 32, kIdPrimary);
    g.secondary = control(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 482, 526, 112, 32, kIdSecondary);
    g.close = control(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 602, 526, 98, 32, kIdClose);

    bool install = g.mode == Mode::Install;
    show(g.dirLabel, install);
    show(g.dirEdit, install);
    show(g.browse, install);
    show(g.autostart, install);
    show(g.network, install);
    show(g.purge, g.mode == Mode::Uninstall);
    for (HWND r : {g.choiceModify, g.choiceRepair, g.choiceRemove}) show(r, g.mode == Mode::Maintain);

    std::wstring dir = installDir(g.o);
    switch (g.mode) {
        case Mode::Maintain:
            setText(g.title, L"Эмулкон " + (g.installed.present ? g.installed.version : version()) + L": обслуживание");
            setText(g.subtitle, L"Установлен в " + dir + L", служба Emulkon — " + serviceStateText(serviceState()) +
                                    L". Изменить параметры, восстановить установку или удалить Эмулкон.");
            break;
        case Mode::Modify:
        case Mode::Repair:
            break;
        case Mode::Install:
            setText(g.title, !g.installed.present           ? L"Установка Эмулкона " + version()
                             : g.installed.version == version() ? L"Переустановка Эмулкона " + version()
                                                                : L"Эмулкон: обновление " + g.installed.version + L" → " + version());
            setText(g.subtitle,
                    L"Эмулятор подключаемого оборудования для 1С:БПО: служба эмуляции, трей с чеками и драйвер для 1С. "
                    L"Перед установкой и после неё проверяется, что всё может работать.");
            break;
        case Mode::Uninstall:
            setText(g.title, L"Удаление Эмулкона " + (g.installed.present ? g.installed.version : version()));
            setText(g.subtitle, L"Будут удалены программа из " + dir +
                                    L", служба Emulkon, ярлыки в меню «Пуск», автозапуск трея и запись в «Установленных приложениях». "
                                    L"Драйвер в базах 1С удаляется в самой 1С.");
            break;
        case Mode::Check:
            setText(g.title, L"Эмулкон " + (g.installed.present ? g.installed.version : version()) + L": проверка работы");
            setText(g.subtitle, L"Служба, сервер, страница загрузок и драйвер: DLL загружается как в 1С и проходит «Тест "
                                L"устройства» на сервере. Каталог: " + dir);
            break;
    }
    layout();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            createControls();
            if (g.mode == Mode::Uninstall) {
                g.phase = Phase::Ready;
                updateButtons();
                setText(g.detail, L"Нажмите «Удалить». Данные службы (устройства, журнал) и настройки трея по умолчанию остаются.");
            } else if (g.mode == Mode::Maintain) {
                g.phase = Phase::Ready;
                // Подсказка и подписи — как при выборе варианта.
                SendMessageW(hwnd, WM_COMMAND,
                             MAKEWPARAM(g.choice == Mode::Modify ? kIdChoiceModify : g.choice == Mode::Repair ? kIdChoiceRepair
                                                                                                             : kIdChoiceRemove,
                                        BN_CLICKED),
                             0);
            } else {
                setText(g.detail, L"Щёлкните строку — здесь будут подробности целиком.");
                startChecks();
            }
            return 0;
        case WM_APP_ITEM: {
            std::unique_ptr<Item> it(reinterpret_cast<Item*>(lp));
            addRow(Row{*it, false});
            return 0;
        }
        case WM_APP_SECTION: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
            addRow(Row{Item{Status::Info, L"— " + *s + L" —", L""}, true});
            return 0;
        }
        case WM_APP_DONE: {
            bool ok = wp != 0, work = lp != 0;
            if (g.mode == Mode::Check) g.phase = ok ? Phase::Done : Phase::Failed;
            else if (work) g.phase = ok ? Phase::Done : Phase::Failed;
            else g.phase = Phase::Ready;
            if (work && ok && g.mode == Mode::Install) g.installed = findInstalled();
            updateButtons();
            return 0;
        }
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == kIdList && nm->code == NM_CUSTOMDRAW) {
                return onCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lp));
            }
            if (nm->idFrom == kIdList && nm->code == LVN_ITEMCHANGED) {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((lv->uNewState & LVIS_SELECTED) && static_cast<size_t>(lv->iItem) < g.rows.size()) {
                    const Item& it = g.rows[static_cast<size_t>(lv->iItem)].item;
                    setText(g.detail, it.detail.empty() ? it.title : it.title + L": " + it.detail);
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            if (reinterpret_cast<HWND>(lp) == g.title) SetTextColor(dc, RGB(0xc2, 0x41, 0x0c));
            if (reinterpret_cast<HWND>(lp) == g.detail) SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE: {
            if (g.phase == Phase::Working || g.phase == Phase::Checking) return 0;  // дождаться конца шага
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runUi(HINSTANCE inst, const Args& a) {
    g.inst = inst;
    g.mode = a.mode;
    // /modify и /repair в окне — экран обслуживания с уже выбранным вариантом.
    if (a.mode == Mode::Modify || a.mode == Mode::Repair) {
        g.mode = Mode::Maintain;
        g.choice = a.mode;
    }
    g.o = a.o;
    g.installed = findInstalled();
    if (g.mode == Mode::Install && g.o.dir.empty()) g.o.dir = installDir(g.o);
    g.dpi = static_cast<int>(GetDpiForSystem());

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g.fontTitle = CreateFontW(-MulDiv(14, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"EmulkonSetupWnd";
    RegisterClassExW(&wc);

    RECT rc{0, 0, S(720), S(574)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, static_cast<UINT>(g.dpi));
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const wchar_t* caption = g.mode == Mode::Uninstall  ? L"Удаление Эмулкона"
                             : g.mode == Mode::Check    ? L"Эмулкон — проверка работы"
                             : g.mode == Mode::Maintain ? L"Эмулкон — обслуживание"
                                                        : L"Установка Эмулкона";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, caption, style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    bool failed = g.phase == Phase::Failed || g.tally.fail > 0;
    return failed ? 1 : 0;
}

// --- без окон ---

int runQuiet(const Args& a) {
    Sink sink = [](const Item& it) { logItem(it); };
    switch (a.mode) {
        case Mode::Check:
            logLine(L"== Проверка работы");
            return workChecks(installDir(a.o), sink).fail ? 1 : 0;
        case Mode::Uninstall:
            logLine(L"== Удаление");
            return uninstall(a.o, sink) ? 0 : 1;
        case Mode::Modify:
        case Mode::Repair:
            return runAction(a.mode, a.o, sink, [](const std::wstring& s) { logLine(L"== " + s); }) ? 0 : 1;
        case Mode::Maintain:
            return 2;  // без окна нужен конкретный вариант: /modify, /repair или /uninstall
        case Mode::Install: {
            logLine(L"== Перед установкой");
            if (preChecks(a.o, sink).fail) return 1;
            logLine(L"== Установка");
            if (!install(a.o, sink)) return 1;
            if (!a.o.run) return 0;
            logLine(L"== Проверка работы");
            return workChecks(installDir(a.o), sink).fail ? 1 : 0;
        }
    }
    return 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Args a = parseArgs();
    if (a.bad) {
        if (!a.quiet) {
            MessageBoxW(nullptr,
                        L"EmulkonSetup.exe [/maintain | /modify | /repair | /check | /uninstall] [/quiet] [/dir <каталог>] "
                        L"[/autostart | /noautostart] [/network | /nonetwork] [/norun] [/purge] [/log <файл>]",
                        L"Эмулкон: ключи установщика", MB_ICONINFORMATION);
        }
        return 2;
    }
    if (!a.log.empty()) g_log = _wfopen(a.log.c_str(), L"a");
    logLine(L"EmulkonSetup " + version() + L": " + GetCommandLineW());

    // Удаление запущено из каталога установки (как из «Параметров» — прямо или через обслуживание): свой exe
    // удалим последним, если после работы установки уже нет.
    bool mayRemove = a.mode == Mode::Uninstall ||
                     (!a.quiet && (a.mode == Mode::Maintain || a.mode == Mode::Modify || a.mode == Mode::Repair));
    std::wstring uninstallDir = mayRemove ? (a.dirGiven ? a.o.dir : findInstalled().dir) : std::wstring();
    int code = a.quiet ? runQuiet(a) : runUi(inst, a);
    logLine(L"код возврата " + std::to_wstring(code));
    if (g_log) std::fclose(g_log);
    if (!uninstallDir.empty() && isInside(selfPath(), uninstallDir) && !findInstalled().present) {
        scheduleSelfDelete(uninstallDir);
    }
    return code;
}
