#pragma once
#include <windows.h>

#include <string>

#include "../core/util.h"

namespace ed {
std::wstring pick_folder(HWND owner, const fs::path& start);
std::wstring pick_wav(HWND owner);
}  // namespace ed
