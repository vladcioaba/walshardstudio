#pragma once

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers

#ifndef NOMINMAX  // keep <windows.h> min/max macros off std::min/std::max
#    define NOMINMAX
#endif
#include <windows.h>
#include <tchar.h>

#include "platform/StdC.h"
