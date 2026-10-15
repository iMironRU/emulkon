// Оснастка MMC «Эмулкон» (docs/local-install.md §4–§5): регистрация DLL и готовая консоль Emulkon.msc.
#include <windows.h>

#include <string>

#include "setup.h"

namespace setup {

namespace {

// Те же GUID, что в console/src/snapin.cpp: оснастка, «О программе», типы узлов.
const wchar_t* const kSnapinClsid = L"{1C4354F7-8B1F-4FF0-912D-50493CBE677C}";
const wchar_t* const kAboutClsid = L"{BD3F6610-A1AB-4AB0-9B2F-9A76B5D41F10}";
const wchar_t* const kNodeTypes[] = {L"{23A02B6E-B8F3-4241-A091-DDED8D8D19DF}", L"{3F874E3C-360C-4E69-9771-D6908E304B42}"};

// Консоль — файл MMC 3.0 (XML): единственный корень — узел оснастки «Эмулкон», режим пользователя с
// одним окном (UserSDI), как у services.msc: без вопроса о сохранении, оснастки не добавляются.
// Собирается текстом: автоматизация MMC20.Application (SnapIns.Add) в Windows 11 падает на любой
// оснастке, а консоль от машины не зависит.
const char kConsoleXml[] = R"(<?xml version="1.0" encoding="utf-8"?>
<MMC_ConsoleFile ConsoleVersion="3.0" ProgramMode="UserSDI">
  <ConsoleFileID>{6E0F4C8A-2B7D-4F1E-9A3C-5D8B7E6F1A20}</ConsoleFileID>
  <FrameState ShowStatusBar="true" LogicalReadOnly="false">
    <WindowPlacement ShowCommand="SW_SHOWNORMAL">
      <Point Name="MinPosition" X="-1" Y="-1"/>
      <Point Name="MaxPosition" X="-1" Y="-1"/>
      <Rectangle Name="NormalPosition" Top="60" Bottom="620" Left="80" Right="1080"/>
    </WindowPlacement>
  </FrameState>
  <Views>
    <View ID="4" ScopePaneWidth="200" ActionsPaneWidth="-1">
      <BookMark Name="RootNode" NodeID="3"/>
      <BookMark Name="SelectedNode" NodeID="3"/>
      <WindowPlacement WPF_RESTORETOMAXIMIZED="true" ShowCommand="SW_SHOWMAXIMIZED">
        <Point Name="MinPosition" X="-1" Y="-1"/>
        <Point Name="MaxPosition" X="-4" Y="-23"/>
        <Rectangle Name="NormalPosition" Top="66" Bottom="331" Left="66" Right="564"/>
      </WindowPlacement>
      <ViewOptions ViewMode="Report" ScopePaneVisible="true" DescriptionBarVisible="false" DefaultColumn0Width="200" DefaultColumn1Width="0"/>
    </View>
  </Views>
  <VisualAttributes>
    <String Name="ApplicationTitle" ID="3"/>
  </VisualAttributes>
  <Favorites>
    <Favorite TYPE="Group">
      <String Name="Name" ID="4"/>
      <Favorites/>
    </Favorite>
  </Favorites>
  <ScopeTree>
    <SnapinCache>
      <Snapin CLSID="{1C4354F7-8B1F-4FF0-912D-50493CBE677C}" AllExtensionsEnabled="true"/>
    </SnapinCache>
    <Nodes>
      <Node ID="3" ImageIdx="0" CLSID="{1C4354F7-8B1F-4FF0-912D-50493CBE677C}" Preload="true">
        <Nodes/>
        <String Name="Name" ID="5"/>
        <ComponentDatas/>
        <Components/>
      </Node>
    </Nodes>
  </ScopeTree>
  <ConsoleTaskpads/>
  <ViewSettingsCache/>
  <ColumnSettingsCache/>
  <StringTables>
    <IdentifierPool AbsoluteMin="1" AbsoluteMax="65535" NextAvailable="6"/>
    <StringTable>
      <GUID>{71E5B33E-1064-11D2-808F-0000F875A9CE}</GUID>
      <Strings>
        <String ID="3" Refs="1">Эмулкон</String>
        <String ID="4" Refs="1">Избранное</String>
        <String ID="5" Refs="1">Эмулкон</String>
      </Strings>
    </StringTable>
  </StringTables>
  <BinaryStorage/>
</MMC_ConsoleFile>
)";

}  // namespace

bool registerSnapIn(const std::wstring& dll, std::wstring& error) {
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        error = L"не загружается " + dll + L": код " + std::to_wstring(GetLastError());
        return false;
    }
    using Fn = HRESULT(STDAPICALLTYPE*)();
    auto reg = reinterpret_cast<Fn>(GetProcAddress(m, "DllRegisterServer"));
    HRESULT hr = reg ? reg() : E_NOINTERFACE;
    FreeLibrary(m);
    if (FAILED(hr)) {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%08X", (unsigned)hr);
        error = std::wstring(L"DllRegisterServer: ") + buf;
    }
    return SUCCEEDED(hr);
}

void unregisterSnapIn() {
    // Без DLL: её может уже не быть (удаление после сбоя).
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Microsoft\\MMC\\SnapIns\\") + kSnapinClsid).c_str());
    for (const wchar_t* n : kNodeTypes)
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Microsoft\\MMC\\NodeTypes\\") + n).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kSnapinClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kAboutClsid).c_str());
}

bool createConsoleFile(const std::wstring& msc, std::wstring& error) {
    HANDLE h = CreateFileW(msc.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = msc + L": код " + std::to_wstring(GetLastError());
        return false;
    }
    DWORD written = 0;
    const DWORD size = sizeof(kConsoleXml) - 1;
    bool ok = WriteFile(h, kConsoleXml, size, &written, nullptr) && written == size;
    if (!ok) error = msc + L": код " + std::to_wstring(GetLastError());
    CloseHandle(h);
    return ok;
}

}  // namespace setup
