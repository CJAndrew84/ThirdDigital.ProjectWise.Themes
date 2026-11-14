#include <windows.h>
#include <commctrl.h>
#include <aaapi.h>
#include <aadmsapi.h>
#include <aawindms.h>
#include <delayimp.h>

#include "QuickAccess.h"
#include "Admin.h"
#include "ClassicUi.h"
#include "VersionStacksAdapter.h"

#include <array>
#include <cwchar>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
	HINSTANCE g_instance = nullptr;
	HWND g_window = nullptr;
	HWND g_frame = nullptr;
	HWND g_selectionControl = nullptr;
	UINT g_toggleCommand = 0;
	UINT g_viewCommand = 0;
	UINT g_classicCommand = 0;
	UINT_PTR g_healthTimer = 0;
	bool g_classicPreferred = false;
	bool g_ribbonExpected = false;
	bool g_creationFailed = false;
	bool g_restorePending = false;
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
	HMENU g_workflowMenu = nullptr;
	HDSOURCE g_workflowSource = nullptr;
	LONG g_workflowFolder = 0;
	LONG g_workflowId = 0;
	ULONGLONG g_workflowChecked = 0;
	QuickAccess::SelectorStates g_selectorCache;
	std::wstring g_selectorContext;
	ULONGLONG g_selectorsChecked = 0;

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
		{0, 0, 0}, // Show all versions uses the published user-setting API, not a guessed menu ID.
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

	HWND NavigationList();
	bool ExplorerControl(HWND control) noexcept;

	bool ReadShowAllVersions(QuickAccess::ButtonState& state)
	{
		state = {};
		__try
		{
			if (!IsWindowEnabled(g_frame) || !ExplorerControl(NavigationList()) ||
				aaApi_GetActiveDatasource() == nullptr || aaApi_GetCurrentUserId() <= 0) return false;
			const LONG value = aaApi_GetUserNumericSetting(AADMS_PAR_DIS_SHOW_VERSION);
			if (value != 0 && value != 1) return false;
			state = {true, value != 0};
			return true;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { return false; }
	}

	bool ToggleShowAllVersions()
	{
		__try
		{
			QuickAccess::ButtonState current{};
			if (!ReadShowAllVersions(current)) return false;
			// The GUID-aware setter validates the standard settings group. A failure leaves the displayed state native.
			if (!aaApi_SetUserNumericSettingByGUID(nullptr, AADMS_PAR_DIS_SHOW_VERSION, current.checked ? 0 : 1))
			{
				Report(L"could not change Show all versions");
				return false;
			}
			// Let Explorer repopulate its lists using the same setting as its native control.
			aaApi_UpdateDocumentWindows();
			QuickAccess::ButtonState updated{};
			return ReadShowAllVersions(updated) && updated.checked != current.checked;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK version-display setting dependency or export unavailable");
			return false;
		}
	}

	bool RefreshCommands(void*, QuickAccess::States& states)
	{
		states = {};
		g_commands.fill(0);
		if (!IsWindow(g_frame) || !IsWindowEnabled(g_frame)) return false;
		const HWND tree = aaApi_GetMainDscTree();
		const HWND list = NavigationList();
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
		const bool versionsReady = ReadShowAllVersions(states[QuickAccess::kShowAllVersions]);
		initialised = (initialised && found) || versionsReady;
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
		if (button == QuickAccess::kShowAllVersions) return ToggleShowAllVersions();
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

	struct OwnedMenu
	{
		HMENU value = nullptr;
		~OwnedMenu() { if (value != nullptr) DestroyMenu(value); }
	};
	struct OwnedBuffer
	{
		HAADMSBUFFER value = nullptr;
		~OwnedBuffer() { if (value != nullptr) aaApi_DmsDataBufferFree(value); }
	};

	bool BuildSelectors(QuickAccess::SelectorStates& result, bool force)
	{
		result = {};
		const HWND list = NavigationList();
		const HDSOURCE source = aaApi_GetActiveDatasource();
		if (source == nullptr || !ExplorerControl(list)) return true;
		const LONG folder = aaApi_DocListGetProject(list);
		const std::wstring context = std::to_wstring(reinterpret_cast<UINT_PTR>(source)) + L"/" +
			std::to_wstring(aaApi_GetCurrentUserId()) + L"/" + std::to_wstring(reinterpret_cast<UINT_PTR>(list)) + L"/" + std::to_wstring(folder);
		const ULONGLONG now = GetTickCount64();
		if (force || context != g_selectorContext || now - g_selectorsChecked >= 5000)
		{
			QuickAccess::SelectorStates fresh;
			for (auto& selector : fresh) selector.context = context;
			const LONG count = aaApi_SelectAllGuis();
			if (count < 0 || count > 1024) return false;
			for (LONG i = 0; i < count; ++i)
			{
				const LONG id = aaApi_GetGuiId(i);
				const LPCWSTR name = aaApi_GetGuiStringProperty(GUI_PROP_NAME, i);
				if (id <= 0 || name == nullptr || *name == L'\0') return false;
				fresh[0].choices.push_back({static_cast<UINT_PTR>(id), name});
			}
			// Copy all interface properties before making any other SDK calls that could replace the static GUI buffer.
			for (HAAVIEW view = aaApi_ViewGetFirstForProject(std::max(0L, folder)); view != nullptr; view = aaApi_ViewGetNext(view))
			{
				if (fresh[1].choices.size() >= 1024) return false;
				const UINT_PTR id = reinterpret_cast<UINT_PTR>(view);
				if (std::any_of(fresh[1].choices.begin(), fresh[1].choices.end(), [id](const auto& item) { return item.id == id; })) return false;
				const LPCWSTR name = aaApi_ViewGetName(view);
				if (name == nullptr || *name == L'\0') return false;
				fresh[1].choices.push_back({id, name});
			}
			g_selectorCache = std::move(fresh);
			g_selectorContext = context;
			g_selectorsChecked = now;
		}
		result = g_selectorCache;
		const LONG activeInterface = aaApi_GetActiveInterface(); // Classic's datasource-level attribute interface.
		const HAAVIEW activeView = aaApi_DocListGetViewHandle(list);
		for (std::size_t i = 0; i < result.size(); ++i)
		{
			auto& selector = result[i];
			selector.selected = -1;
			const UINT_PTR active = i == 0 ? static_cast<UINT_PTR>(activeInterface) : reinterpret_cast<UINT_PTR>(activeView);
			for (std::size_t j = 0; j < selector.choices.size(); ++j)
				if (selector.choices[j].id == active) selector.selected = static_cast<int>(j);
			selector.enabled = !selector.choices.empty() && (i != 0 || aaApi_CanChangeActiveInterface() != FALSE);
		}
		return source == aaApi_GetActiveDatasource() && list == NavigationList() && folder == aaApi_DocListGetProject(list);
	}

	bool RefreshSelectorsImpl(QuickAccess::SelectorStates& selectors, bool force)
	{
		try { return BuildSelectors(selectors, force); }
		catch (...) { Report(L"could not read Interface and View choices"); return false; }
	}

	bool RefreshSelectors(void*, QuickAccess::SelectorStates& selectors, bool force)
	{
		__try { return IsWindowEnabled(g_frame) && RefreshSelectorsImpl(selectors, force); }
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { Report(L"SDK selector dependency or export unavailable"); return false; }
	}

	bool SelectChoiceImpl(QuickAccess::Selector selector, const QuickAccess::SelectorState& shown, std::size_t choice)
	{
		try
		{
			const auto index = static_cast<std::size_t>(selector);
			QuickAccess::SelectorStates current;
			if (index >= current.size() || !BuildSelectors(current, true) || !QuickAccess::CanSelectChoice(shown, current[index], choice)) return false;
			const auto& item = shown.choices[choice];
			bool changed = false;
			if (selector == QuickAccess::Selector::Interface)
			{
				if (item.id > static_cast<UINT_PTR>(MAXLONG) || !aaApi_CanChangeActiveInterface() || !aaApi_SetActiveInterface(static_cast<LONG>(item.id))) return false;
				aaApi_SendInterfaceChangedNotify(); // Keep Classic's existing notification receiver and selector in sync.
				aaApi_UpdateDocumentWindows();
				changed = aaApi_GetActiveInterface() == static_cast<LONG>(item.id);
			}
			else
			{
				const HWND list = NavigationList();
				const auto view = reinterpret_cast<HAAVIEW>(item.id);
				if (!ExplorerControl(list) || !aaApi_ViewIsValidViewHandle(view) || !aaApi_DocListSetView(list, view)) return false;
				if (!aaApi_ViewSetLastViewName(item.label.c_str())) Report(L"view applied but its last-used name could not be updated");
				aaApi_SendViewSettingsNotify();
				changed = aaApi_DocListGetViewHandle(list) == view;
			}
			g_selectorsChecked = 0;
			return changed;
		}
		catch (...) { Report(L"could not apply Interface or View selection"); return false; }
	}

	bool SelectChoice(void*, QuickAccess::Selector selector, const QuickAccess::SelectorState& shown, std::size_t choice)
	{
		__try { return IsWindowEnabled(g_frame) && SelectChoiceImpl(selector, shown, choice); }
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { Report(L"SDK selector command dependency or export unavailable"); return false; }
	}
	struct WorkflowSelection
	{
		HWND list = nullptr;
		HWND control = nullptr;
		LONG folder = 0;
		LONG workflow = 0;
		HDSOURCE source = nullptr;
		std::wstring key;
		std::vector<AADOC_ITEM> documents;
	};

	bool BuildWorkflow(QuickAccess::WorkflowState& result, WorkflowSelection& selection, bool force)
	{
		result = {};
		selection.list = NavigationList();
		if (!ExplorerControl(selection.list)) return false;
		selection.source = aaApi_GetActiveDatasource();
		selection.folder = aaApi_DocListGetProject(selection.list);
		if (selection.source == nullptr || selection.folder <= 0) return true;
		selection.control = QuickAccess::SelectionControl(g_frame, aaApi_GetMainDscTree(), selection.list, g_selectionControl);
		if (selection.control == nullptr) return false;
		const ULONGLONG now = GetTickCount64();
		if (force || selection.source != g_workflowSource || selection.folder != g_workflowFolder || now - g_workflowChecked >= 5000)
		{
			OwnedBuffer project{aaApi_SelectProjectDataBuffer(selection.folder)};
			if (project.value == nullptr || aaApi_DmsDataBufferGetCount(project.value) != 1) return false;
			g_workflowId = aaApi_DmsDataBufferGetNumericProperty(project.value, PROJ_PROP_WORKFLOWID, 0);
			g_workflowSource = selection.source;
			g_workflowFolder = selection.folder;
			g_workflowChecked = now;
		}
		selection.workflow = g_workflowId;
		if (selection.workflow <= 0) return true;
		const HWND listControl = aaApi_GetDocumentListCtrl(selection.list);
		if (!IsWindow(listControl)) return false;
		std::vector<std::wstring> identities;
		for (int index = ListView_GetNextItem(listControl, -1, LVNI_SELECTED); index != -1; index = ListView_GetNextItem(listControl, index, LVNI_SELECTED))
		{
			AADOCLIST_ITEM item{};
			item.iItem = index;
			item.ulMask = AADOCITEM_PROJECTNO | AADOCITEM_DOCNO | AADOCITEM_FLAGS | AADOCITEM_VERSIONSEQ;
			if (!aaApi_DocListGetItem(selection.list, &item)) return false;
			// A mixed folder/document/non-workflow selection must not operate on an undisclosed subset.
			if (item.lDocumentId <= 0 || item.lProjectId != selection.folder || (item.ulFlags & AADOCIF_WORKFL) == 0) return true;
			selection.documents.push_back({item.lProjectId, item.lDocumentId});
			identities.push_back(std::to_wstring(item.lProjectId) + L":" + std::to_wstring(item.lDocumentId) + L":" +
				std::to_wstring(item.versionSeq) + L":" + std::to_wstring(item.ulFlags));
		}
		std::sort(identities.begin(), identities.end());
		selection.key = std::to_wstring(reinterpret_cast<UINT_PTR>(selection.source)) + L"/" + std::to_wstring(selection.folder) +
			L"/" + std::to_wstring(selection.workflow) + L"/" + std::to_wstring(reinterpret_cast<UINT_PTR>(selection.control)) + L"/" + std::to_wstring(aaApi_GetCurrentUserId());
		for (const auto& identity : identities) selection.key += L"/" + identity;
		result.visible = true;
		result.context = selection.key;
		OwnedMenu menu{CreatePopupMenu()};
		if (menu.value == nullptr) return false;
		const HMENU documents = aaApi_InsertMenuByType(AAMENU_DOCUMENT, menu.value, 0);
		if (documents == nullptr || aaApi_InitMenuByAaControl(selection.list, documents,
			AAMG_MODIFY | AAMG_WORKFLOW | AAMG_CUSTOMS | AAMG_PROPERTIES | AAMG_VIEW) == 0) return selection.documents.empty();
		const QuickAccess::WorkflowMenuIds ids{IDMD_CHANGE_NEXT_STATE, IDMD_CHANGE_PREV_STATE, IDMD_STATE, IDMD_SET_FINAL, IDMD_REMOVE_FINAL};
		if (!QuickAccess::ReadWorkflowMenu(documents, ids, !selection.documents.empty() && selection.control == selection.list, result))
			return selection.documents.empty();
		// Preserve the SDK-initialised menu while its extension command IDs are displayed/used.
		std::swap(g_workflowMenu, menu.value);
		return true;
	}

	bool RefreshWorkflowImpl(QuickAccess::WorkflowState& result)
	{
		try
		{
			WorkflowSelection selection;
			return BuildWorkflow(result, selection, false);
		}
		catch (...) { Report(L"could not read the workflow selection and native menu"); return false; }
	}

	bool RefreshWorkflow(void*, QuickAccess::WorkflowState& result)
	{
		__try { return IsWindowEnabled(g_frame) && RefreshWorkflowImpl(result); }
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK workflow dependency or export unavailable");
			return false;
		}
	}

	bool ExecuteWorkflowImpl(const QuickAccess::WorkflowState& shown, UINT command)
	{
		try
		{
			WorkflowSelection selection;
			QuickAccess::WorkflowState current;
			if (!BuildWorkflow(current, selection, true) || !QuickAccess::CanInvokeWorkflow(shown, current, command) ||
				selection.documents.empty() || selection.documents.size() > static_cast<std::size_t>(MAXLONG)) return false;
			SetFocus(selection.list);
			if (GetFocus() != selection.list && !IsChild(selection.list, GetFocus())) return false;
			// Native document command dispatch preserves extension callbacks, WRE prompts and audit-comment dialogs.
			const LONG_PTR outcome = aaApi_ExecuteDocumentCommand(command, static_cast<LONG>(selection.documents.size()), selection.documents.data());
			g_workflowChecked = 0;
			return outcome == IDOK || outcome == IDCANCEL;
		}
		catch (...) { Report(L"could not dispatch the workflow command"); return false; }
	}

	bool ExecuteWorkflow(void*, const QuickAccess::WorkflowState& shown, UINT command)
	{
		__try { return IsWindowEnabled(g_frame) && ExecuteWorkflowImpl(shown, command); }
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			Report(L"SDK workflow command dependency or export unavailable");
			return false;
		}
	}

	bool BuildAdmin(QuickAccess::ContextState& result)
	{
		result = {};
		const HDSOURCE source = aaApi_GetActiveDatasource();
		if (source == nullptr) return true;
		const LONG user = aaApi_GetCurrentUserId();
		const bool administrator = aaApi_IsCurrentUserAdmin() != FALSE;
		const bool restricted = aaApi_IsUserRestrictedAdmin(0) != FALSE;
		const bool setup = aaApi_HasAdminSetup() != FALSE;
		const std::wstring context = std::to_wstring(reinterpret_cast<UINT_PTR>(source)) + L"/" + std::to_wstring(user) +
			L"/" + std::to_wstring(administrator) + L"/" + std::to_wstring(restricted) + L"/" + std::to_wstring(setup);
		result = QuickAccess::AdminContext(context, administrator, restricted, setup);
		return source == aaApi_GetActiveDatasource() && user == aaApi_GetCurrentUserId();
	}
	bool RefreshAdminImpl(QuickAccess::ContextState& result)
	{
		try { return BuildAdmin(result); }
		catch (...) { Report(L"could not read administrative context"); return false; }
	}
	bool RefreshAdmin(void*, QuickAccess::ContextState& result)
	{
		__try { return IsWindowEnabled(g_frame) && RefreshAdminImpl(result); }
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { return false; }
	}
	bool CreateUserImpl(const QuickAccess::ContextState& shown, const QuickAccess::NewUser& user, std::wstring& error)
	{
		try
		{
			QuickAccess::ContextState current;
			if (!QuickAccess::ValidateNewUser(user, error)) return false;
			if (!BuildAdmin(current) || !QuickAccess::CanInvokeWorkflow(shown, current, static_cast<UINT>(QuickAccess::AdminCommand::NewUser)) || !aaApi_HasAdminSetup())
			{ error = L"The datasource or administrative session changed. Reopen New User."; return false; }
			LONG id = 0;
			// The server remains authoritative for Restricted Administrator Users-node permissions and uniqueness.
			if (!aaApi_CreateUser3(&id, user.name.c_str(), user.description.c_str(), user.email.c_str()))
			{
				wchar_t text[180]{};
				swprintf_s(text, L"ProjectWise rejected user creation (error %ld). Check Users-node permissions and the account name.", aaApi_GetLastErrorId());
				error = text; return false;
			}
			return true;
		}
		catch (...) { error = L"The account could not be created."; return false; }
	}
	bool CreateUser(void*, const QuickAccess::ContextState& shown, const QuickAccess::NewUser& user, std::wstring& error)
	{
		__try { return CreateUserImpl(shown, user, error); }
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { return false; }
	}
	bool ExecuteAdminImpl(const QuickAccess::ContextState& shown, UINT command)
	{
		try
		{
			QuickAccess::ContextState current;
			if (!BuildAdmin(current) || !QuickAccess::CanInvokeWorkflow(shown, current, command)) return false;
			wchar_t datasource[1024]{};
			if (!aaApi_GetActiveDatasourceName(datasource, ARRAYSIZE(datasource))) return false;
			if (command == static_cast<UINT>(QuickAccess::AdminCommand::NewUser))
			{
				const INT_PTR result = QuickAccess::ShowNewUserDialog(g_frame, g_instance, shown, datasource, nullptr, CreateUser);
				return result == IDOK || result == IDCANCEL;
			}
			if (command == static_cast<UINT>(QuickAccess::AdminCommand::Users))
			{
				const INT_PTR result = aaApi_UserManagementDlg(g_frame, nullptr);
				return result == IDOK || result == IDCANCEL;
			}
			if (command == static_cast<UINT>(QuickAccess::AdminCommand::SignIn))
			{
				// An explicit native login prompts for credentials. Never silently promote a normal Explorer session.
				const LONG_PTR result = aaApi_LoginDlgExt(g_frame, L"Administrative sign-in", AALOGIN_ADMINISTRATIVE | AALOGIN_REUSE_SESSION |
					AALOGIN_NO_DS_SELECT | AALOGIN_NO_AUTOLOGIN, datasource, ARRAYSIZE(datasource), nullptr, nullptr, nullptr);
				if (result == IDOK) { g_workflowChecked = 0; g_selectorsChecked = 0; aaApi_UpdateProjectWindows(); aaApi_UpdateDocumentWindows(); }
				return result == IDOK || result == IDCANCEL;
			}
			return false;
		}
		catch (...) { Report(L"administrative action failed"); return false; }
	}
	bool ExecuteAdmin(void*, const QuickAccess::ContextState& shown, UINT command)
	{
		__try { return IsWindowEnabled(g_frame) && ExecuteAdminImpl(shown, command); }
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { return false; }
	}

	bool RecoverClassic(const wchar_t* reason, bool interactive = false)
	{
		g_classicPreferred = true; g_ribbonExpected = false;
		QuickAccess::SetVersionStacksAvailable(false);
		if (!QuickAccess::SaveClassicUiPreference(true)) Report(L"could not persist Classic UI preference");
		if (g_startupTimer != 0) { KillTimer(nullptr, g_startupTimer); g_startupTimer = 0; }
		if (IsWindow(g_window)) { QuickAccess::Dock(g_window, false); DestroyWindow(g_window); }
		g_window = nullptr;
		Report(reason);
		g_restorePending = IsWindow(g_frame) && !IsWindowEnabled(g_frame);
		if (g_restorePending) { Report(L"native toolbar restoration deferred until the modal dialog closes"); return true; }
		const auto restored = QuickAccess::RestoreClassicToolbars(g_frame);
		if (interactive && (restored.failed != 0 || restored.found == 0))
			MessageBoxW(g_frame, L"The ribbon is stopped. Some native toolbars could not be verified automatically; use View > Toolbar to choose any missing toolbars.",
				L"Classic UI", MB_OK | MB_ICONINFORMATION);
		return IsWindow(g_frame);
	}
	bool RestoreClassic(void*) { return RecoverClassic(L"Classic UI selected", true); }
	void RibbonFailed(void*, const wchar_t* reason) { RecoverClassic(reason); }
	void CALLBACK CheckRibbon(HWND, UINT, UINT_PTR, DWORD)
	{
		if (g_restorePending && g_classicPreferred && IsWindowEnabled(g_frame))
		{
			g_restorePending = false;
			QuickAccess::RestoreClassicToolbars(g_frame);
		}
		if (g_ribbonExpected && !g_classicPreferred && IsWindowEnabled(g_frame) && !QuickAccess::IsHealthy(g_window))
			RecoverClassic(L"ribbon health check failed; Classic UI restored");
	}

	bool EnsureWindow()
	{
		if (IsWindow(g_window)) return true;
		g_frame = aaApi_GetMainFrameWindow();
		if (!IsWindow(g_frame) || GetWindowThreadProcessId(g_frame, nullptr) != GetCurrentThreadId()) return false;
		g_window = QuickAccess::Create(g_frame, g_instance, {nullptr, RefreshGuarded, Execute, Search, CreateAddress, RefreshNavigation, Navigate,
			[](void*) -> bool {
				const HWND target = QuickAccess::SelectionControl(g_frame, aaApi_GetMainDscTree(), NavigationList(), g_selectionControl);
				if (target == nullptr) { Report(L"no Explorer selection control is available for keyboard focus"); return false; }
				SetFocus(target);
				return GetFocus() == target || IsChild(target, GetFocus());
			}, RefreshAddress, RefreshWorkflow, ExecuteWorkflow, RefreshSelectors, SelectChoice, RefreshAdmin, ExecuteAdmin, RestoreClassic, RibbonFailed});
		g_creationFailed = g_window == nullptr;
		if (g_window != nullptr && !QuickAccess::Dock(g_window, true))
		{
			DestroyWindow(g_window); g_window = nullptr;
			Report(L"Explorer's content pane is not ready for docking");
		}
		g_ribbonExpected = g_window != nullptr;
		if (g_ribbonExpected) QuickAccess::SetVersionStacksAvailable(true);
		return g_window != nullptr;
	}

	LONG ToggleImpl(LPAACMDPARAM command)
	{
		if (command == nullptr || (static_cast<UINT>(command->lCommandId) != g_toggleCommand &&
			static_cast<UINT>(command->lCommandId) != g_viewCommand)) return -1;
		g_classicPreferred = false;
		g_restorePending = false;
		QuickAccess::SaveClassicUiPreference(false);
		if (!EnsureWindow())
		{
			RecoverClassic(L"could not open ribbon; Classic UI restored", true);
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
			RecoverClassic(L"SDK dependency unavailable; Classic UI restored");
			return -1;
		}
	}

	LONG CALLBACK ToggleState(LPAACMDSTATE)
	{
		return IsWindow(g_window) && IsWindowVisible(g_window) &&
			QuickAccess::GetDisplayMode(g_window) != QuickAccess::DisplayMode::Hidden ? AAMS_CHECKED : 0;
	}
	LONG CALLBACK ClassicCommand(LPAACMDPARAM command)
	{
		if (command == nullptr || static_cast<UINT>(command->lCommandId) != g_classicCommand) return -1;
		__try
		{
			if (!IsWindow(g_frame)) g_frame = aaApi_GetMainFrameWindow();
			return RecoverClassic(L"Classic UI selected from Tools", true) ? 0 : -1;
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode())) { RecoverClassic(L"Classic UI retained; SDK unavailable"); return -1; }
	}
	LONG CALLBACK ClassicState(LPAACMDSTATE) { return g_classicPreferred ? AAMS_CHECKED : 0; }

	void CALLBACK Startup(HWND, UINT, UINT_PTR timer, DWORD)
	{
		__try
		{
			if (g_classicPreferred)
			{
				g_frame = aaApi_GetMainFrameWindow();
				if (IsWindow(g_frame)) { RecoverClassic(L"Classic UI preference restored"); return; }
				if (++g_attempts >= 120) { KillTimer(nullptr, timer); g_startupTimer = 0; }
			}
			else if (EnsureWindow())
			{
				KillTimer(nullptr, timer);
				g_startupTimer = 0;
				ShowWindow(g_window, SW_SHOWNOACTIVATE);
			}
			else if (g_creationFailed || ++g_attempts >= 120)
			{
				KillTimer(nullptr, timer);
				g_startupTimer = 0;
				RecoverClassic(L"ribbon startup failed; Classic UI restored");
			}
		}
		__except (SdkLoadExceptionFilter(GetExceptionCode()))
		{
			KillTimer(nullptr, timer);
			g_startupTimer = 0;
			RecoverClassic(L"SDK dependency unavailable during startup; Classic UI retained");
		}
	}

	bool AddVisibilityCommand(LONG type, const wchar_t* name, UINT& id, bool classic = false)
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
		item.fpExecute = classic ? ClassicCommand : Toggle;
		item.fpGetState = classic ? ClassicState : ToggleState;
		if (aaApi_AddCustomMenuItem(0, AADMSMI_LAST, &item) == 0)
		{
			Report(L"could not register ribbon visibility menu command");
			return false;
		}
		return true;
	}

	bool Initialise()
	{
		g_classicPreferred = QuickAccess::PreferClassicUi();
		g_frame = aaApi_GetMainFrameWindow();
		// Register recovery before any ribbon window is created, so it remains available after startup failures.
		if (!AddVisibilityCommand(AAMENU_TOOLS, L"Re-enable Classic UI", g_classicCommand, true) ||
			!AddVisibilityCommand(AAMENU_TOOLS, L"Quick Access Toolbar", g_toggleCommand) ||
			!AddVisibilityCommand(AAMENU_VIEW, L"Ribbon", g_viewCommand)) { RecoverClassic(L"ribbon menu registration failed"); return false; }
		if (!QuickAccess::RegisterVersionStacks(g_instance, !g_classicPreferred))
			Report(L"optional Version stacks view unavailable; native document list retained");
		g_healthTimer = SetTimer(nullptr, 0, 1000, CheckRibbon);
		if (g_healthTimer == 0) { RecoverClassic(L"ribbon health monitoring unavailable"); return false; }
		g_startupTimer = SetTimer(nullptr, 0, 250, Startup);
		if (g_startupTimer == 0)
		{
			RecoverClassic(L"could not schedule ribbon startup");
			return false;
		}
		Report(L"module initialised");
		return true;
	}
	bool InitialiseGuarded()
	{
		try { return Initialise(); }
		catch (...) { RecoverClassic(L"ribbon initialization failed; Classic UI retained"); return false; }
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
		InitialiseGuarded();
	}
	__except (SdkLoadExceptionFilter(GetExceptionCode()))
	{
		RecoverClassic(L"SDK dependency or export unavailable; Classic UI retained");
	}
	return IDOK;
}
