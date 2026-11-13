#include <windows.h>
#include <commctrl.h>
#include <aawindms.h>
#include <delayimp.h>

#include "QuickAccess.h"

#include <array>
#include <cwchar>

namespace
{
	HINSTANCE g_instance = nullptr;
	HWND g_window = nullptr;
	HWND g_frame = nullptr;
	HWND g_selectionControl = nullptr;
	UINT g_toggleCommand = 0;
	UINT g_viewCommand = 0;
	UINT_PTR g_startupTimer = 0;
	unsigned g_attempts = 0;
	LONG g_initialised = 0;
	bool g_menuError = false;
	std::array<UINT, QuickAccess::kButtonCount> g_commands{};
	HWND g_navigationTree = nullptr;
	HWND g_navigationList = nullptr;
	bool g_addressError = false;
	LONG g_historyCount = -2;
	LONG g_historyPosition = -2;
	bool g_historyEnabled = false;
	bool g_hasParent = false;

	constexpr UINT kCommands[][3] = {
		{IDMP_CREATE, 0, 0},
		{IDMD_CREATE_MULTIPLE, 0, 0},
		{IDMD_CHECKOUT, 0, 0},
		{IDMD_CHECKIN, 0, 0},
		{IDMD_FREE, 0, 0},
		{IDM_PROPERTIES, IDMD_PROPERTIES, IDMP_PROPERTY},
		{IDM_REFRESH, IDMP_REFRESH, ID_VIEW_REFRESH},
		{IDM_OPEN, IDMD_OPEN, 0},
		{IDMD_NEW, 0, 0},
		{ID_VIEW_THUMBNAIL, 0, 0},
		{ID_VIEW_LARGEICONS, 0, 0},
		{ID_VIEW_SMALLICONS, 0, 0},
		{ID_VIEW_LISTVW, 0, 0},
		{ID_VIEW_REPORT, 0, 0},
		{ID_VIEW_MANAGE, 0, 0},
		{IDMT_SET_INTERFACE, 0, 0},
		{IDMT_QUERY_DLG, 0, 0},
		{IDMP_FIND_DOCS, 0, 0},
		{IDMT_CHECKIN_DLG, 0, 0},
		{IDMT_USER_SETTINGS, 0, 0},
		{IDMT_WIZARD_MANAGER, 0, 0},
		{AAMENU_HELP_INDEX, 0, 0},
	};
	static_assert(ARRAYSIZE(kCommands) == QuickAccess::kButtonCount);

	void Report(const wchar_t* message) noexcept
	{
		OutputDebugStringW(L"ProjectWise Quick Access: ");
		OutputDebugStringW(message);
		OutputDebugStringW(L"\n");
	}

