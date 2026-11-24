// Linux entry point. axmol's CMake template (AXGameSourceSetup.cmake)
// expects it at this exact path. All argument handling lives in
// app/AppMain.cpp, shared with macOS and Windows.
//
// STATUS: scaffold, never built. See PORTING.md for what's left.

#include "AppMain.h"

int main(int argc, char* argv[])
{
    return walshardMain(std::vector<std::string>(argv + 1, argv + argc));
}
