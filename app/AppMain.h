// AppMain.h — platform-independent entry point.
//
// Every platform `main` (proj.ios_mac/mac, proj.linux, proj.win32) turns
// its native command line into UTF-8 strings and calls walshardMain().
// Keeping argv parsing here means the three entry points stay one-liners
// and can't drift apart.

#pragma once

#include <string>
#include <vector>

/// `args` excludes the program name (argv[0]).
///
/// Recognised arguments:
///   --panel=<name>   detached-panel mode (Scene|Assets|Properties|Layout|Mode)
///   --size=WxH       initial window size for a detached panel
///   <path>           a .csd to open as a tab (several allowed)
int walshardMain(const std::vector<std::string>& args);