	int SdkLoadExceptionFilter(DWORD code) noexcept
	{
		return code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
			code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND)
			? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
	}

	bool RefreshCommands(void*, QuickAccess::States& states)
	{
		states = {};
		g_commands.fill(0);
		if (!IsWindow(g_frame) || !IsWindowEnabled(g_frame)) return false;
		const HWND tree = aaApi_GetMainDscTree();
		const HWND list = aaApi_GetActiveDocumentList();
		const HWND control = QuickAccess::SelectionControl(g_frame, tree, list, g_selectionControl);
		if (control == nullptr) return false;
		g_selectionControl = control;
		HMENU menu = CreatePopupMenu();
		if (menu == nullptr)
		{
			Report(L"could not create command-state menu");
			return false;
		}
		bool initialised = false;
		__try
		{
			// InitMenuByAaControl updates ProjectWise menu items; it does not create an empty menu's layout.
			for (const LONG type : {AAMENU_PROJECT, AAMENU_DOCUMENT, AAMENU_GENERIC, AAMENU_VIEW, AAMENU_TOOLS, AAMENU_HELP})
			{
				HMENU commands = aaApi_InsertMenuByType(type, menu, static_cast<UINT>(GetMenuItemCount(menu)));
				if (commands == nullptr ||
					aaApi_InitMenuByAaControl(control, commands, AAMG_MODIFY | AAMG_PROPERTIES | AAMG_VIEW) == 0) continue;
				initialised = true;
				for (std::size_t i = 0; i < states.size(); ++i)
				{
					for (const UINT command : kCommands[i])
					{
						QuickAccess::ButtonState candidate{};
						if (command != 0 && QuickAccess::ReadMenuState(commands, command, candidate) &&
							(g_commands[i] == 0 || (!states[i].enabled && candidate.enabled)))
						{
							states[i] = candidate;
							g_commands[i] = command;
							if (candidate.enabled) break;
						}
					}
				}
			}
		}
		__finally
		{
			DestroyMenu(menu);
		}
		bool found = false;
		for (const UINT command : g_commands) found = found || command != 0;
		initialised = initialised && found;
		if (!initialised && !g_menuError) Report(L"Explorer command-state query failed; buttons disabled");
		g_menuError = !initialised;
		return initialised;
	}

	bool RefreshGuarded(void* context, QuickAccess::States& states);

	HWND CreateAddress(void*, HWND parent)
	{
		__try
		{
			g_navigationTree = nullptr;
			g_navigationList = nullptr;
			g_addressError = false;
			g_historyCount = g_historyPosition = -2;
			const HWND address = aaApi_CreateAddressBar(parent, 0, 0, 600, 240, 0);
			if (address == nullptr) Report(L"could not create the ribbon's native ProjectWise address bar");
			else ShowWindow(address, SW_SHOWNOACTIVATE);
			return address;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK address bar dependency or export unavailable");
			return nullptr;
		}
	}

	bool ExplorerControl(HWND control) noexcept
	{
		return IsWindow(control) && IsChild(g_frame, control) && GetWindowThreadProcessId(control, nullptr) == GetCurrentThreadId();
	}

	HWND NavigationList()
	{
		const HWND active = aaApi_GetActiveDocumentList();
		if (ExplorerControl(active)) return active;
		const HWND main = aaApi_GetMainDocumentList();
		return ExplorerControl(main) ? main : nullptr;
	}

	HTREEITEM ParentTreeItem(HWND tree) noexcept
	{
		if (!ExplorerControl(tree)) return nullptr;
		const HTREEITEM selected = TreeView_GetSelection(tree);
		return selected != nullptr ? TreeView_GetParent(tree, selected) : nullptr;
	}

	bool RefreshNavigation(void*, HWND, QuickAccess::NavigationStates& states)
	{
		states = {};
		__try
		{
			if (!IsWindow(g_frame) || !IsWindowEnabled(g_frame)) return false;
			const HWND tree = aaApi_GetMainDscTree(), list = NavigationList();
			const bool enabled = ExplorerControl(list) && aaApi_DocListIsNavigatorEnabled(list) != FALSE;
			const LONG count = enabled ? aaApi_DocListNavigatorGetCount(list) : -1;
			const LONG position = enabled ? aaApi_DocListNavigatorGetCurrentPos(list) : -1;
			const bool hasParent = ParentTreeItem(tree) != nullptr;
			states = QuickAccess::NavigationAvailability(enabled, count, position, hasParent);
			if (count != g_historyCount || position != g_historyPosition || enabled != g_historyEnabled || hasParent != g_hasParent)
			{
				wchar_t message[200]{};
				swprintf_s(message, L"navigation history enabled=%d count=%ld position=%ld; Back=%d Forward=%d Up=%d",
					enabled, count, position, states[0].enabled, states[1].enabled, states[2].enabled);
				Report(message);
				g_historyEnabled = enabled;
				g_historyCount = count;
				g_historyPosition = position;
				g_hasParent = hasParent;
			}
			return ExplorerControl(tree) || ExplorerControl(list);
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK document-list navigator dependency or export unavailable");
			return false;
		}
	}

	bool RefreshAddress(void*, HWND address)
	{
		__try
		{
			if (!IsWindow(g_frame) || !IsWindowEnabled(g_frame) || !IsWindow(address)) return false;
			const HWND tree = aaApi_GetMainDscTree(), list = NavigationList();
			for (const HWND control : {tree, list})
			{
				HWND& registered = control == tree ? g_navigationTree : g_navigationList;
				if (!IsWindow(control) || registered == control) continue;
				if (!aaApi_AddAddressBarSynchronization(address, control))
				{
					if (!g_addressError) Report(L"could not synchronize the ribbon address bar; Back/Forward/Up remain independent");
					g_addressError = true;
					return false;
				}
				registered = control;
			}
			const HWND focus = GetFocus(), mainAddress = aaApi_GetMainAddressBar();
			if (IsWindow(mainAddress) && focus != address && !IsChild(address, focus))
			{
				wchar_t current[4096]{}, shown[4096]{};
				GetWindowTextW(mainAddress, current, ARRAYSIZE(current));
				GetWindowTextW(address, shown, ARRAYSIZE(shown));
				if (wcscmp(current, shown) != 0)
				{
					const HMONIKER moniker = aaApi_GetAddressBarMoniker(mainAddress);
					if (moniker != nullptr && !aaApi_SetAddressBarMoniker(address, moniker))
					{
						if (!g_addressError) Report(L"could not update the ribbon address from Explorer's current location");
						g_addressError = true;
						return false;
					}
				}
			}
			g_addressError = false;
			return ExplorerControl(tree) || ExplorerControl(list);
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			if (!g_addressError) Report(L"SDK address dependency or export unavailable");
			g_addressError = true;
			return false;
		}
	}

	bool Navigate(void*, std::size_t button)
	{
		if (button >= QuickAccess::kNavigationCount || !IsWindowEnabled(g_frame)) return false;
		__try
		{
			QuickAccess::NavigationStates states{};
			if (!RefreshNavigation(nullptr, nullptr, states) || !states[button].enabled) return false;
			if (button < 2)
			{
				const HWND list = NavigationList();
				if (!ExplorerControl(list)) return false;
				SetFocus(list);
				if (GetFocus() != list && !IsChild(list, GetFocus())) return false;
				// autoSelect=TRUE lets the SDK apply the history item to its document-list controls.
				const auto* item = button == 0 ? aaApi_DocListNavigatorMoveBack(list, TRUE, 1, nullptr) :
					aaApi_DocListNavigatorMoveForward(list, TRUE, 1, nullptr);
				return item != nullptr;
			}
			const HWND tree = aaApi_GetMainDscTree();
			const HTREEITEM parent = ParentTreeItem(tree);
			if (parent == nullptr) return false;
			SetFocus(tree);
			if (GetFocus() != tree && !IsChild(tree, GetFocus())) return false;
			// The SDK explicitly requires this selector instead of TreeView_SelectItem for a DSC tree.
			return aaApi_DscTreeSelectHTreeItem(tree, parent) != FALSE;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK navigation dependency or export unavailable");
			return false;
		}
	}

	bool RefreshGuarded(void* context, QuickAccess::States& states)
	{
		__try
		{
			return RefreshCommands(context, states);
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			states = {};
			g_commands.fill(0);
			if (!g_menuError) Report(L"SDK dependency or export unavailable; buttons disabled");
			g_menuError = true;
			return false;
		}
	}

	bool Execute(void*, std::size_t button)
	{
		if (button >= g_commands.size() || g_commands[button] == 0 || !IsWindowEnabled(g_frame)) return false;
		if (!IsWindow(g_selectionControl)) return false;
		SetFocus(g_selectionControl);
		const HWND focus = GetFocus();
		if (focus != g_selectionControl && !IsChild(g_selectionControl, focus))
		{
			Report(L"could not restore the selection control for command dispatch");
			return false;
		}
		// Dispatch synchronously while the same selection control still has focus.
		SendMessageW(g_frame, WM_COMMAND, MAKEWPARAM(g_commands[button], 0), 0);
		return true;
	}

	bool Search(void*, const wchar_t* text)
	{
		__try
		{
			const HWND list = aaApi_GetActiveDocumentList();
			if (!IsWindowEnabled(g_frame) || !IsWindow(list) ||
				GetWindowThreadProcessId(list, nullptr) != GetCurrentThreadId())
			{
				Report(L"search requires an enabled Explorer frame and an active document list");
				return false;
			}
			if (!aaApi_ExecuteSimpleQueryToDocumentList(list, text, nullptr))
			{
				Report(L"ProjectWise simple search failed");
				return false;
			}
			return true;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK dependency or search export unavailable");
			return false;
		}
	}

	bool EnsureWindow()
	{
		if (IsWindow(g_window)) return true;
		g_frame = aaApi_GetMainFrameWindow();
		if (!IsWindow(g_frame) || GetWindowThreadProcessId(g_frame, nullptr) != GetCurrentThreadId()) return false;
		g_window = QuickAccess::Create(g_frame, g_instance, {nullptr, RefreshGuarded, Execute, Search, CreateAddress, RefreshNavigation, Navigate,
			[](void*) -> bool {
				const HWND target = QuickAccess::SelectionControl(g_frame, aaApi_GetMainDscTree(), aaApi_GetActiveDocumentList(), g_selectionControl);
				if (target == nullptr) { Report(L"no Explorer selection control is available for keyboard focus"); return false; }
				SetFocus(target);
				return GetFocus() == target || IsChild(target, GetFocus());
			}, RefreshAddress});
		if (g_window != nullptr && !QuickAccess::Dock(g_window, true))
			Report(L"Explorer's content pane is not ready for docking; use the toolbar context menu to retry");
		return g_window != nullptr;
	}

	LONG ToggleImpl(LPAACMDPARAM command)
	{
		if (command == nullptr || (static_cast<UINT>(command->lCommandId) != g_toggleCommand &&
			static_cast<UINT>(command->lCommandId) != g_viewCommand)) return -1;
		if (!EnsureWindow())
		{
			Report(L"could not open toolbar");
			MessageBoxW(aaApi_GetMainFrameWindow(), L"The Quick Access toolbar could not be created on Explorer's UI thread.",
						L"ProjectWise Quick Access", MB_OK | MB_ICONERROR);
			return -1;
		}
		const bool show = QuickAccess::GetDisplayMode(g_window) == QuickAccess::DisplayMode::Hidden || !IsWindowVisible(g_window);
		return (show ? QuickAccess::Restore(g_window) : QuickAccess::SetDisplayMode(g_window, QuickAccess::DisplayMode::Hidden)) ? 0 : -1;
	}

	LONG CALLBACK Toggle(LPAACMDPARAM command)
	{
		__try
		{
			return ToggleImpl(command);
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK dependency or export unavailable in toolbar visibility callback");
			MessageBoxW(g_frame, L"Explorer could not open the Quick Access toolbar. See the debug output for details.",
						L"ProjectWise Quick Access", MB_OK | MB_ICONERROR);
			return -1;
		}
	}

	LONG CALLBACK ToggleState(LPAACMDSTATE)
	{
		return IsWindow(g_window) && IsWindowVisible(g_window) &&
			QuickAccess::GetDisplayMode(g_window) != QuickAccess::DisplayMode::Hidden ? AAMS_CHECKED : 0;
	}

	void CALLBACK Startup(HWND, UINT, UINT_PTR timer, DWORD)
	{
		__try
		{
			if (EnsureWindow())
			{
				KillTimer(nullptr, timer);
				g_startupTimer = 0;
				ShowWindow(g_window, SW_SHOWNOACTIVATE);
			}
			else if (++g_attempts >= 120)
			{
				KillTimer(nullptr, timer);
				g_startupTimer = 0;
				Report(L"Explorer frame was unavailable; use Tools > Quick Access Toolbar to retry");
			}
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			KillTimer(nullptr, timer);
			g_startupTimer = 0;
			Report(L"SDK dependency or export unavailable; toolbar not opened");
		}
	}

	bool AddVisibilityCommand(LONG type, const wchar_t* name, UINT& id)
	{
		id = aaApi_ReserveCommandIdByType(type);
		if (id == 0)
		{
			Report(L"could not reserve toolbar visibility command");
			return false;
		}
		AADMSMENUITEM item{};
		item.ulMask = AADMSMIF_FLAGS | AADMSMIF_NAME | AADMSMIF_CMDTYPE | AADMSMIF_CMDID | AADMSMIF_CALLBACK | AADMSMIF_STATEPROC;
		item.uiFlags = AAMIF_COMMAND;
		item.uiCmdType = static_cast<UINT>(type);
		item.uiCommandId = id;
		item.ushParamType = AACMDPT_GENERIC;
		item.lptstrName = const_cast<LPWSTR>(name);
		item.fpExecute = Toggle;
		item.fpGetState = ToggleState;
		if (aaApi_AddCustomMenuItem(0, AADMSMI_LAST, &item) == 0)
		{
			Report(L"could not register ribbon visibility menu command");
			return false;
		}
		return true;
	}

	bool Initialise()
	{
		if (!AddVisibilityCommand(AAMENU_TOOLS, L"Quick Access Toolbar", g_toggleCommand) ||
			!AddVisibilityCommand(AAMENU_VIEW, L"Ribbon", g_viewCommand)) return false;
		g_startupTimer = SetTimer(nullptr, 0, 250, Startup);
		if (g_startupTimer == 0)
		{
			Report(L"could not schedule toolbar startup; use the Tools menu to open it");
			return false;
		}
		Report(L"module initialised");
		return true;
	}
}

BOOL APIENTRY DllMain(HMODULE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_instance = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

extern "C" LONG WINAPI CustomInitialize(ULONG applicationMask, LPVOID)
{
	wchar_t path[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
	const wchar_t* name = wcsrchr(path, L'\\');
	if ((applicationMask & AAMODULE_EXPLORER) == 0 || length == 0 || length >= ARRAYSIZE(path) ||
		_wcsicmp(name != nullptr ? name + 1 : path, L"pwc.exe") != 0) return IDOK;
	if (InterlockedCompareExchange(&g_initialised, 1, 0) != 0) return IDOK;
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
						  reinterpret_cast<LPCWSTR>(&CustomInitialize), &self))
	{
		Report(L"could not pin module; toolbar inactive");
		return IDOK;
	}
	__try
	{
		Initialise();
	}
	__except (SdkLoadExceptionFilter(GetExceptionCode()))
	{
		Report(L"SDK dependency or export unavailable; toolbar inactive");
	}
	return IDOK;
}
