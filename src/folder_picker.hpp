#pragma once

#include "language.hpp"

#include <windows.h>
#include <optional>
#include <string>

namespace nf {

std::optional<std::wstring> chooseFolder(HINSTANCE instance, HWND owner, std::wstring initial, Language language);

}
