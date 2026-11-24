// PlatformCompat.h — tiny POSIX shims so shared code compiles on Windows.
//
// Covers only the calls used in the editor (getpid, popen/pclose). Anything
// bigger (process spawning, sockets, IPC) has a real per-platform
// implementation instead — see PlatformSpawn.cpp, InspectServer.cpp,
// IpcBridge.cpp. Include this instead of <unistd.h> in shared code.

#pragma once

#if defined(_WIN32)
#    include <process.h>  // _getpid
#    include <cstdio>     // _popen / _pclose
#    ifndef getpid
#        define getpid _getpid
#    endif
#    ifndef popen
#        define popen _popen
#    endif
#    ifndef pclose
#        define pclose _pclose
#    endif
#else
#    include <unistd.h>
#endif
