#include "ClassicUi.h"
#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace QuickAccess
{
	namespace
	{
		constexpr wchar_t kPreference[] = L"Software\\AtkinsRealis\\ProjectWise\\UI";
		struct FindBar { UINT id; HWND window = nullptr; HWND frame = nullptr; };
		BOOL CALLBACK MatchBar(HWND window, LPARAM parameter)
		{
			auto& match = *reinterpret_cast<FindBar*>(parameter);
			if (static_cast<UINT>(GetDlgCtrlID(window)) != match.id) return TRUE;
			wchar_t name[96]{}; GetClassNameW(window, name, ARRAYSIZE(name));
			if (_wcsicmp(name, TOOLBARCLASSNAMEW) != 0 && _wcsicmp(name, REBARCLASSNAMEW) != 0 &&
				!(match.id >= 0xE800 && match.id <= 0xE8FF && wcsncmp(name, L"Afx:", 4) == 0)) return TRUE;
			match.window = window; return FALSE;
		}
		BOOL CALLBACK FloatingBar(HWND window, LPARAM parameter)
		{
			auto& match = *reinterpret_cast<FindBar*>(parameter);
			if (GetAncestor(window, GA_ROOTOWNER) != match.frame) return TRUE;
			EnumChildWindows(window, MatchBar, parameter);
			return match.window == nullptr;
		}
		HWND Bar(HWND frame, UINT id)
		{
			FindBar match{id, nullptr, frame};
			EnumChildWindows(frame, MatchBar, reinterpret_cast<LPARAM>(&match));
			if (match.window == nullptr) EnumThreadWindows(GetCurrentThreadId(), FloatingBar, reinterpret_cast<LPARAM>(&match));
			return match.window;
		}
		bool Find(HMENU menu, UINT command, HMENU& parent, UINT& position, int depth = 0)
		{
			if (depth > 16) return false;
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
			{
				MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_ID | MIIM_SUBMENU;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) continue;
				if (item.hSubMenu != nullptr) { if (Find(item.hSubMenu, command, parent, position, depth + 1)) return true; }
				else if (item.wID == command) { parent = menu; position = static_cast<UINT>(i); return true; }
			}
			return false;
		}
		void Commands(HWND frame, HMENU menu, std::vector<UINT>& commands, int depth = 0)
		{
			if (depth > 16) return;
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
			{
				MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) continue;
				if (item.hSubMenu != nullptr) { Commands(frame, item.hSubMenu, commands, depth + 1); continue; }
				// MFC control-bar command IDs, plus toolbar controls whose IDs match actual host menu commands.
				// Never guess command IDs or toggle unrelated unchecked menu actions such as Customize.
				if ((item.fType & MFT_SEPARATOR) != 0 || item.wID == 0 || item.wID == static_cast<UINT>(-1) ||
					!(item.wID == 0xE800 || item.wID == 0xE801 || Bar(frame, item.wID) != nullptr)) continue;
				if (std::find(commands.begin(), commands.end(), item.wID) == commands.end()) commands.push_back(item.wID);
			}
		}
		UINT CurrentState(HWND frame, UINT command)
		{
			HMENU parent = nullptr; UINT position = 0;
			if (!Find(GetMenu(frame), command, parent, position)) return static_cast<UINT>(-1);
			SendMessageW(frame, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(parent), MAKELPARAM(0, FALSE));
			if (!Find(GetMenu(frame), command, parent, position)) return static_cast<UINT>(-1);
			return GetMenuState(parent, position, MF_BYPOSITION);
		}
	}

	ClassicRestoreResult RestoreClassicToolbars(HWND frame) noexcept
	{
		ClassicRestoreResult result;
		if (!IsWindow(frame) || !IsWindowEnabled(frame) || GetWindowThreadProcessId(frame, nullptr) != GetCurrentThreadId()) { result.failed = 1; return result; }
		try
		{
			std::vector<UINT> commands; Commands(frame, GetMenu(frame), commands);
			result.found = static_cast<unsigned>(commands.size());
			for (const UINT command : commands)
			{
				const UINT state = CurrentState(frame, command);
				if (state == static_cast<UINT>(-1)) { ++result.failed; continue; }
				if ((state & (MF_DISABLED | MF_GRAYED)) != 0) { ++result.skipped; continue; }
				const HWND bar = Bar(frame, command);
				if (bar != nullptr ? IsWindowVisible(bar) != FALSE : (state & MF_CHECKED) != 0) continue;
				SendMessageW(frame, WM_COMMAND, MAKEWPARAM(command, 0), 0);
				const UINT after = CurrentState(frame, command);
				if ((after != static_cast<UINT>(-1) && (after & MF_CHECKED) != 0) || (bar != nullptr && IsWindowVisible(bar))) ++result.restored;
				else ++result.failed;
			}
			RECT client{}; GetClientRect(frame, &client);
			SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
			DrawMenuBar(frame); RedrawWindow(frame, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
		}
		catch (...) { ++result.failed; }
		return result;
	}

	bool PreferClassicUi() noexcept
	{
		DWORD value = 0, size = sizeof(value);
		const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kPreference, L"UseClassicUI", RRF_RT_REG_DWORD, nullptr, &value, &size);
		return status == ERROR_FILE_NOT_FOUND ? false : status != ERROR_SUCCESS || value != 0;
	}
	bool SaveClassicUiPreference(bool classic) noexcept
	{
		const DWORD value = classic ? 1 : 0;
		return RegSetKeyValueW(HKEY_CURRENT_USER, kPreference, L"UseClassicUI", REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
	}
}
