// Windows entry point. axmol's CMake template (AXGameSourceSetup.cmake)
// expects proj.win32/{main.cpp,main.h,resource.h,game.rc} at these paths.
// All argument handling lives in app/AppMain.cpp, shared with macOS and
// Linux; this file only converts the UTF-16 command line to UTF-8.
//
// STATUS: scaffold, never built. See PORTING.md for what's left.

#include "main.h"

#include "AppMain.h"

#include <shellapi.h>  // CommandLineToArgvW

#include <string>
#include <vector>

// Attach a console so stderr logging ("[inspect] listening on ...",
// OCS_LOG output) is visible when launched from a terminal.
#define USE_WIN32_CONSOLE

namespace
{
std::string toUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/// argv[1..] as UTF-8. Uses GetCommandLineW so it is independent of
/// whether the build defines UNICODE/_UNICODE.
std::vector<std::string> utf8Args()
{
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return args;
    for (int i = 1; i < argc; ++i) args.push_back(toUtf8(argv[i]));
    ::LocalFree(argv);
    return args;
}
}  // namespace

#if !defined(_CONSOLE)
int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

#    ifdef USE_WIN32_CONSOLE
#        include "platform/win32/EmbedConsole.h"
#    endif

    return walshardMain(utf8Args());
}
#else
int main(int, char**)
{
    return walshardMain(utf8Args());
}
#endif
