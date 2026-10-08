// Windows: the common file dialogs, on a thread of their own (COM's single-threaded apartment
// there, as the shell's parts of them want), owned by Brack's window, which they disable meanwhile.
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <string>

#include "file_dialog.h"
#include "util/common.h"

namespace brack {

namespace {
HWND g_owner = nullptr;

std::wstring filterString(const char* name, const char* pattern) {
    std::wstring f = widen(name);
    f.push_back(L'\0');
    f += widen(pattern);
    f.push_back(L'\0');
    f += L"All files";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

std::string run(bool save, const char* name, const char* pattern, const char* ext) {
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring filter = filterString(name, pattern);
    std::wstring defExt = ext ? widen(ext) : std::wstring();
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrDefExt = defExt.empty() ? nullptr : defExt.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? narrow(file) : std::string();
}

std::future<std::string> runAsync(bool save, std::string name, std::string pattern, std::string ext) {
    return std::async(std::launch::async, [=] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        std::string path = run(save, name.c_str(), pattern.c_str(), ext.empty() ? nullptr : ext.c_str());
        if (com) CoUninitialize();
        return path;
    });
}
}  // namespace

void setFileDialogOwner(GLFWwindow* owner) { g_owner = owner ? glfwGetWin32Window(owner) : nullptr; }
std::future<std::string> openFileDialog(std::string filterName, std::string pattern) {
    return runAsync(false, std::move(filterName), std::move(pattern), {});
}
std::future<std::string> saveFileDialog(std::string filterName, std::string pattern, std::string defaultExt) {
    return runAsync(true, std::move(filterName), std::move(pattern), std::move(defaultExt));
}

}  // namespace brack
