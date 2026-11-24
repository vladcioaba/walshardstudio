// macOS entry point. axmol's CMake template expects it at this exact path.
// All argument handling lives in app/AppMain.cpp, shared with Linux and
// Windows.

#include "AppMain.h"

int main(int argc, char* argv[])
{
    return walshardMain(std::vector<std::string>(argv + 1, argv + argc));
}
