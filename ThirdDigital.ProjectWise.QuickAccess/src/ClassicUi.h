#pragma once
#include <windows.h>

namespace QuickAccess
{
	struct ClassicRestoreResult { unsigned found = 0, restored = 0, skipped = 0, failed = 0; };
	ClassicRestoreResult RestoreClassicToolbars(HWND frame) noexcept;
	bool PreferClassicUi() noexcept;
	bool SaveClassicUiPreference(bool classic) noexcept;
}
