#pragma once
#include <windows.h>

namespace QuickAccess
{
	bool RegisterVersionStacks(HINSTANCE instance, bool available) noexcept;
	void SetVersionStacksAvailable(bool available) noexcept;
}
