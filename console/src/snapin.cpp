// EmulkonSnapIn — оснастка MMC «Эмулкон» (docs/local-install.md §4–§5).
//
// Корень «Эмулкон» — сводка (служба, версия, адрес, устройства, рабочие места). Раздел «Служба» (этап 2) —
// состояние, запуск и остановка, свойства: кассы в сети, токены управления и устройств. «Рабочие места» и
// «Устройства» (этап 3) — списки, свойства, токены рабочих мест, сброс и удаление устройства. «Сценарии» и
// «Журнал» (этап 4) — списки, включение и удаление правил, выгрузка и загрузка, отбор и выгрузка журнала.
// Без ATL: COM-классы вручную; окна — dialogs.cpp.
#include <windows.h>
#include <commctrl.h>
#include <mmc.h>
#include <olectl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <new>
#include <map>
#include <string>
#include <vector>

#include "dialogs.h"
#include "inventory.h"
#include "records.h"
#include "summary.h"

namespace {

// {1C4354F7-8B1F-4FF0-912D-50493CBE677C} — оснастка; её же пишет установщик в Emulkon.msc.
const CLSID CLSID_Snapin = {0x1C4354F7, 0x8B1F, 0x4FF0, {0x91, 0x2D, 0x50, 0x49, 0x3C, 0xBE, 0x67, 0x7C}};
// {BD3F6610-A1AB-4AB0-9B2F-9A76B5D41F10} — «О программе» (ISnapinAbout).
const CLSID CLSID_About = {0xBD3F6610, 0xA1AB, 0x4AB0, {0x9B, 0x2F, 0x9A, 0x76, 0xB5, 0xD4, 0x1F, 0x10}};
// Типы узлов: корень и раздел.
const GUID kNodeRoot = {0x23A02B6E, 0xB8F3, 0x4241, {0xA0, 0x91, 0xDD, 0xED, 0x8D, 0x8D, 0x19, 0xDF}};
const GUID kNodeSection = {0x3F874E3C, 0x360C, 0x4E69, {0x97, 0x71, 0xD6, 0x90, 0x8E, 0x30, 0x4B, 0x42}};

const wchar_t* const kSnapinName = L"Эмулкон";
const wchar_t* const kProvider = L"imiron.ru";

HINSTANCE g_module = nullptr;

// TRACE — журнал вызовов MMC в %TEMP%\emulkon-snapin.log; включается сборкой с EMULKON_SNAPIN_TRACE.
#ifdef EMULKON_SNAPIN_TRACE
void trace(const char* fmt, ...) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, "emulkon-snapin.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
#define TRACE(...) trace(__VA_ARGS__)
void traceIid(const char* where, REFIID riid) {
    wchar_t g[64];
    StringFromGUID2(riid, g, 64);
    trace("%s %ls", where, g);
}
#else
#define TRACE(...) ((void)0)
#define traceIid(where, riid) ((void)0)
#endif
std::atomic<long> g_objects{0}, g_locks{0};

// Картинки: в списках узлов и результатов одинаковые номера.
enum Image { kImgRoot, kImgService, kImgWorkstations, kImgDevices, kImgScenarios, kImgJournal, kImgInfo, kImgCount };

// Разделы — дочерние узлы корня. cookie раздела — номер + 1, у корня 0.
struct Section {
    const wchar_t* name;
    Image image;
};
const Section kSections[] = {
    {L"Служба", kImgService},     {L"Рабочие места", kImgWorkstations}, {L"Устройства", kImgDevices},
    {L"Сценарии", kImgScenarios}, {L"Журнал", kImgJournal},
};
const int kSectionCount = sizeof(kSections) / sizeof(kSections[0]);
const MMC_COOKIE kCookieService = 1;       // раздел «Служба»
const MMC_COOKIE kCookieWorkstations = 2;  // «Рабочие места»
const MMC_COOKIE kCookieDevices = 3;       // «Устройства»
const MMC_COOKIE kCookieScenarios = 4;     // «Сценарии»
const MMC_COOKIE kCookieJournal = 5;       // «Журнал»

// hasList — у узла список: у корня и всех разделов.
bool hasList(MMC_COOKIE cookie) { return cookie >= 0 && cookie <= kCookieJournal; }
// hasObjects — строки списка — объекты сервера: у них меню и свойства.
bool hasObjects(MMC_COOKIE cookie) { return cookie >= kCookieWorkstations && cookie <= kCookieJournal; }

// webView — вкладка веб-интерфейса раздела.
const wchar_t* webView(MMC_COOKIE cookie) {
    switch (cookie) {
        case kCookieWorkstations: return L"sessions";
        case kCookieDevices: return L"devices";
        case kCookieScenarios: return L"scenarios";
        case kCookieJournal: return L"journal";
        default: return L"";
    }
}

// openWeb — веб-интерфейс на вкладке view (devices, sessions, …) через проводник: браузер не наследует
// права администратора консоли.
void openWeb(const std::wstring& view = {}) {
    const std::wstring url = console::serverBase(console::readServerSettings()) + L"/" +
                             (view.empty() ? L"" : L"#" + view);
    ShellExecuteW(nullptr, nullptr, L"explorer.exe", url.c_str(), nullptr, SW_SHOWNORMAL);
}

const wchar_t* nodeName(MMC_COOKIE cookie) {
    if (cookie >= 1 && cookie <= kSectionCount) return kSections[cookie - 1].name;
    return kSnapinName;
}

int sectionImage(LPARAM cookie) { return cookie >= 1 && cookie <= kSectionCount ? kSections[cookie - 1].image : kImgRoot; }

wchar_t* coString(const std::wstring& s) {
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    auto* p = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
    if (p) memcpy(p, s.c_str(), bytes);
    return p;
}

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// --- картинки ---

const COLORREF kMask = RGB(255, 0, 255);

HICON stockIcon(SHSTOCKICONID id, bool large) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (FAILED(SHGetStockIconInfo(id, SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON), &sii))) return nullptr;
    return sii.hIcon;
}

HICON imageIcon(Image img, int size) {
    bool large = size > 16;
    switch (img) {
        case kImgRoot: return (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0);
        case kImgService: return stockIcon(SIID_SERVER, large);
        case kImgWorkstations: return stockIcon(SIID_DESKTOPPC, large);
        case kImgDevices: return stockIcon(SIID_PRINTER, large);
        case kImgScenarios: return stockIcon(SIID_SOFTWARE, large);
        case kImgJournal: return stockIcon(SIID_DOCASSOC, large);
        default: return stockIcon(SIID_INFO, large);
    }
}

