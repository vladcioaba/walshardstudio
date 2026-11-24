#include "AppMain.h"

#include "AppDelegate.h"
#include "axmol.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

int walshardMain(const std::vector<std::string>& args)
{
    for (const std::string& arg : args)
    {
        const std::string_view a(arg);
        if (a.rfind("--panel=", 0) == 0)
        {
            AppDelegate::sPanelMode.assign(a.substr(8));
            continue;
        }
        if (a.rfind("--size=", 0) == 0)
        {
            // Format: --size=WxH (integers, no spaces). Parent window
            // passes this when detaching a panel so the new OS window
            // opens at the exact panel size.
            const std::string rest(a.substr(7));
            const auto xpos = rest.find('x');
            if (xpos != std::string::npos)
            {
                AppDelegate::sPanelWindowW = std::atoi(rest.substr(0, xpos).c_str());
                AppDelegate::sPanelWindowH = std::atoi(rest.substr(xpos + 1).c_str());
            }
            continue;
        }
        // TODO(windows): `arg` is UTF-8, but std::filesystem::path(std::string)
        // uses the ANSI code page on Windows, so non-ASCII paths will be
        // mangled. Convert via std::u8string (C++20) once the rest of the
        // editor's path handling is audited for the same issue.
        AppDelegate::sCsdPaths.push_back(std::filesystem::absolute(arg).string());
    }
    // Back-compat: expose the first path as sCsdPath so code that still
    // reads the single-path field (older tooling, tests) sees it.
    if (!AppDelegate::sCsdPaths.empty())
        AppDelegate::sCsdPath = AppDelegate::sCsdPaths.front();

    AppDelegate app;
    const int result = ax::Application::getInstance()->run();

#if AX_OBJECT_LEAK_DETECTION
    ax::Object::printLeaks();
#endif
    return result;
}
