#include "taskdialog.h"

#include <commctrl.h>
#include <shellapi.h>

namespace console {

namespace {

HRESULT CALLBACK taskDialogLinks(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        const wchar_t* url = reinterpret_cast<const wchar_t*>(lp);
        if (url && wcsncmp(url, L"https://", 8) == 0) ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

}  // namespace

bool newVersionTaskDialog(HWND parent, const std::wstring& head, const std::wstring& installed, const std::wstring& page,
                          const std::wstring& about, int& answer) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&taskDialogLinks), &self);
    ACTCTXW actx{sizeof actx};
    actx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    actx.hModule = self;
    actx.lpResourceName = MAKEINTRESOURCEW(3);
    HANDLE ctx = CreateActCtxW(&actx);
    if (ctx == INVALID_HANDLE_VALUE) return false;
    ULONG_PTR cookie = 0;
    bool shown = false;
    if (ActivateActCtx(ctx, &cookie)) {
        using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        HMODULE cc = LoadLibraryW(L"comctl32.dll");  // в активированном контексте — версия 6
        auto fn = cc ? reinterpret_cast<TaskDialogFn>(GetProcAddress(cc, "TaskDialogIndirect")) : nullptr;
        if (fn) {
            // Адрес страницы собран из адреса выпуска GitHub; кавычка или «<» сломали бы разметку ссылки.
            const bool link = !page.empty() && page.find_first_of(L"\"<>") == std::wstring::npos;
            std::wstring content = installed;
            if (link) content += L" <a href=\"" + page + L"\">Что нового</a> — список изменений на GitHub.";
            content += L"\n\n" + about;
            const TASKDIALOG_BUTTON buttons[] = {{IDYES, L"Скачать и установить"}};
            TASKDIALOGCONFIG cfg{sizeof cfg};
            cfg.hwndParent = parent;
            cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
            cfg.pszWindowTitle = L"Эмулкон";
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            cfg.pszMainInstruction = head.c_str();
            cfg.pszContent = content.c_str();
            cfg.pButtons = buttons;
            cfg.cButtons = 1;
            cfg.nDefaultButton = IDYES;
            cfg.pfCallback = taskDialogLinks;
            shown = SUCCEEDED(fn(&cfg, &answer, nullptr, nullptr));
        }
        DeactivateActCtx(0, cookie);
    }
    ReleaseActCtx(ctx);
    return shown;
}

}  // namespace console