// drawMasked рисует значок с полупрозрачными краями в полосу с цветом-маской kMask: альфа
// восстанавливается по двум подложкам (чёрной и белой), пиксели с альфой меньше половины — маска.
// Иначе на магенте вокруг сглаженных краёв остаётся розовая кайма.
void drawMasked(HDC dc, HICON icon, int x, int size) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bitsBlack = nullptr;
    void* bitsWhite = nullptr;
    HBITMAP black = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsBlack, nullptr, 0);
    HBITMAP white = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsWhite, nullptr, 0);
    if (!black || !white) {
        if (black) DeleteObject(black);
        if (white) DeleteObject(white);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, black);
    RECT r{0, 0, size, size};
    FillRect(mem, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(mem, white);
    FillRect(mem, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);
    GdiFlush();
    auto* pb = static_cast<const BYTE*>(bitsBlack);
    auto* pw = static_cast<const BYTE*>(bitsWhite);
    for (int y = 0; y < size; ++y) {
        for (int i = 0; i < size; ++i) {
            const BYTE* b = pb + (y * size + i) * 4;
            const BYTE* w = pw + (y * size + i) * 4;
            int alpha = 255 - (w[1] - b[1]);  // по зелёному каналу
            COLORREF c = kMask;
            if (alpha >= 128) {
                auto un = [alpha](BYTE v) { return (BYTE)(v * 255 / alpha > 255 ? 255 : v * 255 / alpha); };
                c = RGB(un(b[2]), un(b[1]), un(b[0]));
                if (c == kMask) c = RGB(254, 0, 254);
            }
            SetPixel(dc, x + i, y, c);
        }
    }
    DeleteDC(mem);
    DeleteObject(black);
    DeleteObject(white);
}

// strip — полоса из всех картинок размера size × size (для IImageList::ImageListSetStrip).
HBITMAP strip(int size) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, size * kImgCount, size);
    HGDIOBJ old = SelectObject(dc, bmp);
    for (int i = 0; i < kImgCount; ++i) {
        HICON icon = imageIcon((Image)i, size);
        RECT r{i * size, 0, (i + 1) * size, size};
        HBRUSH brush = CreateSolidBrush(kMask);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        if (icon) {
            drawMasked(dc, icon, i * size, size);
            DestroyIcon(icon);
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bmp;
}

void setImages(IImageList* list) {
    if (!list) return;
    HBITMAP sm = strip(16), lg = strip(32);
    list->ImageListSetStrip(reinterpret_cast<LONG_PTR*>(sm), reinterpret_cast<LONG_PTR*>(lg), 0, kMask);
    DeleteObject(sm);
    DeleteObject(lg);
}

// single — одна картинка: для «О программе» (папка оснастки в окне «Добавление оснасток»).
HBITMAP single(Image img, int size) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, size, size);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r{0, 0, size, size};
    HBRUSH brush = CreateSolidBrush(kMask);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    if (HICON icon = imageIcon(img, size)) {
        drawMasked(dc, icon, 0, size);
        DestroyIcon(icon);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bmp;
}

// --- объект данных: им MMC спрашивает тип узла, имя и чей он ---

UINT cfNodeType = 0, cfSzNodeType = 0, cfDisplayName = 0, cfSnapinClsid = 0, cfColumnSet = 0, cfCookie = 0;

void registerFormats() {
    if (cfCookie) return;
    cfColumnSet = RegisterClipboardFormatW(CCF_COLUMN_SET_ID);
    cfNodeType = RegisterClipboardFormatW(CCF_NODETYPE);
    cfSzNodeType = RegisterClipboardFormatW(CCF_SZNODETYPE);
    cfDisplayName = RegisterClipboardFormatW(CCF_DISPLAY_NAME);
    cfSnapinClsid = RegisterClipboardFormatW(CCF_SNAPIN_CLASSID);
    cfCookie = RegisterClipboardFormatW(L"Emulkon.SnapIn.Cookie");
}

struct CookieData {
    MMC_COOKIE cookie;
    DATA_OBJECT_TYPES type;
};

class DataObject final : public IDataObject {
public:
    DataObject(MMC_COOKIE cookie, DATA_OBJECT_TYPES type, std::wstring name = {}) : data_{cookie, type}, name_(std::move(name)) {
        // MMC спрашивает имя и тип узла ещё до Initialize (окно «Добавление оснасток»).
        registerFormats();
        ++g_objects;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* m) override {
        if (!f || !m) return E_POINTER;
#ifdef EMULKON_SNAPIN_TRACE
        wchar_t cfName[128] = L"";
        GetClipboardFormatNameW(f->cfFormat, cfName, 128);
        TRACE("DataObject::GetDataHere %ls tymed=%lu hGlobal=%p size=%zu cookie=%lld type=%d", cfName, m->tymed, m->hGlobal,
              m->hGlobal ? GlobalSize(m->hGlobal) : 0, (long long)data_.cookie, (int)data_.type);
#endif
        if (m->tymed != TYMED_HGLOBAL || !m->hGlobal) return DV_E_TYMED;
        const bool section = data_.type != CCT_RESULT && data_.cookie != 0;
        const GUID& node = data_.type == CCT_RESULT || section ? kNodeSection : kNodeRoot;
        if (f->cfFormat == cfNodeType) return write(m, &node, sizeof(node));
        if (f->cfFormat == cfSzNodeType) {
            wchar_t s[64];
            int n = StringFromGUID2(node, s, 64);
            return write(m, s, n * sizeof(wchar_t));
        }
        if (f->cfFormat == cfDisplayName) {
            const std::wstring name = !name_.empty() ? name_ : nodeName(data_.type == CCT_RESULT ? 0 : data_.cookie);
            return write(m, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        }
        if (f->cfFormat == cfSnapinClsid) return write(m, &CLSID_Snapin, sizeof(CLSID));
        if (f->cfFormat == cfCookie) return write(m, &data_, sizeof(data_));
        if (f->cfFormat == cfColumnSet && data_.type != CCT_RESULT) {
            // Свой набор колонок у каждого узла: у разделов один тип узла, а колонки разные — MMC помнит
            // ширину и порядок по этому ключу.
            char id[32];
            int n = snprintf(id, sizeof id, "emulkon.columns.%lld", (long long)data_.cookie);
            std::vector<BYTE> buf(offsetof(SColumnSetID, id) + n);
            auto* set = reinterpret_cast<SColumnSetID*>(buf.data());
            set->dwFlags = 0;
            set->cBytes = (DWORD)n;
            memcpy(set->id, id, n);
            return write(m, buf.data(), buf.size());
        }
        return DV_E_CLIPFORMAT;
    }
    STDMETHODIMP GetData(FORMATETC* f, STGMEDIUM*) override {
        TRACE("DataObject::GetData cf=%u", f ? f->cfFormat : 0);
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryGetData(FORMATETC* f) override {
        TRACE("DataObject::QueryGetData cf=%u", f ? f->cfFormat : 0);
        return E_NOTIMPL;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~DataObject() { --g_objects; }

    static HRESULT write(STGMEDIUM* m, const void* p, size_t n) {
        IStream* stream = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(m->hGlobal, FALSE, &stream);
        if (FAILED(hr)) return hr;
        ULONG written = 0;
        hr = stream->Write(p, (ULONG)n, &written);
        stream->Release();
        return hr;
    }

    std::atomic<ULONG> refs_{1};
    CookieData data_;
    std::wstring name_;  // имя строки списка: заголовок окна свойств
};

// cookieOf — наш ли объект данных и какой узел за ним. Особые объекты MMC (мультивыбор и т. п.) — нет.
bool cookieOf(IDataObject* obj, CookieData& out) {
    if (!obj || IS_SPECIAL_DATAOBJECT(obj)) return false;
    HGLOBAL h = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(CookieData));
    if (!h) return false;
    FORMATETC f{(CLIPFORMAT)cfCookie, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{TYMED_HGLOBAL};
    m.hGlobal = h;
    bool ok = SUCCEEDED(obj->GetDataHere(&f, &m));
    if (ok) {
        auto* p = static_cast<CookieData*>(GlobalLock(h));
        if (p) out = *p;
        GlobalUnlock(h);
        ok = p != nullptr;
    }
    GlobalFree(h);
    return ok;
}

HRESULT newDataObject(MMC_COOKIE cookie, DATA_OBJECT_TYPES type, IDataObject** pp, const std::wstring& name = {}) {
    if (!pp) return E_POINTER;
    *pp = new (std::nothrow) DataObject(cookie, type, name);
    return *pp ? S_OK : E_OUTOFMEMORY;
}

// --- IComponentData: дерево узлов ---

const LPARAM kHintRefresh = 1;

class Component;

// Команды меню «Действие» (они же — панель действий).
enum Command : long {
    kCmdWeb = 1,
    kCmdConsoleToken,
    kCmdStart,
    kCmdStop,
    kCmdRestart,
    kCmdIssueControl,
    kCmdRemoveControl,
    kCmdIssueDevice,
    kCmdRemoveDevice,
    kCmdNewWorkstation,
    kCmdExportScenarios,
    kCmdImportScenarios,
    kCmdJournalFilter,
    kCmdJournalNoFilter,
    kCmdExportJournal,
};

class ComponentData final : public IComponentData, public IExtendContextMenu, public IExtendPropertySheet2 {
public:
    ComponentData() { ++g_objects; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        traceIid(__FUNCTION__, riid);
        if (riid == IID_IUnknown || riid == IID_IComponentData) {
            *ppv = static_cast<IComponentData*>(this);
        } else if (riid == IID_IExtendContextMenu) {
            *ppv = static_cast<IExtendContextMenu*>(this);
        } else if (riid == IID_IExtendPropertySheet || riid == IID_IExtendPropertySheet2) {
            *ppv = static_cast<IExtendPropertySheet2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // --- IExtendContextMenu ---

    STDMETHODIMP AddMenuItems(LPDATAOBJECT obj, LPCONTEXTMENUCALLBACK menu, long* allowed) override {
        CookieData d{};
        if (!menu || !allowed || !(*allowed & CCM_INSERTIONALLOWED_TOP) || !cookieOf(obj, d) || d.type == CCT_RESULT)
            return S_OK;
        if (d.cookie == kCookieService) {
            const console::ServiceInfo svc = console::queryServiceInfo();
            const console::ServerSettings s = console::readServerSettings();
            const bool running = svc.state == console::Service::Running;
            add(menu, kCmdStart, L"Запустить", L"Запустить службу Emulkon", svc.state != console::Service::Stopped);
            add(menu, kCmdStop, L"Остановить", L"Остановить службу Emulkon", !running);
            add(menu, kCmdRestart, L"Перезапустить", L"Остановить и снова запустить службу Emulkon", !running);
            separator(menu);
            add(menu, kCmdIssueControl, L"Выдать токен управления…", L"Новый токен Control API: показывается один раз", !s.readable);
            add(menu, kCmdRemoveControl, L"Снять токен управления", L"Управление без токена", !s.readable || !s.controlToken);
            add(menu, kCmdIssueDevice, L"Выдать токен устройств…", L"Новый общий токен Device API: показывается один раз", !s.readable);
            add(menu, kCmdRemoveDevice, L"Снять токен устройств", L"Драйверы без токена рабочего места — без токена",
                !s.readable || !s.deviceToken);
            separator(menu);
        }
        if (d.cookie == kCookieScenarios) {
            add(menu, kCmdExportScenarios, L"Выгрузить сценарии…", L"Все правила в файл JSON", false);
            add(menu, kCmdImportScenarios, L"Загрузить сценарии…", L"Правила из файла: с тем же кодом — заменить, новые — добавить", false);
            separator(menu);
        }
        if (d.cookie == kCookieJournal) {
            const bool filtered = !console::journalFilter().empty();
            add(menu, kCmdJournalFilter, L"Отбор…", L"Показать записи с текстом", false);
            add(menu, kCmdJournalNoFilter, L"Сбросить отбор", L"Показать все записи", !filtered);
            add(menu, kCmdExportJournal, filtered ? L"Выгрузить журнал (с отбором)…" : L"Выгрузить журнал…",
                L"Записи журнала в файл JSON Lines", false);
            separator(menu);
        }
        if (d.cookie == kCookieWorkstations) {
            add(menu, kCmdNewWorkstation, L"Новое рабочее место…", L"Завести рабочее место заранее и выдать токен для драйвера", false);
            separator(menu);
        }
        if (d.cookie == 0 || d.cookie == kCookieService) {
            add(menu, kCmdConsoleToken, L"Токен управления для консоли…", L"Токен Control API для этой консоли (только в памяти)", false);
        }
        if (hasList(d.cookie)) add(menu, kCmdWeb, L"Открыть веб-интерфейс", L"Веб-интерфейс сервера в браузере", false);
        return S_OK;
    }

    STDMETHODIMP Command(long id, LPDATAOBJECT obj) override {
        HWND parent = nullptr;
        if (console_) console_->GetMainWindow(&parent);
        bool changed = false;
        switch (id) {
            case kCmdWeb: {
                CookieData d{};
                cookieOf(obj, d);
                openWeb(webView(d.cookie));
                return S_OK;
            }
            case kCmdNewWorkstation: changed = console::newWorkstation(parent); break;
            case kCmdExportScenarios: changed = console::exportScenariosDialog(parent); break;
            case kCmdImportScenarios: changed = console::importScenariosDialog(parent); break;
            case kCmdJournalFilter: changed = console::askJournalFilter(parent); break;
            case kCmdJournalNoFilter:
                console::setJournalFilter(L"");
                changed = true;
                break;
            case kCmdExportJournal: changed = console::exportJournalDialog(parent); break;
            case kCmdConsoleToken: {
                std::string token;
                if (console::askToken(parent, token)) {
                    console::setConsoleToken(token);
                    changed = true;
                }
                break;
            }
            case kCmdStart: changed = console::serviceAction(parent, console::ServiceAction::Start); break;
            case kCmdStop: changed = console::serviceAction(parent, console::ServiceAction::Stop); break;
            case kCmdRestart: changed = console::serviceAction(parent, console::ServiceAction::Restart); break;
            case kCmdIssueControl: changed = console::issueToken(parent, true); break;
            case kCmdRemoveControl: changed = console::removeToken(parent, true); break;
            case kCmdIssueDevice: changed = console::issueToken(parent, false); break;
            case kCmdRemoveDevice: changed = console::removeToken(parent, false); break;
            default: return S_FALSE;
        }
        if (changed) refreshViews(obj);
        return S_OK;
    }

    // --- IExtendPropertySheet2: свойства службы ---

    STDMETHODIMP CreatePropertyPages(LPPROPERTYSHEETCALLBACK provider, LONG_PTR handle, LPDATAOBJECT obj) override {
        CookieData d{};
        if (!provider || !cookieOf(obj, d) || d.cookie != kCookieService) {
            if (handle) MMCFreeNotifyHandle(handle);
            return S_FALSE;
        }
        HPROPSHEETPAGE page = console::createServicePage(handle);
        if (!page) {
            if (handle) MMCFreeNotifyHandle(handle);
            return E_FAIL;
        }
        return provider->AddPage(page);
    }

    STDMETHODIMP QueryPagesFor(LPDATAOBJECT obj) override {
        CookieData d{};
        return cookieOf(obj, d) && d.type == CCT_SCOPE && d.cookie == kCookieService ? S_OK : S_FALSE;
    }

    STDMETHODIMP GetWatermarks(LPDATAOBJECT, HBITMAP* watermark, HBITMAP* header, HPALETTE* palette, BOOL* stretch) override {
        if (watermark) *watermark = nullptr;
        if (header) *header = nullptr;
        if (palette) *palette = nullptr;
        if (stretch) *stretch = FALSE;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Initialize(LPUNKNOWN unk) override {
        TRACE("ComponentData::Initialize");
        if (!unk) return E_POINTER;
        registerFormats();
        unk->QueryInterface(IID_IConsole2, reinterpret_cast<void**>(&console_));
        unk->QueryInterface(IID_IConsoleNameSpace, reinterpret_cast<void**>(&ns_));
        if (!console_ || !ns_) return E_NOINTERFACE;
        IImageList* list = nullptr;
        if (SUCCEEDED(console_->QueryScopeImageList(&list))) {
            setImages(list);
            list->Release();
        }
        return S_OK;
    }

    STDMETHODIMP CreateComponent(LPCOMPONENT* pp) override;

    STDMETHODIMP Notify(LPDATAOBJECT obj, MMC_NOTIFY_TYPE event, LPARAM arg, LPARAM param) override {
        TRACE("ComponentData::Notify event=0x%x arg=%lld", (unsigned)event, (long long)arg);
        CookieData d{};
        switch (event) {
            case MMCN_EXPAND:
                if (arg && cookieOf(obj, d) && d.cookie == 0) return expandRoot((HSCOPEITEM)param);
                return S_OK;
            case MMCN_REFRESH:
                refreshViews(obj);
                return S_OK;
            case MMCN_PROPERTY_CHANGE:
                // Страница свойств что-то изменила (её поток): обновить списки здесь, в потоке MMC.
                refreshViews(nullptr);
                return S_OK;
            case MMCN_REMOVE_CHILDREN:
                return S_OK;
            default:
                return S_FALSE;
        }
    }

    STDMETHODIMP Destroy() override {
        release(ns_);
        release(console_);
        return S_OK;
    }

    STDMETHODIMP QueryDataObject(MMC_COOKIE cookie, DATA_OBJECT_TYPES type, LPDATAOBJECT* pp) override {
        TRACE("%s cookie=%lld type=%d", __FUNCTION__, (long long)cookie, (int)type);
        return newDataObject(cookie, type, pp);
    }

    STDMETHODIMP GetDisplayInfo(SCOPEDATAITEM* item) override {
        if (!item) return E_POINTER;
        TRACE("ComponentData::GetDisplayInfo mask=0x%x lParam=%lld", item->mask, (long long)item->lParam);
        if (item->mask & SDI_STR) item->displayname = const_cast<LPOLESTR>(nodeName(item->lParam));
        if (item->mask & SDI_IMAGE) item->nImage = sectionImage(item->lParam);
        if (item->mask & SDI_OPENIMAGE) item->nOpenImage = item->nImage;
        return S_OK;
    }

    STDMETHODIMP CompareObjects(LPDATAOBJECT a, LPDATAOBJECT b) override {
        CookieData x{}, y{};
        if (!cookieOf(a, x) || !cookieOf(b, y)) return E_UNEXPECTED;
        return x.cookie == y.cookie && x.type == y.type ? S_OK : S_FALSE;
    }

private:
    ~ComponentData() {
        Destroy();
        --g_objects;
    }

    static void add(LPCONTEXTMENUCALLBACK menu, long id, const wchar_t* name, const wchar_t* status, bool grayed) {
        CONTEXTMENUITEM item{};
        item.strName = const_cast<LPWSTR>(name);
        item.strStatusBarText = const_cast<LPWSTR>(status);
        item.lCommandID = id;
        item.lInsertionPointID = CCM_INSERTIONPOINTID_PRIMARY_TOP;
        item.fFlags = grayed ? MF_GRAYED : 0;
        menu->AddItem(&item);
    }

    static void separator(LPCONTEXTMENUCALLBACK menu) {
        CONTEXTMENUITEM item{};
        item.lInsertionPointID = CCM_INSERTIONPOINTID_PRIMARY_TOP;
        item.fFlags = MF_SEPARATOR;
        menu->AddItem(&item);
    }

    // refreshViews — перечитать списки во всех окнах (MMCN_VIEW_CHANGE).
    void refreshViews(LPDATAOBJECT obj) {
        if (!console_) return;
        IDataObject* own = nullptr;
        if (!obj && SUCCEEDED(newDataObject(0, CCT_SCOPE, &own))) obj = own;
        console_->UpdateAllViews(obj, 0, kHintRefresh);
        release(own);
    }

    HRESULT expandRoot(HSCOPEITEM root) {
        // Значок корня: до раскрытия MMC берёт его из «О программе», дальше — из списка картинок.
        SCOPEDATAITEM self{};
        self.mask = SDI_IMAGE | SDI_OPENIMAGE;
        self.ID = root;
        self.nImage = self.nOpenImage = kImgRoot;
        ns_->SetItem(&self);
        if (expanded_) return S_OK;
        expanded_ = true;
        for (int i = 0; i < kSectionCount; ++i) {
            SCOPEDATAITEM item{};
            item.mask = SDI_STR | SDI_PARAM | SDI_IMAGE | SDI_OPENIMAGE | SDI_PARENT | SDI_CHILDREN;
            item.displayname = MMC_CALLBACK;
            item.nImage = item.nOpenImage = kSections[i].image;
            item.cChildren = 0;
            item.relativeID = root;
            item.lParam = i + 1;
            ns_->InsertItem(&item);
        }
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    IConsole2* console_ = nullptr;
    IConsoleNameSpace* ns_ = nullptr;
    bool expanded_ = false;
};

// --- IComponent: правая панель ---

// Команды строк списка (рабочее место, устройство, правило).
enum ItemCommand : long {
    kItemIssueToken = 100,
    kItemRevokeToken,
    kItemResetDevice,
    kItemDeleteDevice,
    kItemToggleScenario,
    kItemDeleteScenario,
    kItemWeb,
};

class Component final : public IComponent, public IExtendContextMenu, public IExtendPropertySheet2 {
public:
    explicit Component(ComponentData* owner) : owner_(owner) {
        owner_->AddRef();
        ++g_objects;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        traceIid(__FUNCTION__, riid);
        if (riid == IID_IUnknown || riid == IID_IComponent) {
            *ppv = static_cast<IComponent*>(this);
        } else if (riid == IID_IExtendContextMenu) {
            *ppv = static_cast<IExtendContextMenu*>(this);
        } else if (riid == IID_IExtendPropertySheet || riid == IID_IExtendPropertySheet2) {
            *ppv = static_cast<IExtendPropertySheet2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Initialize(LPCONSOLE con) override {
        if (!con) return E_POINTER;
        con->QueryInterface(IID_IConsole2, reinterpret_cast<void**>(&console_));
        con->QueryInterface(IID_IResultData, reinterpret_cast<void**>(&result_));
        con->QueryInterface(IID_IHeaderCtrl, reinterpret_cast<void**>(&header_));
        if (!console_ || !result_ || !header_) return E_NOINTERFACE;
        console_->QueryConsoleVerb(&verb_);
        return S_OK;
    }

    STDMETHODIMP Notify(LPDATAOBJECT obj, MMC_NOTIFY_TYPE event, LPARAM arg, LPARAM param) override {
        TRACE("Component::Notify event=0x%x arg=%lld", (unsigned)event, (long long)arg);
        CookieData d{};
        switch (event) {
            case MMCN_ADD_IMAGES:
                setImages(reinterpret_cast<IImageList*>(arg));
                return S_OK;
            case MMCN_SHOW:
                if (!cookieOf(obj, d)) return S_OK;
                if (!arg) {
                    shown_ = -1;
                    return S_OK;
                }
                shown_ = d.cookie;
                return showList();
            case MMCN_SELECT:
                if (verb_ && HIWORD(arg) && cookieOf(obj, d)) {
                    // Обновить — у узлов со списком и строк списка; «Свойства» — у службы, рабочих мест и устройств.
                    const bool list = d.type == CCT_RESULT || hasList(d.cookie);
                    const bool item = d.type == CCT_RESULT && hasObjects(shown_) && !key(d.cookie).empty();
                    verb_->SetVerbState(MMC_VERB_REFRESH, ENABLED, list);
                    verb_->SetVerbState(MMC_VERB_PROPERTIES, ENABLED, item || (d.type == CCT_SCOPE && d.cookie == kCookieService));
                    if (item) verb_->SetDefaultVerb(MMC_VERB_PROPERTIES);  // двойной щелчок — свойства
                }
                return S_OK;
            case MMCN_REFRESH:
            case MMCN_PROPERTY_CHANGE:
                if (hasList(shown_)) return showList();
                return S_OK;
            case MMCN_VIEW_CHANGE:
                if (param == kHintRefresh && hasList(shown_)) return showList();
                return S_OK;
            case MMCN_DBLCLICK:
                return S_FALSE;  // действие по умолчанию: раскрыть узел или свойства строки
            default:
                return S_FALSE;
        }
    }

    STDMETHODIMP Destroy(MMC_COOKIE) override {
        release(verb_);
        release(header_);
        release(result_);
        release(console_);
        return S_OK;
    }

    STDMETHODIMP QueryDataObject(MMC_COOKIE cookie, DATA_OBJECT_TYPES type, LPDATAOBJECT* pp) override {
        TRACE("%s cookie=%lld type=%d", __FUNCTION__, (long long)cookie, (int)type);
        std::wstring name;
        if (type == CCT_RESULT && hasObjects(shown_) && !key(cookie).empty()) name = table_.cells[cookie][0];
        return newDataObject(cookie, type, pp, name);
    }

    STDMETHODIMP GetResultViewType(MMC_COOKIE cookie, LPOLESTR* viewType, long* options) override {
        if (!viewType || !options) return E_POINTER;
        // У всех узлов — список; дочерние разделы в нём не повторяем.
        (void)cookie;
        *viewType = nullptr;
        *options = MMC_VIEW_OPTIONS_EXCLUDE_SCOPE_ITEMS_FROM_LIST;
        return S_FALSE;
    }

    STDMETHODIMP GetDisplayInfo(RESULTDATAITEM* item) override {
        if (!item) return E_POINTER;
        if (item->bScopeItem) {
            if (item->mask & RDI_STR) item->str = item->nCol == 0 ? const_cast<LPOLESTR>(nodeName(item->lParam)) : const_cast<LPOLESTR>(L"");
            if (item->mask & RDI_IMAGE) item->nImage = sectionImage(item->lParam);
            return S_OK;
        }
        if (item->mask & RDI_STR) {
            const wchar_t* s = L"";
            if (!table_.problem.empty()) {
                if (item->nCol == 0) s = table_.problem.c_str();
            } else if (item->lParam >= 0 && item->lParam < (LPARAM)table_.cells.size()) {
                const auto& row = table_.cells[item->lParam];
                if (item->nCol >= 0 && item->nCol < (int)row.size()) s = row[item->nCol].c_str();
            }
            item->str = const_cast<LPOLESTR>(s);
        }
        if (item->mask & RDI_IMAGE) item->nImage = rowImage();
        return S_OK;
    }

    STDMETHODIMP CompareObjects(LPDATAOBJECT a, LPDATAOBJECT b) override {
        CookieData x{}, y{};
        if (!cookieOf(a, x) || !cookieOf(b, y)) return E_UNEXPECTED;
        return x.cookie == y.cookie && x.type == y.type ? S_OK : S_FALSE;
    }

    // --- IExtendContextMenu: строки рабочих мест и устройств ---

    STDMETHODIMP AddMenuItems(LPDATAOBJECT obj, LPCONTEXTMENUCALLBACK menu, long* allowed) override {
        CookieData d{};
        if (!menu || !allowed || !(*allowed & CCM_INSERTIONALLOWED_TOP) || !cookieOf(obj, d) || d.type != CCT_RESULT)
            return S_OK;
        if (shown_ == kCookieWorkstations) {
            const console::Workstation* w = workstation(d.cookie);
            if (!w) return S_OK;
            addItem(menu, kItemIssueToken, L"Выдать токен…", L"Новый токен рабочего места: показывается один раз", false);
            addItem(menu, kItemRevokeToken, L"Отозвать токен", L"Драйвер с этим токеном больше не подключится", !w->token);
            addItem(menu, kItemWeb, L"Открыть в веб-интерфейсе", L"Рабочие места в веб-интерфейсе", false);
        } else if (shown_ == kCookieDevices) {
            if (!device(d.cookie)) return S_OK;
            addItem(menu, kItemResetDevice, L"Сбросить настройки…", L"Настройки класса по умолчанию", false);
            addItem(menu, kItemDeleteDevice, L"Удалить…", L"Удалить устройство: драйвер заведёт его заново", false);
            addItem(menu, kItemWeb, L"Открыть в веб-интерфейсе", L"Устройства в веб-интерфейсе", false);
        } else if (shown_ == kCookieScenarios) {
            const console::Scenario* s = scenario(d.cookie);
            if (!s) return S_OK;
            addItem(menu, kItemToggleScenario, s->enabled ? L"Выключить" : L"Включить",
                    s->enabled ? L"Правило перестанет срабатывать" : L"Правило начнёт срабатывать", false);
            addItem(menu, kItemDeleteScenario, L"Удалить…", L"Удалить правило", false);
            addItem(menu, kItemWeb, L"Открыть в веб-интерфейсе", L"Сценарии в веб-интерфейсе: правка правил", false);
        } else if (shown_ == kCookieJournal) {
            addItem(menu, kItemWeb, L"Открыть в веб-интерфейсе", L"Журнал в веб-интерфейсе: поток вживую", false);
        }
        return S_OK;
    }

    STDMETHODIMP Command(long id, LPDATAOBJECT obj) override {
        CookieData d{};
        if (!cookieOf(obj, d) || d.type != CCT_RESULT) return S_FALSE;
        HWND parent = nullptr;
        console_->GetMainWindow(&parent);
        bool changed = false;
        if (id == kItemWeb) {
            openWeb(webView(shown_));
            return S_OK;
        }
        if (const console::Workstation* w = shown_ == kCookieWorkstations ? workstation(d.cookie) : nullptr) {
            const std::string wid = w->id;  // список перечитается — копия
            if (id == kItemIssueToken) changed = console::issueWorkstationToken(parent, wid);
            if (id == kItemRevokeToken) changed = console::revokeWorkstationToken(parent, wid);
        }
        if (const console::Device* dev = shown_ == kCookieDevices ? device(d.cookie) : nullptr) {
            const console::Device copy = *dev;
            if (id == kItemResetDevice) changed = console::resetDevice(parent, copy);
            if (id == kItemDeleteDevice) changed = console::deleteDevice(parent, copy);
        }
        if (const console::Scenario* s = shown_ == kCookieScenarios ? scenario(d.cookie) : nullptr) {
            const console::Scenario copy = *s;
            if (id == kItemToggleScenario) changed = console::toggleScenario(parent, copy);
            if (id == kItemDeleteScenario) changed = console::deleteScenario(parent, copy);
        }
        if (changed) showList();
        return S_OK;
    }

    // --- IExtendPropertySheet2: свойства строки ---

    STDMETHODIMP CreatePropertyPages(LPPROPERTYSHEETCALLBACK provider, LONG_PTR handle, LPDATAOBJECT obj) override {
        CookieData d{};
        HPROPSHEETPAGE pages[2] = {};
        bool textOnly = false;
        if (provider && cookieOf(obj, d) && d.type == CCT_RESULT) {
            if (const console::Workstation* w = shown_ == kCookieWorkstations ? workstation(d.cookie) : nullptr) {
                pages[0] = console::createWorkstationPage(handle, *w);
            } else if (const console::Device* dev = shown_ == kCookieDevices ? device(d.cookie) : nullptr) {
                auto it = titles_.find(dev->type);
                pages[0] = console::createDevicePage(handle, *dev, it != titles_.end() ? it->second : dev->equipmentType);
                if (pages[0]) pages[1] = console::createDeviceStatePage(*dev);
            } else if (const console::Scenario* s = shown_ == kCookieScenarios ? scenario(d.cookie) : nullptr) {
                pages[0] = console::createTextPage(L"Правило", (s->enabled ? L"Включено. " : L"Выключено. ") + s->condition + L" → " +
                                                                   s->action + L". Править — в веб-интерфейсе.",
                                                   console::prettyJson(s->raw));
                textOnly = true;
            } else if (const console::JournalEntry* j = shown_ == kCookieJournal ? journalEntry(d.cookie) : nullptr) {
                pages[0] = console::createTextPage(L"Запись", L"№ " + std::to_wstring(j->seq) + L", " + j->time + L", " + j->direction +
                                                                  (j->method.empty() ? L"" : L", " + j->method),
                                                   console::prettyJson(j->raw));
                textOnly = true;
            }
        }
        // Дескриптор уведомлений нужен только страницам с правкой.
        if ((!pages[0] || textOnly) && handle) MMCFreeNotifyHandle(handle);
        if (!pages[0]) return S_FALSE;
        for (HPROPSHEETPAGE p : pages)
            if (p) provider->AddPage(p);
        return S_OK;
    }

    STDMETHODIMP QueryPagesFor(LPDATAOBJECT obj) override {
        CookieData d{};
        return cookieOf(obj, d) && d.type == CCT_RESULT && hasObjects(shown_) && !key(d.cookie).empty() ? S_OK : S_FALSE;
    }

    STDMETHODIMP GetWatermarks(LPDATAOBJECT, HBITMAP* watermark, HBITMAP* header, HPALETTE* palette, BOOL* stretch) override {
        if (watermark) *watermark = nullptr;
        if (header) *header = nullptr;
        if (palette) *palette = nullptr;
        if (stretch) *stretch = FALSE;
        return S_OK;
    }

private:
    ~Component() {
        Destroy(0);
        owner_->Release();
        --g_objects;
    }

    static void addItem(LPCONTEXTMENUCALLBACK menu, long id, const wchar_t* name, const wchar_t* status, bool grayed) {
        CONTEXTMENUITEM item{};
        item.strName = const_cast<LPWSTR>(name);
        item.strStatusBarText = const_cast<LPWSTR>(status);
        item.lCommandID = id;
        item.lInsertionPointID = CCM_INSERTIONPOINTID_PRIMARY_TOP;
        item.fFlags = grayed ? MF_GRAYED : 0;
        menu->AddItem(&item);
    }

    std::string key(MMC_COOKIE row) const {
        return row >= 0 && row < (MMC_COOKIE)table_.keys.size() ? table_.keys[row] : std::string();
    }

    const console::Workstation* workstation(MMC_COOKIE row) const {
        const std::string k = key(row);
        for (const auto& w : workstations_)
            if (!k.empty() && w.id == k) return &w;
        return nullptr;
    }

    const console::Device* device(MMC_COOKIE row) const {
        const std::string k = key(row);
        for (const auto& d : devices_)
            if (!k.empty() && d.id == k) return &d;
        return nullptr;
    }

    const console::Scenario* scenario(MMC_COOKIE row) const {
        const std::string k = key(row);
        for (const auto& s : scenarios_)
            if (!k.empty() && s.id == k) return &s;
        return nullptr;
    }

    const console::JournalEntry* journalEntry(MMC_COOKIE row) const {
        const std::string k = key(row);
        for (const auto& j : journal_)
            if (!k.empty() && std::to_string(j.seq) == k) return &j;
        return nullptr;
    }

    int rowImage() const {
        if (!table_.problem.empty()) return kImgInfo;
        if (shown_ == kCookieWorkstations) return kImgWorkstations;
        if (shown_ == kCookieDevices) return kImgDevices;
        if (shown_ == kCookieScenarios) return kImgScenarios;
        if (shown_ == kCookieJournal) return kImgJournal;
        return kImgInfo;
    }

    // load — данные узла с сервера в table_ (и копии объектов для меню и свойств).
    void load() {
        workstations_.clear();
        devices_.clear();
        scenarios_.clear();
        journal_.clear();
        status_.clear();
        if (shown_ == kCookieService) {
            table_ = console::rowsTable(console::serviceRows(console::queryServiceInfo(), console::readServerSettings(),
                                                             !console::consoleToken().empty(), console::machineDataDir()));
        } else if (shown_ == kCookieWorkstations) {
            table_ = console::workstationsTable({});
            console::Reply r = console::apiRequest("GET", "/control/v1/workstations");
            std::wstring err;
            if (r.status != 200) {
                table_.problem = console::replyError(r);
            } else if (!console::parseWorkstations(r.body, workstations_, err)) {
                table_.problem = err;
            } else {
                table_ = console::workstationsTable(workstations_);
                if (workstations_.empty()) table_.problem = L"Рабочих мест пока нет: они появляются при первом подключении драйвера";
            }
        } else if (shown_ == kCookieDevices) {
            table_ = console::devicesTable({}, {});
            console::Reply r = console::apiRequest("GET", "/control/v1/devices");
            std::wstring err;
            if (r.status != 200) {
                table_.problem = console::replyError(r);
            } else if (!console::parseDevices(r.body, devices_, err)) {
                table_.problem = err;
            } else {
                titles_ = console::typeTitles(console::apiRequest("GET", "/control/v1/info").body);
                table_ = console::devicesTable(devices_, titles_);
                if (devices_.empty()) table_.problem = L"Устройств пока нет: их заводит драйвер при подключении";
            }
        } else if (shown_ == kCookieScenarios) {
            table_ = console::scenariosTable({});
            console::Reply r = console::apiRequest("GET", "/control/v1/scenarios");
            std::wstring err;
            if (r.status != 200) {
                table_.problem = console::replyError(r);
            } else if (!console::parseScenarios(r.body, scenarios_, err)) {
                table_.problem = err;
            } else {
                table_ = console::scenariosTable(scenarios_);
                if (scenarios_.empty()) table_.problem = L"Правил нет: их создают в веб-интерфейсе или загружают из файла";
            }
        } else if (shown_ == kCookieJournal) {
            table_ = console::journalTable({});
            const std::wstring filter = console::journalFilter();
            console::Reply r = console::apiRequest("GET", console::journalPath(filter));
            std::wstring err;
            bool more = false;
            if (r.status != 200) {
                table_.problem = console::replyError(r);
            } else if (!console::parseJournal(r.body, journal_, more, err)) {
                table_.problem = err;
            } else {
                table_ = console::journalTable(journal_);
                if (journal_.empty()) table_.problem = filter.empty() ? L"Журнал пуст" : L"Нет записей с «" + filter + L"»";
                status_ = (more ? L"Последние " + std::to_wstring(journal_.size()) + L" записей, есть более ранние — «Выгрузить журнал…»"
                                : L"Записей: " + std::to_wstring(journal_.size())) +
                          (filter.empty() ? L"" : L"; отбор: «" + filter + L"»");
            }
        } else {
            table_ = console::rowsTable(console::summaryRows(console::collect()));
        }
    }

    // showList — список узла. Колонки — при каждом показе: MMC сбрасывает их при смене узла.
    HRESULT showList() {
        result_->DeleteAllRsltItems();
        load();
        for (size_t i = 0; i < table_.columns.size(); ++i)
            header_->InsertColumn((int)i, table_.columns[i].name.c_str(), LVCFMT_LEFT, table_.columns[i].width);
        console_->SetStatusText(const_cast<LPOLESTR>(status_.c_str()));
        const size_t n = table_.problem.empty() ? table_.cells.size() : 1;
        for (size_t i = 0; i < n; ++i) {
            RESULTDATAITEM item{};
            item.mask = RDI_STR | RDI_IMAGE | RDI_PARAM;
            item.str = MMC_CALLBACK;
            item.nImage = rowImage();
            item.lParam = (LPARAM)i;
            result_->InsertItem(&item);
        }
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    ComponentData* owner_;
    IConsole2* console_ = nullptr;
    IResultData* result_ = nullptr;
    IHeaderCtrl* header_ = nullptr;
    IConsoleVerb* verb_ = nullptr;
    console::Table table_;
    std::vector<console::Workstation> workstations_;
    std::vector<console::Device> devices_;
    std::map<std::string, std::wstring> titles_;
    std::vector<console::Scenario> scenarios_;
    std::vector<console::JournalEntry> journal_;
    std::wstring status_;  // строка состояния MMC: сколько записей журнала и отбор
    MMC_COOKIE shown_ = -1;
};

STDMETHODIMP ComponentData::CreateComponent(LPCOMPONENT* pp) {
    if (!pp) return E_POINTER;
    *pp = new (std::nothrow) Component(this);
    return *pp ? S_OK : E_OUTOFMEMORY;
}

// --- «О программе» ---

class About final : public ISnapinAbout {
public:
    About() { ++g_objects; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        traceIid(__FUNCTION__, riid);
        if (riid == IID_IUnknown || riid == IID_ISnapinAbout) {
            *ppv = static_cast<ISnapinAbout*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP GetSnapinDescription(LPOLESTR* p) override {
        return set(p, L"ЭМУЛЯТОР подключаемого оборудования для 1С:БПО: служба сервера эмуляции, рабочие места, "
                      L"устройства, сценарии сбоев и журнал обменов.");
    }
    STDMETHODIMP GetProvider(LPOLESTR* p) override { return set(p, kProvider); }
    STDMETHODIMP GetSnapinVersion(LPOLESTR* p) override { return set(p, console::widen(EMULKON_VERSION)); }
    STDMETHODIMP GetSnapinImage(HICON* icon) override {
        if (!icon) return E_POINTER;
        TRACE("About::GetSnapinImage");
        // Значок и картинки живут, пока загружена DLL: MMC пользуется ими и после освобождения About.
        static HICON big = (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
        *icon = big;
        return big ? S_OK : E_FAIL;
    }
    STDMETHODIMP GetStaticFolderImage(HBITMAP* sm, HBITMAP* smOpen, HBITMAP* lg, COLORREF* mask) override {
        if (!sm || !smOpen || !lg || !mask) return E_POINTER;
        TRACE("About::GetStaticFolderImage");
        static HBITMAP small16 = single(kImgRoot, 16), large32 = single(kImgRoot, 32);
        *sm = *smOpen = small16;
        *lg = large32;
        *mask = kMask;
        return S_OK;
    }

private:
    ~About() { --g_objects; }

    static HRESULT set(LPOLESTR* p, const std::wstring& s) {
        if (!p) return E_POINTER;
        *p = coString(s);
        return *p ? S_OK : E_OUTOFMEMORY;
    }

    std::atomic<ULONG> refs_{1};
};

// --- фабрика классов ---

template <class T>
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Фабрики статические: счётчик ссылок не нужен.
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        T* obj = new (std::nothrow) T();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            ++g_locks;
        else
            --g_locks;
        return S_OK;
    }
};

Factory<ComponentData> g_snapinFactory;
Factory<About> g_aboutFactory;

// --- регистрация (DllRegisterServer вызывает установщик) ---

std::wstring guidText(REFGUID g) {
    wchar_t s[64];
    StringFromGUID2(g, s, 64);
    return s;
}

bool setValue(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                             DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

const std::wstring kClasses = L"SOFTWARE\\Classes\\CLSID\\";
const std::wstring kSnapIns = L"SOFTWARE\\Microsoft\\MMC\\SnapIns\\";
const std::wstring kNodeTypes = L"SOFTWARE\\Microsoft\\MMC\\NodeTypes\\";

bool registerClass(REFCLSID clsid, const wchar_t* name, const std::wstring& dll) {
    const std::wstring key = kClasses + guidText(clsid);
    return setValue(key, nullptr, name) && setValue(key + L"\\InprocServer32", nullptr, dll) &&
           setValue(key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        console::g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    traceIid("DllGetClassObject", clsid);
    if (clsid == CLSID_Snapin) return g_snapinFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_About) return g_aboutFactory.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return E_FAIL;
    const std::wstring dll = path;
    const std::wstring snapin = kSnapIns + guidText(CLSID_Snapin);
    const std::wstring root = guidText(kNodeRoot), section = guidText(kNodeSection);
    bool ok = registerClass(CLSID_Snapin, L"Эмулкон — оснастка MMC", dll) &&
              registerClass(CLSID_About, L"Эмулкон — о программе", dll) &&
              setValue(snapin, L"NameString", kSnapinName) && setValue(snapin, L"About", guidText(CLSID_About)) &&
              setValue(snapin, L"Provider", kProvider) && setValue(snapin, L"Version", console::widen(EMULKON_VERSION)) &&
              setValue(snapin + L"\\StandAlone", nullptr, L"") && setValue(snapin + L"\\NodeTypes\\" + root, nullptr, L"") &&
              setValue(snapin + L"\\NodeTypes\\" + section, nullptr, L"") &&
              setValue(kNodeTypes + root, nullptr, L"Эмулкон") && setValue(kNodeTypes + section, nullptr, L"Эмулкон: раздел");
    return ok ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kSnapIns + guidText(CLSID_Snapin)).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kNodeTypes + guidText(kNodeRoot)).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kNodeTypes + guidText(kNodeSection)).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kClasses + guidText(CLSID_Snapin)).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kClasses + guidText(CLSID_About)).c_str());
    return S_OK;
}
