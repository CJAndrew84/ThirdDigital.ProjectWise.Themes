#include "..\src\QuickAccess.h"
#include "..\src\Admin.h"
#include "..\src\AdminResources.h"
#include "..\src\ClassicUi.h"

#include <commctrl.h>
#include <oleacc.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

void CheckVersionStacks(const wchar_t* preview, void (*expect)(bool, const wchar_t*), bool (*save)(HWND, const wchar_t*));
void CheckNotifications(HWND owner, const wchar_t* preview, void (*expect)(bool, const wchar_t*), bool (*save)(HWND, const wchar_t*));

namespace
{
	int g_failures = 0;
	int g_checks = 0;

	void Expect(bool condition, const wchar_t* description)
	{
		++g_checks;
		if (!condition) ++g_failures;
		std::wprintf(L"%s: %s\n", condition ? L"PASS" : L"FAIL", description);
	}

	struct FakeHost
	{
		QuickAccess::States states{};
		bool available = true;
		bool executeSucceeds = true;
		unsigned executions = 0;
		std::size_t last = QuickAccess::kButtonCount;
		HWND focusOnExecute = nullptr;
		HWND nestedRepeatTarget = nullptr;
		bool nestedRepeatConsumed = false;
		HWND destroyOnRefresh = nullptr;
		unsigned searches = 0;
		std::wstring query;
		bool searchSucceeds = true;
		HWND destroyOnSearch = nullptr;
		bool navigationAvailable = true;
		bool addressAvailable = true;
		QuickAccess::NavigationStates navigationStates{{{true, false}, {true, false}, {true, false}}};
		unsigned navigations = 0;
		std::size_t lastNavigation = QuickAccess::kNavigationCount;
		HWND destroyOnNavigationRefresh = nullptr;
		HWND destroyOnNavigate = nullptr;
		HWND destroyOnAddressRefresh = nullptr;
		QuickAccess::WorkflowState workflow;
		bool workflowAvailable = true;
		unsigned workflowExecutions = 0;
		UINT lastWorkflowCommand = 0;
		HWND destroyOnWorkflowRefresh = nullptr;
		HWND destroyOnWorkflowExecute = nullptr;
		QuickAccess::SelectorStates selectors;
		bool selectorsAvailable = true;
		unsigned selections = 0, forcedSelectorQueries = 0;
		QuickAccess::Selector lastSelector = QuickAccess::Selector::Interface;
		UINT_PTR lastChoice = 0;
		HWND destroyOnSelectorsRefresh = nullptr, destroyOnSelect = nullptr;
		QuickAccess::ContextState admin;
		bool adminAvailable = true;
		unsigned adminExecutions = 0, recoveries = 0, classicSelections = 0;
		UINT lastAdmin = 0;
		QuickAccess::ContextState projectAdmin;
		unsigned projectAdminExecutions = 0;
		UINT lastProjectAdmin = 0;
		HWND destroyOnProjectAdminRefresh = nullptr;
		HWND recoveryWindow = nullptr, destroyOnAdminRefresh = nullptr;
		bool throwOnRefresh = false, throwOnExecute = false, throwOnCreateAddress = false;

		static bool Refresh(void* context, QuickAccess::States& result)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (host.throwOnRefresh) throw 1;
			result = host.states;
			if (host.destroyOnRefresh != nullptr)
			{
				HWND window = host.destroyOnRefresh;
				host.destroyOnRefresh = nullptr;
				DestroyWindow(window);
			}
			return host.available;
		}

		static bool Execute(void* context, std::size_t button)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (host.throwOnExecute) throw 1;
			++host.executions;
			host.last = button;
			if (button == QuickAccess::kShowAllVersions && host.executeSucceeds)
				host.states[button].checked = !host.states[button].checked;
			if (IsWindow(host.focusOnExecute)) SetFocus(host.focusOnExecute);
			if (IsWindow(host.nestedRepeatTarget))
			{
				PostMessageW(host.nestedRepeatTarget, WM_KEYDOWN, VK_RETURN, 1L << 30);
				MSG repeat{};
				host.nestedRepeatConsumed = PeekMessageW(&repeat, host.nestedRepeatTarget, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) && repeat.message == WM_NULL;
			}
			return host.executeSucceeds;
		}

		static bool Search(void* context, const wchar_t* text)
		{
			auto& host = *static_cast<FakeHost*>(context);
			++host.searches;
			host.query = text;
			if (host.destroyOnSearch != nullptr)
			{
				HWND window = host.destroyOnSearch;
				host.destroyOnSearch = nullptr;
				DestroyWindow(window);
			}
			return host.searchSucceeds;
		}

		static HWND CreateAddress(void* context, HWND parent)
		{
			if (static_cast<FakeHost*>(context)->throwOnCreateAddress) throw 1;
			return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
				0, 0, 400, 240, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
		}

		static bool RefreshNavigation(void* context, HWND, QuickAccess::NavigationStates& states)
		{
			auto& host = *static_cast<FakeHost*>(context);
			states = host.navigationStates;
			if (host.destroyOnNavigationRefresh != nullptr)
			{
				const HWND window = host.destroyOnNavigationRefresh;
				host.destroyOnNavigationRefresh = nullptr;
				DestroyWindow(window);
			}
			return host.navigationAvailable;
		}

		static bool Navigate(void* context, std::size_t button)
		{
			auto& host = *static_cast<FakeHost*>(context);
			++host.navigations;
			host.lastNavigation = button;
			if (host.destroyOnNavigate != nullptr)
			{
				const HWND window = host.destroyOnNavigate;
				host.destroyOnNavigate = nullptr;
				DestroyWindow(window);
			}
			return host.executeSucceeds;
		}

		static bool RefreshAddress(void* context, HWND)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (host.destroyOnAddressRefresh != nullptr)
			{
				const HWND window = host.destroyOnAddressRefresh;
				host.destroyOnAddressRefresh = nullptr;
				DestroyWindow(window);
			}
			return host.addressAvailable;
		}

		static bool RefreshWorkflow(void* context, QuickAccess::WorkflowState& result)
		{
			auto& host = *static_cast<FakeHost*>(context);
			result = host.workflow;
			if (host.destroyOnWorkflowRefresh != nullptr)
			{
				const HWND window = host.destroyOnWorkflowRefresh;
				host.destroyOnWorkflowRefresh = nullptr;
				DestroyWindow(window);
			}
			return host.workflowAvailable;
		}

		static bool ExecuteWorkflow(void* context, const QuickAccess::WorkflowState& shown, UINT command)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (!QuickAccess::CanInvokeWorkflow(shown, host.workflow, command)) return false;
			++host.workflowExecutions;
			host.lastWorkflowCommand = command;
			if (host.destroyOnWorkflowExecute != nullptr)
			{
				const HWND window = host.destroyOnWorkflowExecute;
				host.destroyOnWorkflowExecute = nullptr;
				DestroyWindow(window);
			}
			return host.executeSucceeds;
		}

		static bool RefreshSelectors(void* context, QuickAccess::SelectorStates& result, bool force)
		{
			auto& host = *static_cast<FakeHost*>(context);
			result = host.selectors;
			if (force) ++host.forcedSelectorQueries;
			if (host.destroyOnSelectorsRefresh != nullptr)
			{
				const HWND window = host.destroyOnSelectorsRefresh;
				host.destroyOnSelectorsRefresh = nullptr;
				DestroyWindow(window);
			}
			return host.selectorsAvailable;
		}

		static bool SelectChoice(void* context, QuickAccess::Selector selector, const QuickAccess::SelectorState& shown, std::size_t choice)
		{
			auto& host = *static_cast<FakeHost*>(context);
			auto& state = host.selectors[static_cast<std::size_t>(selector)];
			if (!QuickAccess::CanSelectChoice(shown, state, choice)) return false;
			++host.selections;
			host.lastSelector = selector;
			host.lastChoice = shown.choices[choice].id;
			if (host.executeSucceeds)
				for (std::size_t i = 0; i < state.choices.size(); ++i) if (state.choices[i].id == host.lastChoice) state.selected = static_cast<int>(i);
			if (host.destroyOnSelect != nullptr)
			{
				const HWND window = host.destroyOnSelect;
				host.destroyOnSelect = nullptr;
				DestroyWindow(window);
			}
			return host.executeSucceeds;
		}
		static bool RefreshAdmin(void* context, QuickAccess::ContextState& result)
		{
			auto& host = *static_cast<FakeHost*>(context); result = host.admin;
			if (host.destroyOnAdminRefresh != nullptr) { HWND window = host.destroyOnAdminRefresh; host.destroyOnAdminRefresh = nullptr; DestroyWindow(window); }
			return host.adminAvailable;
		}
		static bool ExecuteAdmin(void* context, const QuickAccess::ContextState& shown, UINT command)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (!QuickAccess::CanInvokeWorkflow(shown, host.admin, command)) return false;
			++host.adminExecutions; host.lastAdmin = command; return true;
		}
		static bool RefreshProjectAdmin(void* context, QuickAccess::ContextState& result)
		{
			auto& host = *static_cast<FakeHost*>(context); result = host.projectAdmin;
			if (host.destroyOnProjectAdminRefresh != nullptr)
			{ HWND window = host.destroyOnProjectAdminRefresh; host.destroyOnProjectAdminRefresh = nullptr; DestroyWindow(window); }
			return true;
		}
		static bool ExecuteProjectAdmin(void* context, const QuickAccess::ContextState& shown, UINT command)
		{
			auto& host = *static_cast<FakeHost*>(context);
			if (!QuickAccess::CanInvokeWorkflow(shown, host.projectAdmin, command)) return false;
			++host.projectAdminExecutions; host.lastProjectAdmin = command; return true;
		}
		static bool RestoreClassic(void* context)
		{
			auto& host = *static_cast<FakeHost*>(context); ++host.classicSelections; DestroyWindow(host.recoveryWindow); return true;
		}
		static void Failed(void* context, const wchar_t*)
		{
			auto& host = *static_cast<FakeHost*>(context); ++host.recoveries; DestroyWindow(host.recoveryWindow);
		}
	};

	void CheckWorkflowMenus()
	{
		using Kind = QuickAccess::WorkflowActionKind;
		const QuickAccess::WorkflowMenuIds ids{501, 502, 503, 504, 505};
		HMENU root = CreatePopupMenu(), documents = CreatePopupMenu(), state = CreatePopupMenu();
		AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(documents), L"Documents");
		AppendMenuW(documents, MF_STRING, 900, L"Unrelated action");
		AppendMenuW(documents, MF_POPUP, reinterpret_cast<UINT_PTR>(state), L"Changer l'état");
		AppendMenuW(state, MF_STRING, ids.change, L"&Change...\tCtrl+G");
		AppendMenuW(state, MF_STRING | MF_GRAYED, ids.previous, L"&Previous");
		AppendMenuW(state, MF_STRING, ids.next, L"&Next");
		AppendMenuW(state, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(state, MF_STRING, ids.setFinal, L"Set Final Status");
		AppendMenuW(state, MF_STRING, ids.removeFinal, L"Remove Final Status");
		QuickAccess::WorkflowState result{true, false, L"source/folder/documents", {}};
		Expect(QuickAccess::ReadWorkflowMenu(root, ids, true, result) && result.actions.size() == 3 && !result.rulesEngine,
			L"standard workflow exposes exactly Next, Previous and Change from the localised state submenu");
		Expect(result.actions[0].command == ids.next && result.actions[1].command == ids.previous && result.actions[2].command == ids.change &&
			result.actions[0].label == L"Next" && result.actions[2].label == L"Change...", L"standard actions use requested order and clean native labels");
		Expect(result.actions[0].enabled && !result.actions[1].enabled && result.actions[2].enabled, L"standard actions preserve individual native permissions");
		EnableMenuItem(documents, 1, MF_BYPOSITION | MF_GRAYED);
		QuickAccess::ReadWorkflowMenu(root, ids, true, result);
		Expect(std::none_of(result.actions.begin(), result.actions.end(), [](const auto& a) { return a.enabled; }), L"disabled native parent disables every workflow child");
		EnableMenuItem(documents, 1, MF_BYPOSITION | MF_ENABLED);
		AppendMenuW(state, MF_STRING, 801, L"&Approuver && publier...\tCtrl+A");
		AppendMenuW(state, MF_STRING | MF_GRAYED, 802, L"Réviser...");
		Expect(QuickAccess::ReadWorkflowMenu(root, ids, true, result) && result.rulesEngine && result.actions.size() == 2,
			L"WRE operations replace the three standard transitions and exclude final-status and unrelated commands");
		Expect(result.actions[0].command == 801 && result.actions[0].label == L"Approuver & publier..." && result.actions[0].enabled &&
			result.actions[1].kind == Kind::Operation && !result.actions[1].enabled, L"WRE labels, order, literal ampersands and disabled state follow the native menu");
		const auto shown = result;
		Expect(QuickAccess::CanInvokeWorkflow(shown, result, 801) && !QuickAccess::CanInvokeWorkflow(shown, result, 802), L"only enabled displayed workflow operations can run");
		result.context = L"other source/folder/selection";
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801), L"a changed datasource, folder or document selection invalidates workflow actions");
		result = shown; result.actions[0].enabled = false;
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801), L"lost permissions invalidate a previously enabled workflow operation");
		result = shown; result.actions[0].label = L"A different operation";
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801), L"a reused native command ID cannot execute a relabelled operation");
		result = shown; result.actions.erase(result.actions.begin());
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801), L"a removed operation cannot execute from a stale button");
		result = shown; result.rulesEngine = false;
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801), L"switching WRE/standard mode invalidates stale workflow actions");
		result = shown; result.visible = false;
		Expect(!QuickAccess::CanInvokeWorkflow(shown, result, 801) && !QuickAccess::CanInvokeWorkflow(shown, shown, 999), L"hidden workflows and unknown command IDs cannot run");
		QuickAccess::ReadWorkflowMenu(root, ids, false, result);
		Expect(std::none_of(result.actions.begin(), result.actions.end(), [](const auto& a) { return a.enabled; }), L"no actionable document selection disables every WRE operation");
		AppendMenuW(state, MF_STRING, 801, L"Duplicate command");
		Expect(!QuickAccess::ReadWorkflowMenu(root, ids, true, result) && result.actions.empty(), L"ambiguous duplicate workflow IDs fail closed");
		DestroyMenu(root);
		Expect(!QuickAccess::ReadWorkflowMenu(nullptr, ids, true, result) && result.actions.empty(), L"missing workflow menus do not fabricate operations");
		root = CreatePopupMenu(); state = CreatePopupMenu();
		AppendMenuW(state, MF_STRING, 801, L"Server-side WRE approval");
		MENUITEMINFOW parent{sizeof(parent)}; parent.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
		parent.wID = ids.change; parent.hSubMenu = state; parent.dwTypeData = const_cast<LPWSTR>(L"Native change-state branch");
		InsertMenuItemW(root, 0, TRUE, &parent);
		Expect(QuickAccess::ReadWorkflowMenu(root, ids, true, result) && result.rulesEngine && result.actions.size() == 1 && result.actions[0].command == 801,
			L"WRE-only menus remain discoverable when every standard transition leaf is replaced");
		DestroyMenu(root);
	}

	void CheckNavigationAvailability()
	{
		struct Case { bool enabled; LONG count, position; bool parent; std::array<bool, 3> expected; const wchar_t* name; };
		const Case cases[] = {
			{false, 3, 1, true, {false, false, true}, L"Up is available even when document-list history is disabled"},
			{true, 0, -1, false, {false, false, false}, L"empty history at the root disables all navigation"},
			{true, 1, 0, true, {false, false, true}, L"one location has no Back or Forward history"},
			{true, 3, 0, true, {false, true, true}, L"first history entry allows Forward but not Back"},
			{true, 3, 1, true, {true, true, true}, L"middle history entry allows both directions"},
			{true, 3, 2, true, {true, false, true}, L"last history entry allows Back but not Forward"},
			{true, 3, 1, false, {true, true, false}, L"a root location disables Up independently of history"},
			{true, -1, 0, true, {false, false, true}, L"failed history-count query does not disable Up"},
			{true, 3, -1, true, {false, false, true}, L"failed position query does not guess history availability"},
			{true, 3, 3, true, {false, false, true}, L"out-of-range history position blocks Back and Forward"},
			{true, MAXLONG, MAXLONG - 1, true, {true, false, true}, L"history bounds do not overflow at the maximum LONG value"}
		};
		for (const auto& test : cases)
		{
			const auto states = QuickAccess::NavigationAvailability(test.enabled, test.count, test.position, test.parent);
			Expect(states[0].enabled == test.expected[0] && states[1].enabled == test.expected[1] && states[2].enabled == test.expected[2], test.name);
		}
	}

	void CheckMenuStates()
	{
		HMENU menu = CreatePopupMenu();
		HMENU nested = CreatePopupMenu();
		AppendMenuW(menu, MF_STRING, 101, L"Enabled");
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(nested), L"Nested");
		AppendMenuW(nested, MF_STRING | MF_GRAYED, 102, L"Disabled");
		AppendMenuW(nested, MF_STRING | MF_CHECKED, 103, L"Checked");
		QuickAccess::ButtonState state{};
		Expect(QuickAccess::ReadMenuState(menu, 101, state) && state.enabled && !state.checked, L"enabled menu command");
		Expect(QuickAccess::ReadMenuState(menu, 102, state) && !state.enabled, L"disabled nested menu command");
		Expect(QuickAccess::ReadMenuState(menu, 103, state) && state.enabled && state.checked, L"checked nested menu command");
		Expect(!QuickAccess::ReadMenuState(menu, 104, state) && !state.enabled && !state.checked, L"missing commands reset state to disabled");
		EnableMenuItem(menu, 1, MF_BYPOSITION | MF_GRAYED);
		Expect(QuickAccess::ReadMenuState(menu, 103, state) && !state.enabled, L"disabled parent disables its nested command");
		DestroyMenu(menu);
	}

	RECT RelativeRect(HWND child, HWND owner)
	{
		RECT rect{};
		GetWindowRect(child, &rect);
		MapWindowPoints(nullptr, owner, reinterpret_cast<POINT*>(&rect), 2);
		return rect;
	}

	HWND CommandPage(HWND window)
	{
		return GetDlgItem(FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr), QuickAccess::kCommandPage);
	}

	HWND Toolbar(HWND window)
	{
		return FindWindowExW(CommandPage(window), nullptr, TOOLBARCLASSNAMEW, nullptr);
	}

	bool QueuedKey(HWND target, UINT key, UINT message = WM_KEYDOWN, LPARAM flags = 0)
	{
		PostMessageW(target, message, key, flags);
		MSG queued{};
		if (!PeekMessageW(&queued, target, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) return false;
		const bool handled = queued.message == WM_NULL;
		if (!handled) { TranslateMessage(&queued); DispatchMessageW(&queued); }
		return handled;
	}

	void PaintCommand(HWND window, HWND toolbar, HDC dc, UINT command, RECT rect)
	{
		NMCUSTOMDRAW draw{};
		draw.hdr = {toolbar, 0, NM_CUSTOMDRAW};
		draw.dwDrawStage = CDDS_ITEMPREPAINT;
		draw.hdc = dc;
		draw.rc = rect;
		draw.dwItemSpec = command;
		SendMessageW(window, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&draw));
	}

	void CheckCommandIcons(HWND window, HWND toolbar)
	{
		const int dpi = static_cast<int>(GetDpiForWindow(window));
		const int width = MulDiv(112, dpi, 96), height = MulDiv(64, dpi, 96), icon = MulDiv(32, dpi, 96);
		HDC screen = GetDC(toolbar), dc = CreateCompatibleDC(screen);
		HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
		ReleaseDC(toolbar, screen);
		Expect(dc != nullptr && bitmap != nullptr, L"command icon measurement surface created");
		if (dc != nullptr && bitmap != nullptr)
		{
			const auto old = SelectObject(dc, bitmap);
			const COLORREF background = GetSysColor(COLOR_WINDOW);
			std::vector<std::vector<bool>> silhouettes;
			for (int i = 0; i < static_cast<int>(QuickAccess::kButtonCount); ++i)
			{
				SetDCBrushColor(dc, background);
				RECT rect{0, 0, width, height};
				FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
				PaintCommand(window, toolbar, dc, QuickAccess::kFirstButton + i, rect);
				std::vector<bool> pixels;
				int ink = 0, lowerInk = 0;
				for (int y = 0; y < icon; ++y) for (int x = 0; x < icon; ++x)
				{
					const COLORREF pixel = GetPixel(dc, (width - icon) / 2 + x, MulDiv(6, dpi, 96) + y);
					const bool drawn = pixel != CLR_INVALID && pixel != background;
					pixels.push_back(drawn);
					if (drawn) { ++ink; if (y >= icon * 2 / 3) ++lowerInk; }
				}
				Expect(ink >= icon * 2, L"each custom-painted command has a substantial, visible icon silhouette");
				if (i == 6) Expect(lowerInk > icon / 2, L"Refresh draws a complete circular pair of arrows, including the lower sweep");
				for (const auto& previous : silhouettes) Expect(pixels != previous, L"different commands have distinct icon silhouettes");
				silhouettes.push_back(std::move(pixels));
			}
			SelectObject(dc, old);
		}
		if (bitmap != nullptr) DeleteObject(bitmap);
		if (dc != nullptr) DeleteDC(dc);
	}

	bool SaveCommandPreview(HWND window, HWND toolbar, const wchar_t* path)
	{
		const int dpi = static_cast<int>(GetDpiForWindow(window));
		const int cellWidth = MulDiv(160, dpi, 96), cellHeight = MulDiv(64, dpi, 96), header = MulDiv(32, dpi, 96);
		const int width = cellWidth * 6, sectionHeight = header + cellHeight * 4, height = sectionHeight * 2;
		BITMAPINFO info{};
		info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
		void* pixels = nullptr;
		HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		HDC dc = CreateCompatibleDC(nullptr);
		if (dc == nullptr || bitmap == nullptr)
		{
			if (dc != nullptr) DeleteDC(dc);
			if (bitmap != nullptr) DeleteObject(bitmap);
			return false;
		}
		const auto old = SelectObject(dc, bitmap);
		RECT page{0, 0, width, height};
		SetDCBrushColor(dc, GetSysColor(COLOR_WINDOW));
		FillRect(dc, &page, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		SetBkMode(dc, TRANSPARENT);
		for (int section = 0; section < 2; ++section)
		{
			const wchar_t* title = section == 0 ? L"Enabled commands" : L"Disabled commands";
			TextOutW(dc, MulDiv(12, dpi, 96), section * sectionHeight + MulDiv(8, dpi, 96), title, lstrlenW(title));
			for (UINT i = 0; i < QuickAccess::kButtonCount; ++i)
			{
				const UINT command = QuickAccess::kFirstButton + i;
				const bool enabled = SendMessageW(toolbar, TB_ISBUTTONENABLED, command, 0) != 0;
				SendMessageW(toolbar, TB_ENABLEBUTTON, command, MAKELPARAM(section == 0, 0));
				const int left = static_cast<int>(i % 6) * cellWidth, top = section * sectionHeight + header + static_cast<int>(i / 6) * cellHeight;
				PaintCommand(window, toolbar, dc, command, {left, top, left + cellWidth, top + cellHeight});
				SendMessageW(toolbar, TB_ENABLEBUTTON, command, MAKELPARAM(enabled, 0));
			}
		}
		GdiFlush();
		const DWORD byteCount = static_cast<DWORD>(width * height * 4);
		BITMAPFILEHEADER fileHeader{};
		fileHeader.bfType = 0x4D42;
		fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(BITMAPINFOHEADER);
		fileHeader.bfSize = fileHeader.bfOffBits + byteCount;
		FILE* file = nullptr;
		bool result = _wfopen_s(&file, path, L"wb") == 0 && file != nullptr;
		if (result)
		{
			result = std::fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1 &&
				std::fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file) == 1 && std::fwrite(pixels, byteCount, 1, file) == 1;
			result = std::fclose(file) == 0 && result;
		}
		SelectObject(dc, old);
		DeleteObject(bitmap);
		DeleteDC(dc);
		return result;
	}

	std::wstring AccessibleName(HWND control)
	{
		IAccessible* accessible = nullptr;
		if (FAILED(AccessibleObjectFromWindow(control, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&accessible)))) return L"";
		VARIANT self{};
		self.vt = VT_I4;
		self.lVal = CHILDID_SELF;
		BSTR name = nullptr;
		const HRESULT result = accessible->get_accName(self, &name);
		std::wstring text = SUCCEEDED(result) && name != nullptr ? name : L"";
		SysFreeString(name);
		accessible->Release();
		return text;
	}

	bool SaveRibbonPreview(HWND window, const wchar_t* path)
	{
		RECT bounds{};
		GetWindowRect(window, &bounds);
		const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
		if (width <= 0 || height <= 0) return false;
		RECT client{0, 0, width, height};
		BITMAPINFO info{};
		info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
		void* pixels = nullptr;
		HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		HDC dc = CreateCompatibleDC(nullptr);
		if (bitmap == nullptr || dc == nullptr)
		{
			if (bitmap != nullptr) DeleteObject(bitmap);
			if (dc != nullptr) DeleteDC(dc);
			return false;
		}
		const auto old = SelectObject(dc, bitmap);
		SetDCBrushColor(dc, GetSysColor(COLOR_BTNFACE));
		FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
		GdiFlush();
		BITMAPFILEHEADER header{};
		header.bfType = 0x4D42;
		header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
		const DWORD bytes = static_cast<DWORD>(width * height * 4);
		header.bfSize = header.bfOffBits + bytes;
		FILE* file = nullptr;
		bool result = _wfopen_s(&file, path, L"wb") == 0 && file != nullptr;
		if (result)
		{
			result = std::fwrite(&header, sizeof(header), 1, file) == 1 && std::fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file) == 1 &&
				std::fwrite(pixels, bytes, 1, file) == 1;
			result = std::fclose(file) == 0 && result;
		}
		SelectObject(dc, old);
		DeleteObject(bitmap);
		DeleteDC(dc);
		return result;
	}

	void CheckVersionsToggle(HWND owner, const wchar_t* previewPath)
	{
		using namespace QuickAccess;
		FakeHost host;
		host.states[kShowAllVersions] = {true, false};
		HWND window = Create(owner, GetModuleHandleW(nullptr), {&host, FakeHost::Refresh, FakeHost::Execute});
		Expect(window != nullptr, L"version-display ribbon created");
		if (window == nullptr) return;
		HWND toolbar = Toolbar(window);
		const UINT command = kFirstButton + static_cast<UINT>(kShowAllVersions);
		Expect(ButtonTab(kShowAllVersions) == Tab::View && SelectTab(window, Tab::View) &&
			!SendMessageW(toolbar, TB_ISBUTTONHIDDEN, command, 0), L"Show all versions belongs to View");
		TBBUTTON button{};
		SendMessageW(toolbar, TB_GETBUTTON, kShowAllVersions, reinterpret_cast<LPARAM>(&button));
		Expect((button.fsStyle & BTNS_CHECK) != 0, L"Show all versions exposes a native toggle-button style");
		RECT item{};
		SendMessageW(toolbar, TB_GETITEMRECT, kShowAllVersions, reinterpret_cast<LPARAM>(&item));
		Expect(item.right - item.left >= MulDiv(160, static_cast<int>(GetDpiForWindow(window)), 96),
			L"version-display button leaves room for its complete label");
		Expect(!SendMessageW(toolbar, TB_ISBUTTONCHECKED, command, 0), L"latest-only host setting leaves the toggle unchecked");
		ShowWindow(owner, SW_SHOWNOACTIVATE);
		ShowWindow(window, SW_SHOWNOACTIVATE);
		SetFocus(toolbar);
		QueuedKey(toolbar, VK_END);
		Expect(SendMessageW(toolbar, TB_GETHOTITEM, 0, 0) == static_cast<LRESULT>(kShowAllVersions),
			L"keyboard End reaches Show all versions");
		Expect(QueuedKey(toolbar, VK_RETURN) && host.executions == 1 && host.last == kShowAllVersions &&
			SendMessageW(toolbar, TB_ISBUTTONCHECKED, command, 0), L"Enter toggles all versions on once and reflects the host state");
		QueuedKey(toolbar, VK_RETURN, WM_KEYUP);
		IAccessible* accessible = nullptr;
		bool named = false, pressed = false;
		if (SUCCEEDED(AccessibleObjectFromWindow(toolbar, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&accessible))))
		{
			VARIANT child{};
			child.vt = VT_I4;
			child.lVal = static_cast<LONG>(kShowAllVersions) + 1;
			BSTR name = nullptr;
			named = SUCCEEDED(accessible->get_accName(child, &name)) && name != nullptr && wcscmp(name, L"Show all versions") == 0;
			SysFreeString(name);
			VARIANT state{};
			pressed = SUCCEEDED(accessible->get_accState(child, &state)) && state.vt == VT_I4 &&
				(state.lVal & (STATE_SYSTEM_CHECKED | STATE_SYSTEM_PRESSED)) != 0;
			VariantClear(&state);
			accessible->Release();
		}
		Expect(named && pressed, L"native accessibility exposes the complete version-toggle name and its on state");
		if (previewPath != nullptr) Expect(SaveRibbonPreview(window, previewPath), L"native View version-toggle preview written");
		Expect(QueuedKey(toolbar, VK_SPACE) && host.executions == 2 && !SendMessageW(toolbar, TB_ISBUTTONCHECKED, command, 0),
			L"Space toggles all versions off once");
		QueuedKey(toolbar, VK_SPACE, WM_KEYUP);
		host.states[kShowAllVersions].checked = true;
		Refresh(window);
		Expect(SendMessageW(toolbar, TB_ISBUTTONCHECKED, command, 0) != 0, L"an external native setting change updates the toggle");
		host.executeSucceeds = false;
		Expect(!Invoke(window, command) && SendMessageW(toolbar, TB_ISBUTTONCHECKED, command, 0),
			L"a rejected setting change preserves the host's checked state");
		host.states[kShowAllVersions].enabled = false;
		const unsigned before = host.executions;
		Expect(!Invoke(window, command) && host.executions == before && !SendMessageW(toolbar, TB_ISBUTTONENABLED, command, 0),
			L"unavailable version settings cannot be dispatched");
		host.states[kShowAllVersions].enabled = true;
		host.executeSucceeds = true;
		SelectTab(window, Tab::Home);
		Expect(!Invoke(window, command) && host.executions == before, L"Home cannot dispatch View's version toggle");
		DestroyWindow(window);
	}

	void CheckSelectors(HWND owner, const wchar_t* previewPath)
	{
		using namespace QuickAccess;
		FakeHost host;
		for (auto& item : host.states) item = {true, false};
		host.selectors = {{{L"source/list/folder", {{11, L"Standard"}, {12, L"Engineering"}}, 0, true},
			{L"source/list/folder", {{101, L"Default"}, {102, L"Document control"}, {103, L"Delivery"}}, 1, true}}};
		const auto original = host.selectors;
		auto changed = original[0];
		Expect(CanSelectChoice(original[0], changed, 1), L"current selector choices can be committed by identity");
		std::reverse(changed.choices.begin(), changed.choices.end());
		Expect(CanSelectChoice(original[0], changed, 1), L"reordering native choices preserves identity-based dispatch");
		changed = original[0]; changed.context = L"another datasource or folder";
		Expect(!CanSelectChoice(original[0], changed, 1), L"a selector cannot apply to a different datasource, list or folder");
		changed = original[0]; changed.enabled = false;
		Expect(!CanSelectChoice(original[0], changed, 1), L"newly denied selector changes are rejected");
		changed = original[0]; changed.choices[1].label = L"Reused ID";
		Expect(!CanSelectChoice(original[0], changed, 1), L"renamed or reused native choices are rejected");
		changed.choices.pop_back();
		Expect(!CanSelectChoice(original[0], changed, 1), L"removed selector choices are rejected");
		changed = original[0]; changed.choices.push_back(changed.choices[0]);
		Expect(!CanSelectChoice(original[0], changed, 1) && !CanSelectChoice(original[0], original[0], 900), L"duplicate and invalid selector identities fail closed");
		const Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute, nullptr, FakeHost::CreateAddress, FakeHost::RefreshNavigation,
			FakeHost::Navigate, nullptr, FakeHost::RefreshAddress, nullptr, nullptr, FakeHost::RefreshSelectors, FakeHost::SelectChoice};
		HWND window = Create(owner, GetModuleHandleW(nullptr), callbacks);
		Expect(window != nullptr && SelectTab(window, Tab::View), L"View ribbon with native selectors created");
		if (window == nullptr) return;
		ShowWindow(owner, SW_SHOWNOACTIVATE); ShowWindow(window, SW_SHOWNOACTIVATE);
		HWND page = CommandPage(window), toolbar = Toolbar(window);
		HWND interfaceCombo = GetDlgItem(page, kFirstSelector), viewCombo = GetDlgItem(page, kFirstSelector + 1);
		Expect(interfaceCombo != nullptr && viewCombo != nullptr && IsWindowVisible(interfaceCombo) && IsWindowVisible(viewCombo), L"Interface and View are visible native dropdown selectors");
		Expect((GetWindowLongW(interfaceCombo, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST &&
			AccessibleName(interfaceCombo) == L"Interface" && AccessibleName(viewCombo) == L"View", L"selectors expose their complete native accessible names");
		Expect(SendMessageW(interfaceCombo, CB_GETCOUNT, 0, 0) == 2 && SendMessageW(interfaceCombo, CB_GETCURSEL, 0, 0) == 0 &&
			SendMessageW(viewCombo, CB_GETCOUNT, 0, 0) == 3 && SendMessageW(viewCombo, CB_GETCURSEL, 0, 0) == 1, L"selector options and current values match the host");
		Expect(SendMessageW(toolbar, TB_ISBUTTONHIDDEN, kFirstButton + kInterfaceDialog, 0) &&
			!Invoke(window, kFirstButton + static_cast<UINT>(kInterfaceDialog)), L"the dropdown replaces the Interface dialog button when supported");
		if (previewPath != nullptr) Expect(SaveRibbonPreview(window, previewPath), L"native Interface and View selector preview written");
		SetFocus(interfaceCombo);
		QueuedKey(interfaceCombo, VK_F4);
		Expect(SendMessageW(interfaceCombo, CB_GETDROPPEDSTATE, 0, 0) != 0, L"native F4 opens the Interface list");
		QueuedKey(interfaceCombo, VK_DOWN);
		Expect(host.selections == 0, L"browsing an open dropdown does not apply an interface");
		Expect(QueuedKey(interfaceCombo, VK_RETURN) && host.selections == 1 && host.lastChoice == 12 &&
			SendMessageW(interfaceCombo, CB_GETCURSEL, 0, 0) == 1, L"Enter applies the chosen Interface once");
		QueuedKey(interfaceCombo, VK_RETURN, WM_KEYUP);
		Expect(QueuedKey(interfaceCombo, VK_TAB) && GetFocus() == viewCombo, L"Tab moves from Interface to View");
		Expect(HandleKey(window, VK_TAB, true) && GetFocus() == interfaceCombo, L"Shift+Tab returns from View to Interface");
		SendMessageW(viewCombo, CB_SETCURSEL, 2, 0);
		SendMessageW(page, WM_COMMAND, MAKEWPARAM(kFirstSelector + 1, CBN_SELENDOK), reinterpret_cast<LPARAM>(viewCombo));
		Expect(host.selections == 2 && host.lastSelector == Selector::View && host.lastChoice == 103 && host.forcedSelectorQueries > 0, L"a native selection notification applies the exact View after revalidation");
		host.selectors[1].selected = 0; Refresh(window);
		Expect(SendMessageW(viewCombo, CB_GETCURSEL, 0, 0) == 0, L"external Classic View changes update the ribbon selection");
		host.executeSucceeds = false;
		Expect(!SelectChoice(window, Selector::View, 2) && SendMessageW(viewCombo, CB_GETCURSEL, 0, 0) == 0, L"a rejected View change restores the actual host value");
		host.executeSucceeds = true;
		const unsigned before = host.selections;
		host.selectors[0].context = L"other datasource/list/folder";
		Expect(!SelectChoice(window, Selector::Interface, 1) && host.selections == before, L"navigation between rendering and committing rejects a stale choice");
		host.selectors = original; Refresh(window);
		SetFocus(interfaceCombo); host.selectors[0].enabled = false; Refresh(window);
		Expect(!IsWindowEnabled(interfaceCombo) && GetFocus() != interfaceCombo && !SelectChoice(window, Selector::Interface, 1), L"permission changes disable the selector and preserve visible focus");
		host.selectors = original; Refresh(window);
		SelectTab(window, Tab::Home);
		Expect(!IsWindowVisible(interfaceCombo) && !IsWindowVisible(viewCombo) && !SelectChoice(window, Selector::View, 1), L"selectors are hidden and inactive on Home");
		SelectTab(window, Tab::View); EnableWindow(owner, FALSE);
		Expect(!SelectChoice(window, Selector::View, 2), L"modal Explorer prevents selector dispatch"); EnableWindow(owner, TRUE);
		HWND content = CreateWindowExW(0, L"STATIC", L"Selector content", WS_CHILD | WS_VISIBLE, 0, 0, 640, 500, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
		Expect(Dock(window, true), L"selector ribbon docks for narrow-window checks");
		SetFocus(toolbar); QueuedKey(toolbar, VK_TAB);
		RECT item = RelativeRect(interfaceCombo, page), viewport{}; GetClientRect(page, &viewport);
		Expect(GetFocus() == interfaceCombo && item.left >= 0 && item.right <= viewport.right, L"Tab reveals the Interface selector within a narrow ribbon");
		QueuedKey(interfaceCombo, VK_TAB); item = RelativeRect(viewCombo, page);
		Expect(GetFocus() == viewCombo && item.left >= 0 && item.right <= viewport.right, L"Tab reveals the View selector within a narrow ribbon");
		SetDisplayMode(window, DisplayMode::Minimized); SelectTab(window, Tab::View); SetFocus(interfaceCombo);
		SendMessageW(interfaceCombo, CB_SHOWDROPDOWN, TRUE, 0);
		COMBOBOXINFO info{sizeof(info)}; GetComboBoxInfo(interfaceCombo, &info);
		PostMessageW(info.hwndList, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
		MSG click{}; PeekMessageW(&click, info.hwndList, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
		Expect(IsPopupOpen(window), L"clicking a native dropdown list does not dismiss the minimized ribbon");
		const unsigned beforeEscape = host.selections;
		QueuedKey(interfaceCombo, VK_ESCAPE);
		Expect(!SendMessageW(interfaceCombo, CB_GETDROPPEDSTATE, 0, 0) && IsPopupOpen(window) && host.selections == beforeEscape,
			L"Escape cancels only the native dropdown before dismissing the ribbon");
		// Dispatch the WM_CHAR generated by TranslateMessage for the first native Escape before posting the next key.
		MSG character{};
		while (PeekMessageW(&character, interfaceCombo, WM_CHAR, WM_CHAR, PM_REMOVE)) DispatchMessageW(&character);
		QueuedKey(interfaceCombo, VK_ESCAPE);
		Expect(!IsPopupOpen(window), L"a second Escape dismisses minimized commands");
		SetDisplayMode(window, DisplayMode::Expanded); SelectTab(window, Tab::View);
		host.selectorsAvailable = false; Refresh(window);
		Expect(!IsWindowEnabled(interfaceCombo) && SendMessageW(interfaceCombo, CB_GETCOUNT, 0, 0) == 0 && !SelectChoice(window, Selector::Interface, 0), L"failed queries clear stale choices and disable selectors");
		host.selectorsAvailable = true; host.selectors = original; Refresh(window);
		host.destroyOnSelectorsRefresh = window;
		Expect(!SelectChoice(window, Selector::View, 0) && !IsWindow(window), L"destruction during selector revalidation blocks dispatch safely");
		window = Create(owner, GetModuleHandleW(nullptr), callbacks); ShowWindow(window, SW_SHOWNOACTIVATE); SelectTab(window, Tab::View);
		host.destroyOnSelect = window;
		Expect(SelectChoice(window, Selector::View, 0) && !IsWindow(window), L"destruction during selector execution is safe");
		DestroyWindow(content);
	}

	struct AdminSessionFixture
	{
		QuickAccess::AdminSession session, after;
		QuickAccess::AdminAccess outcome = QuickAccess::AdminAccess::Ready;
		bool available = true, replaceOnLogin = false, failAfterLogin = false, replaceOnSecondRead = false, throwLogin = false, throwRead = false;
		unsigned reads = 0, logins = 0;
		static bool Read(void* context, QuickAccess::AdminSession& result)
		{
			auto& fixture = *static_cast<AdminSessionFixture*>(context); ++fixture.reads;
			if (fixture.throwRead) throw 1;
			if (!fixture.available || (fixture.failAfterLogin && fixture.logins != 0)) return false;
			if (fixture.replaceOnSecondRead && fixture.reads == 2) fixture.session = fixture.after;
			result = fixture.session; return true;
		}
		static QuickAccess::AdminAccess Login(void* context, const QuickAccess::ContextState& expected, std::wstring& error)
		{
			auto& fixture = *static_cast<AdminSessionFixture*>(context); ++fixture.logins;
			Expect(expected.context == fixture.session.commands.context, L"automatic administrative login receives the validated current identity");
			if (fixture.throwLogin) throw 1;
			if (fixture.outcome == QuickAccess::AdminAccess::Failed) error = L"Denied by server.";
			if (fixture.outcome != QuickAccess::AdminAccess::Ready) return fixture.outcome;
			if (fixture.replaceOnLogin) fixture.session = fixture.after;
			else fixture.session.administrative = true;
			return fixture.outcome;
		}
		QuickAccess::AdminSessionHost Host() { return {this, Read, Login}; }
	};
	void CheckAdminSession()
	{
		using namespace QuickAccess;
		const auto shown = AdminContext(L"current datasource/current user/admin role", true, false);
		ContextState ready; std::wstring error;
		for (const auto command : {AdminCommand::NewUser, AdminCommand::Users})
		{
			AdminSessionFixture fixture; fixture.session.commands = shown;
			Expect(EnsureAdminSession(fixture.Host(), shown, static_cast<UINT>(command), ready, error) == AdminAccess::Ready &&
				fixture.logins == 1 && fixture.reads == 2 && ready.context == shown.context && error.empty(),
				L"each Admin command establishes and verifies administrative access as part of the action");
			Expect(EnsureAdminSession(fixture.Host(), shown, static_cast<UINT>(command), ready, error) == AdminAccess::Ready && fixture.logins == 1,
				L"an existing administrative connection is reused without another login");
		}
		AdminSessionFixture fixture; fixture.session.commands = shown;
		const auto restricted = AdminContext(L"current datasource/restricted administrator", false, true);
		fixture.session.commands = restricted;
		Expect(EnsureAdminSession(fixture.Host(), restricted, 2, ready, error) == AdminAccess::Ready && fixture.logins == 1 && ready.context == restricted.context,
			L"Restricted Administrators use the same automatic session flow without changing identity or role");
		fixture = {}; fixture.session.commands = shown;
		fixture.outcome = AdminAccess::Cancelled;
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Cancelled && fixture.logins == 1 && !ready.visible && !error.empty(),
			L"cancelled authentication stops the action without a retry or actionable context");
		fixture.outcome = AdminAccess::Ready;
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Ready && fixture.logins == 2,
			L"a later explicit action can retry authentication after cancellation");
		fixture = {}; fixture.session.commands = shown; fixture.outcome = AdminAccess::Failed;
		Expect(EnsureAdminSession(fixture.Host(), shown, 3, ready, error) == AdminAccess::Failed && fixture.logins == 1 && !ready.visible && error == L"Denied by server.",
			L"denied authentication retains the error and never releases an administrative command");
		for (const auto* changed : {L"different datasource/current user/admin role", L"current datasource/different user/admin role", L"current datasource/current user/restricted role"})
		{
			fixture = {}; fixture.session.commands = shown; fixture.replaceOnLogin = true;
			fixture.after = {AdminContext(changed, true, false), true};
			Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && fixture.logins == 1 && !ready.visible,
				L"datasource, user or role changes during login stop the original administrative action");
		}
		fixture = {}; fixture.session.commands = shown; fixture.replaceOnLogin = true; fixture.after = {shown, false};
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible,
			L"a nominal login success still requires the SDK to confirm administrative setup");
		fixture.after = {{}, true}; fixture.session = {shown, false};
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible,
			L"losing administrative membership during login prevents command execution");
		fixture.after = {shown, true}; fixture.after.commands.actions[0].enabled = false; fixture.session = {shown, false};
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible,
			L"a revoked command after successful login remains unavailable");
		fixture = {}; fixture.session.commands = shown; fixture.failAfterLogin = true;
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible,
			L"post-login read failure cannot reuse a stale administrative context");
		fixture = {}; fixture.session = {shown, true}; fixture.replaceOnSecondRead = true;
		fixture.after = {AdminContext(L"another datasource/user", true, false), true};
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && fixture.logins == 0 && !ready.visible,
			L"an already administrative session is revalidated before allowing the action");
		fixture = {}; fixture.session.commands = AdminContext(L"already changed datasource", true, false);
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && fixture.logins == 0,
			L"a stale command cannot initiate administrative authentication on a different datasource");
		fixture = {};
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && fixture.logins == 0,
			L"users without an administrative role cannot initiate the automatic sign-in");
		fixture.session.commands = shown;
		Expect(EnsureAdminSession(fixture.Host(), shown, 1, ready, error) == AdminAccess::Failed && fixture.logins == 0,
			L"the removed sign-in command cannot be invoked through its old numeric ID");
		fixture.throwLogin = true;
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible && !error.empty(),
			L"authentication exceptions are contained and never allow an administrative action");
		fixture.throwLogin = false; fixture.throwRead = true;
		Expect(EnsureAdminSession(fixture.Host(), shown, 2, ready, error) == AdminAccess::Failed && !ready.visible,
			L"session query exceptions do not retain an earlier ready context");
	}

	void CheckAdminAndRecovery(HWND owner, const wchar_t* previewPath)
	{
		using namespace QuickAccess;
		CheckAdminSession();
		Expect(!AdminContext(L"ds/user", false, false).visible, L"an administrative session alone does not bypass Admin-group visibility");
		Expect(!AdminContext(L"", true, false).visible, L"no datasource context means no Admin tab");
		const auto member = AdminContext(L"ds/user/member", true, false);
		const auto restricted = AdminContext(L"ds/user/restricted", false, true);
		Expect(member.visible && restricted.visible && member.actions.size() == 2 && restricted.actions.size() == 2 &&
			member.actions[0].enabled && restricted.actions[0].enabled, L"Admin members can open New User directly with no separate sign-in action");
		FakeHost host; host.admin = member;
		Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute};
		callbacks.refreshWorkflow = FakeHost::RefreshWorkflow; callbacks.executeWorkflow = FakeHost::ExecuteWorkflow;
		callbacks.refreshAdmin = FakeHost::RefreshAdmin; callbacks.executeAdmin = FakeHost::ExecuteAdmin;
		callbacks.restoreClassic = FakeHost::RestoreClassic; callbacks.failed = FakeHost::Failed;
		HWND window = Create(owner, GetModuleHandleW(nullptr), callbacks); host.recoveryWindow = window;
		Expect(window != nullptr && IsHealthy(window), L"contextual Admin ribbon starts with healthy controls");
		if (window == nullptr) return;
		ShowWindow(owner, SW_SHOWNOACTIVATE); ShowWindow(window, SW_SHOWNOACTIVATE);
		HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
		Expect(TabCtrl_GetItemCount(tabs) == 5 && !SelectTab(window, Tab::Workflow), L"Admin can appear without a Workflow tab");
		SetFocus(tabs); QueuedKey(tabs, VK_END);
		Expect(SelectedTab(window) == Tab::Admin && TabCtrl_GetCurSel(tabs) == 4, L"tab keyboard routing uses contextual identity rather than fixed native indices");
		QueuedKey(tabs, VK_DOWN);
		Expect(GetDlgCtrlID(GetFocus()) == kFirstAdmin && GetDlgItem(CommandPage(window), kFirstAdmin + 2) == nullptr,
			L"Admin keyboard entry reaches New User and no separate login control exists");
		Expect(QueuedKey(GetFocus(), VK_RETURN) && host.lastAdmin == static_cast<UINT>(AdminCommand::NewUser), L"Enter opens New User directly for an authorized member");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		Expect(InvokeAdmin(window, kFirstAdmin + 1) && host.lastAdmin == static_cast<UINT>(AdminCommand::Users), L"Users and Groups is available as a direct administrative action");
		host.admin = AdminContext(L"ds/user/admin", true, false); Refresh(window);
		Expect(InvokeAdmin(window, kFirstAdmin) && host.lastAdmin == static_cast<UINT>(AdminCommand::NewUser), L"New User dispatches only after current administrative context is revalidated");
		if (previewPath != nullptr) Expect(SaveRibbonPreview(window, previewPath), L"native contextual Admin preview written");
		host.workflow = {true, false, L"ds/folder/doc", {{501, L"Next", true, WorkflowActionKind::Next}}}; Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 6 && SelectedTab(window) == Tab::Admin && TabCtrl_GetCurSel(tabs) == 5, L"inserting Workflow preserves selection of the Admin tab");
		SetFocus(tabs); QueuedKey(tabs, VK_LEFT);
		Expect(SelectedTab(window) == Tab::Workflow, L"adjacent contextual tabs retain correct keyboard order");
		SelectTab(window, Tab::Admin); host.workflow = {}; Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 5 && SelectedTab(window) == Tab::Admin && TabCtrl_GetCurSel(tabs) == 4, L"removing Workflow preserves Admin's native selected index");
		const unsigned before = host.adminExecutions;
		host.admin.context = L"another datasource or user";
		Expect(!InvokeAdmin(window, kFirstAdmin) && host.adminExecutions == before, L"datasource or identity changes reject stale administrative actions");
		host.admin.actions[0].enabled = false; Refresh(window);
		Expect(!InvokeAdmin(window, kFirstAdmin), L"revoked create permissions cannot dispatch New User");
		EnableWindow(owner, FALSE); Expect(!InvokeAdmin(window, kFirstAdmin + 1), L"modal Explorer prevents administrative commands"); EnableWindow(owner, TRUE);
		SetDisplayMode(window, DisplayMode::Minimized); SelectTab(window, Tab::Admin);
		host.admin = {}; Refresh(window);
		Expect(SelectedTab(window) == Tab::Home && TabCtrl_GetItemCount(tabs) == 4 && !IsPopupOpen(window), L"losing Admin membership removes its tab and popup with a visible Home fallback");
		Expect(!InvokeAdmin(window, kFirstAdmin), L"removed Admin controls cannot be invoked");
		SetDisplayMode(window, DisplayMode::Expanded); SelectTab(window, Tab::Tools);
		HWND classic = GetDlgItem(CommandPage(window), kClassic);
		Expect(IsWindowVisible(classic) && AccessibleName(classic) == L"Re-enable Classic UI", L"Tools exposes an accessible Classic UI recovery option");
		SetFocus(classic);
		Expect(QueuedKey(classic, VK_RETURN) && host.classicSelections == 1 && !IsWindow(window), L"Classic UI can stop the ribbon by keyboard without a successful SDK command query");
		QueuedKey(owner, VK_RETURN, WM_KEYUP);
		host.admin = member;
		window = Create(owner, GetModuleHandleW(nullptr), callbacks); host.recoveryWindow = window;
		HWND content = CreateWindowExW(0, L"STATIC", L"Recovery content", WS_CHILD | WS_VISIBLE, 0, 0, 640, 500, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
		const RECT original = RelativeRect(content, owner);
		Expect(Dock(window, true), L"recovery test ribbon docks without hiding native UI");
		DestroyWindow(Toolbar(window));
		Expect(!IsHealthy(window), L"damage to an essential ribbon control is detected");
		SendMessageW(window, WM_TIMER, 1, 0);
		MSG message{};
		while (PeekMessageW(&message, window, WM_APP, WM_APP + 200, PM_REMOVE)) DispatchMessageW(&message);
		const RECT restored = RelativeRect(content, owner);
		Expect(host.recoveries == 1 && !IsWindow(window) && EqualRect(&original, &restored), L"a handled ribbon failure stops the ribbon and restores the docked content layout");
		DestroyWindow(content);
		window = Create(owner, GetModuleHandleW(nullptr), callbacks); host.destroyOnAdminRefresh = window;
		Refresh(window); Expect(!IsWindow(window), L"destruction during administrative-context refresh is safe");
		host.throwOnRefresh = true;
		window = Create(owner, GetModuleHandleW(nullptr), callbacks); host.recoveryWindow = window;
		Expect(window != nullptr && !IsHealthy(window), L"a throwing host refresh is contained instead of terminating Explorer");
		while (PeekMessageW(&message, window, WM_APP, WM_APP + 200, PM_REMOVE)) DispatchMessageW(&message);
		Expect(!IsWindow(window), L"a contained host refresh exception reaches Classic recovery");
		host.throwOnRefresh = false; host.states[2] = {true, false};
		window = Create(owner, GetModuleHandleW(nullptr), callbacks); host.recoveryWindow = window; host.throwOnExecute = true;
		Expect(!Invoke(window, kFirstButton + 2) && !IsHealthy(window), L"a throwing command is contained and schedules Classic recovery");
		while (PeekMessageW(&message, window, WM_APP, WM_APP + 200, PM_REMOVE)) DispatchMessageW(&message);
		host.throwOnExecute = false; host.throwOnCreateAddress = true; callbacks.createAddress = FakeHost::CreateAddress;
		Expect(Create(owner, GetModuleHandleW(nullptr), callbacks) == nullptr && IsWindowEnabled(owner), L"a throwing creation callback leaves the host UI usable");
	}

	struct RoleFixture
	{
		struct Edge { QuickAccess::RoleMemberKind kind; LONG id; std::vector<QuickAccess::RoleList> parents; };
		std::vector<Edge> edges;
		unsigned calls = 0;
		bool fail = false, chain = false, oversized = false;
		static bool Read(void* context, QuickAccess::RoleMemberKind kind, LONG id, std::vector<QuickAccess::RoleList>& result)
		{
			auto& fixture = *static_cast<RoleFixture*>(context); ++fixture.calls;
			if (fixture.fail) return false;
			if (fixture.chain) { result = {{id + 1, L"Ordinary team"}}; return true; }
			if (fixture.oversized) { result.resize(4097); return true; }
			const auto found = std::find_if(fixture.edges.begin(), fixture.edges.end(), [&](const auto& edge) { return edge.kind == kind && edge.id == id; });
			result = found == fixture.edges.end() ? std::vector<QuickAccess::RoleList>{} : found->parents; return true;
		}
	};
	void CheckProjectAdmin(HWND owner, const wchar_t* preview)
	{
		using namespace QuickAccess;
		for (const auto* name : {L"*BA", L"*ba", L"12345-Project Administrator", L"Different.Project-Project Administrator", L"ANY-project administrator"})
			Expect(IsProjectAdminList(name), L"project role names match the literal BA list or wildcard project-prefix suffix");
		for (const auto* name : {L"BA", L"ABCBA", L"ABC*BA", L"*BA-extra", L"12345-Project Administrator-extra", L"12345Project Administrator", L"12345-Project Administrator ", L"*BA\n"})
			Expect(!IsProjectAdminList(name), L"nearby names and trailing text cannot qualify as a project administration userlist");
		RoleFixture roles;
		roles.edges = {{RoleMemberKind::User, 10, {{20, L"*BA"}}}};
		Expect(FindProjectAdminRole(10, {}, &roles, RoleFixture::Read).id == 20, L"direct user membership qualifies for Project Admin");
		Expect(FindProjectAdminRole(11, {}, &roles, RoleFixture::Read).id == 0, L"another user's membership does not qualify the current user");
		roles.edges = {{RoleMemberKind::Group, 7, {{30, L"A-Project Administrator"}}}};
		Expect(FindProjectAdminRole(10, {7}, &roles, RoleFixture::Read).id == 30, L"membership through a user group qualifies for Project Admin");
		roles.edges = {{RoleMemberKind::User, 10, {{21, L"Team"}}}, {RoleMemberKind::UserList, 21, {{22, L"Nested team"}}},
			{RoleMemberKind::UserList, 22, {{21, L"Team"}, {30, L"B-Project Administrator"}}}};
		Expect(FindProjectAdminRole(10, {}, &roles, RoleFixture::Read).id == 30, L"nested userlists are resolved and cycles do not hide a valid role path");
		roles.fail = true;
		Expect(FindProjectAdminRole(10, {}, &roles, RoleFixture::Read).id == 0, L"unreadable membership cannot grant Project Admin");
		roles = {}; roles.chain = true;
		Expect(FindProjectAdminRole(10, {}, &roles, RoleFixture::Read).id == 0 && roles.calls == 256, L"unbounded membership chains stop at the query limit");
		roles = {}; roles.oversized = true;
		Expect(FindProjectAdminRole(10, {}, &roles, RoleFixture::Read).id == 0, L"oversized membership results fail without granting a role");
		Expect(!ProjectAdminContext(L"ds/user", L"ordinary list", true, true, L"Project").visible,
			L"datasource administrative status alone does not qualify for Project Admin");
		const auto member = ProjectAdminContext(L"ds/user/project-one", L"*BA", false, true, L"Test work area");
		Expect(member.visible && !member.actions[0].enabled && member.actions[1].enabled && member.actions[2].enabled,
			L"project membership enables owned-list and project tools while datasource user creation requires its own rights");
		const auto noProject = ProjectAdminContext(L"ds/user/no-project", L"A-Project Administrator", false, false, L"");
		Expect(noProject.visible && !noProject.actions[2].enabled && !noProject.actions[3].enabled && !noProject.status.empty(),
			L"eligible users retain the tab outside a work area with project-specific tools disabled");
		FakeHost host; host.projectAdmin = member;
		Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute};
		callbacks.refreshProjectAdmin = FakeHost::RefreshProjectAdmin; callbacks.executeProjectAdmin = FakeHost::ExecuteProjectAdmin;
		callbacks.refreshAdmin = FakeHost::RefreshAdmin; callbacks.executeAdmin = FakeHost::ExecuteAdmin;
		callbacks.refreshWorkflow = FakeHost::RefreshWorkflow; callbacks.executeWorkflow = FakeHost::ExecuteWorkflow;
		HWND window = Create(owner, GetModuleHandleW(nullptr), callbacks);
		Expect(window != nullptr && IsHealthy(window), L"Project Admin uses the shared healthy contextual ribbon");
		if (window == nullptr) return;
		ShowWindow(window, SW_SHOWNOACTIVATE);
		HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
		Expect(TabCtrl_GetItemCount(tabs) == 5 && SelectTab(window, Tab::ProjectAdmin) && TabCtrl_GetCurSel(tabs) == 4 && !SelectTab(window, Tab::Admin),
			L"Project Admin can be visible independently of DataSource Admin");
		SetFocus(tabs); QueuedKey(tabs, VK_DOWN);
		Expect(GetDlgCtrlID(GetFocus()) == kFirstProjectAdmin + 1 && QueuedKey(GetFocus(), VK_RETURN) &&
			host.lastProjectAdmin == static_cast<UINT>(ProjectAdminCommand::Members), L"keyboard entry skips unauthorized creation and opens Users and Lists");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		Expect(!InvokeProjectAdmin(window, kFirstProjectAdmin), L"the disabled project New User button cannot be dispatched");
		host.admin = AdminContext(L"ds/user/admin-role", true, false); Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 6 && SelectedTab(window) == Tab::ProjectAdmin && TabCtrl_GetCurSel(tabs) == 5,
			L"inserting DataSource Admin preserves the selected Project Admin identity");
		host.workflow = {true, false, L"ds/folder/workflow", {{11, L"Next", true, WorkflowActionKind::Next}}}; Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 7 && SelectedTab(window) == Tab::ProjectAdmin && TabCtrl_GetCurSel(tabs) == 6,
			L"all three contextual pages coexist in stable keyboard order");
		host.projectAdmin = ProjectAdminContext(L"ds/user/project-one", L"*BA", true, true, L"Test work area"); Refresh(window);
		Expect(InvokeProjectAdmin(window, kFirstProjectAdmin) && host.lastProjectAdmin == static_cast<UINT>(ProjectAdminCommand::NewUser),
			L"Project Admin exposes user creation when current datasource rights also allow it");
		if (preview != nullptr)
		{
			RECT original{}; GetWindowRect(owner, &original);
			SetWindowPos(owner, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			HWND content = CreateWindowExW(0, L"STATIC", L"Project content", WS_CHILD | WS_VISIBLE, 0, 0, 1200, 600, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
			Expect(Dock(window, true) && SaveRibbonPreview(window, preview), L"native combined DataSource Admin and Project Admin preview written");
			Dock(window, false); DestroyWindow(content);
			SetWindowPos(owner, nullptr, 0, 0, original.right - original.left, original.bottom - original.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		EnableWindow(owner, FALSE); Refresh(window);
		Expect(SelectedTab(window) == Tab::ProjectAdmin && !InvokeProjectAdmin(window, kFirstProjectAdmin + 2),
			L"native modal dialogs preserve Project Admin but block action dispatch");
		EnableWindow(owner, TRUE); Refresh(window);
		const auto before = host.projectAdminExecutions;
		host.projectAdmin.context = L"ds/user/other-project";
		Expect(!InvokeProjectAdmin(window, kFirstProjectAdmin + 2) && host.projectAdminExecutions == before,
			L"changing the work area prevents stale project action dispatch");
		Refresh(window); host.projectAdmin = {}; Refresh(window);
		Expect(SelectedTab(window) == Tab::Home && TabCtrl_GetItemCount(tabs) == 6 && !InvokeProjectAdmin(window, kFirstProjectAdmin),
			L"losing eligible userlist membership removes Project Admin and returns focus to Home");
		host.projectAdmin = member; Refresh(window); SelectTab(window, Tab::ProjectAdmin);
		host.destroyOnProjectAdminRefresh = window;
		Expect(!InvokeProjectAdmin(window, kFirstProjectAdmin + 1) && !IsWindow(window), L"destruction during project context revalidation is safe");
	}

	enum class UserDialogMode { Federated, Logical, Cancel, Throw };
	struct DialogFixture
	{
		HWND owner = nullptr; unsigned calls = 0, ticks = 0;
		UserDialogMode mode = UserDialogMode::Federated;
		const wchar_t* preview = nullptr;
	};
	DialogFixture* g_dialogFixture = nullptr;
	bool CreateFixtureUser(void* context, const QuickAccess::ContextState&, const QuickAccess::NewUser& user, std::wstring& error)
	{
		using namespace QuickAccess;
		auto& fixture = *static_cast<DialogFixture*>(context); ++fixture.calls;
		const auto calls = fixture.calls;
		HWND dialog = GetLastActivePopup(fixture.owner);
		SendMessageW(dialog, WM_COMMAND, IDOK, 0); SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
		Expect(fixture.calls == calls && !IsWindowEnabled(GetDlgItem(dialog, IDOK)), L"creation in progress blocks nested submissions and cancellation");
		if (fixture.mode == UserDialogMode::Throw) throw 1;
		if (fixture.mode == UserDialogMode::Logical)
			Expect(user.type == AccountType::Logical && user.password == L"Fixture P@ssword 42!" && user.confirmation == user.password,
				L"logical submission passes the exact confirmed password and selected type to the callback");
		else Expect(user.type == AccountType::Federated && user.password.empty() && user.confirmation.empty() && user.email == L"fixture@example.test",
			L"federated submission passes the identity email without a logical password");
		if (user.name == L"rejected") { error = L"Server rejected this account."; return false; }
		return user.name == L"fixture.user" && user.email == L"fixture@example.test";
	}
	void SelectUserType(HWND dialog, QuickAccess::AccountType type)
	{
		SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_SETCURSEL, static_cast<WPARAM>(type), 0);
		SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_USER_TYPE, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDC_USER_TYPE)));
	}
	void FillFixturePassword(HWND dialog)
	{
		SetDlgItemTextW(dialog, IDC_USER_PASSWORD, L"Fixture P@ssword 42!");
		SetDlgItemTextW(dialog, IDC_USER_CONFIRMATION, L"Fixture P@ssword 42!");
	}
	void CALLBACK DriveUserDialog(HWND, UINT, UINT_PTR, DWORD)
	{
		using namespace QuickAccess;
		auto& fixture = *g_dialogFixture;
		HWND dialog = GetLastActivePopup(fixture.owner);
		if (GetDlgItem(dialog, IDC_USER_NAME) == nullptr) return;
		if (++fixture.ticks > 1) { SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return; }
		Expect(SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_GETCURSEL, 0, 0) == 0 && !IsWindowVisible(GetDlgItem(dialog, IDC_USER_PASSWORD)),
			L"New User defaults to federated identity and hides logical credentials");
		Expect(GetNextDlgTabItem(dialog, GetDlgItem(dialog, IDC_USER_EMAIL), FALSE) != GetDlgItem(dialog, IDC_USER_PASSWORD) &&
			AccessibleName(GetDlgItem(dialog, IDC_USER_EMAIL)).find(L"Identity") != std::wstring::npos,
			L"federated keyboard traversal skips password controls and announces the identity email label");
		if (fixture.mode != UserDialogMode::Federated)
		{
			RECT federated{}; GetWindowRect(dialog, &federated);
			SelectUserType(dialog, AccountType::Logical);
			RECT logical{}; GetWindowRect(dialog, &logical);
			Expect(IsWindowVisible(GetDlgItem(dialog, IDC_USER_PASSWORD)) && logical.bottom - logical.top > federated.bottom - federated.top &&
				SendDlgItemMessageW(dialog, IDC_USER_PASSWORD, EM_GETPASSWORDCHAR, 0, 0) != 0 &&
				SendDlgItemMessageW(dialog, IDC_USER_CONFIRMATION, EM_GETPASSWORDCHAR, 0, 0) != 0,
				L"logical mode expands the form and masks both password fields");
			Expect(GetNextDlgTabItem(dialog, GetDlgItem(dialog, IDC_USER_EMAIL), FALSE) == GetDlgItem(dialog, IDC_USER_PASSWORD) &&
				GetNextDlgTabItem(dialog, GetDlgItem(dialog, IDC_USER_PASSWORD), FALSE) == GetDlgItem(dialog, IDC_USER_CONFIRMATION) &&
				AccessibleName(GetDlgItem(dialog, IDC_USER_EMAIL)).find(L"Contact") != std::wstring::npos,
				L"logical keyboard order reaches both credentials and the contact email has the correct accessible label");
			FillFixturePassword(dialog); SelectUserType(dialog, AccountType::Federated);
			Expect(GetWindowTextLengthW(GetDlgItem(dialog, IDC_USER_PASSWORD)) == 0 && GetWindowTextLengthW(GetDlgItem(dialog, IDC_USER_CONFIRMATION)) == 0 &&
				!IsWindowEnabled(GetDlgItem(dialog, IDC_USER_PASSWORD)), L"changing to federated mode clears and disables logical credentials");
			SelectUserType(dialog, AccountType::Logical);
			if (fixture.mode == UserDialogMode::Cancel)
			{ FillFixturePassword(dialog); SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return; }
		}
		SendMessageW(dialog, WM_COMMAND, IDOK, 0);
		Expect(fixture.calls == 0, L"blank New User submissions do not reach the creation callback");
		SetDlgItemTextW(dialog, IDC_USER_NAME, L"rejected");
		SendMessageW(dialog, WM_COMMAND, IDOK, 0);
		Expect(fixture.calls == 0, L"missing identity or password prevents creation before any native callback");
		SetDlgItemTextW(dialog, IDC_USER_EMAIL, L"fixture@example.test");
		if (fixture.mode != UserDialogMode::Federated) FillFixturePassword(dialog);
		SendMessageW(dialog, WM_COMMAND, IDOK, 0);
		Expect(fixture.calls == 1 && IsWindow(dialog) && GetWindowTextLengthW(GetDlgItem(dialog, IDC_USER_STATUS)) > 0, L"server rejection keeps New User open with an inline error");
		Expect(GetWindowTextLengthW(GetDlgItem(dialog, IDC_USER_PASSWORD)) == 0 && GetWindowTextLengthW(GetDlgItem(dialog, IDC_USER_CONFIRMATION)) == 0 &&
			IsWindowEnabled(GetDlgItem(dialog, IDOK)) && IsWindowEnabled(GetDlgItem(dialog, IDCANCEL)),
			L"server errors and throwing callbacks clear password controls and restore usable dialog buttons");
		if (fixture.mode == UserDialogMode::Throw) { SendMessageW(dialog, WM_CLOSE, 0, 0); return; }
		SetDlgItemTextW(dialog, IDC_USER_NAME, L"fixture.user"); SetDlgItemTextW(dialog, IDC_USER_EMAIL, L"fixture@example.test");
		SetDlgItemTextW(dialog, IDC_USER_DESCRIPTION, L"Design coordination user");
		SetDlgItemTextW(dialog, IDC_USER_STATUS, L"");
		if (fixture.mode == UserDialogMode::Logical) FillFixturePassword(dialog);
		if (fixture.preview != nullptr) Expect(SaveRibbonPreview(dialog, fixture.preview), L"native account-type form preview written using fake account data");
		SendMessageW(dialog, WM_COMMAND, IDOK, 0);
	}
	void CheckUserDialog(HWND owner, const wchar_t* federatedPreview, const wchar_t* logicalPreview)
	{
		using namespace QuickAccess;
		std::wstring error;
		NewUser user;
		Expect(user.type == AccountType::Federated, L"new accounts prefer federated identity by default");
		user.name = L"   ";
		Expect(!ValidateNewUser(user, error) && !error.empty(), L"New User validates a nonblank account name");
		user.name = L"bad\nname";
		Expect(!ValidateNewUser(user, error), L"New User rejects control characters in account names");
		user.name = L"fixture.user";
		int field = 0;
		Expect(!ValidateNewUser(user, error, &field) && field == IDC_USER_EMAIL, L"federated accounts require an identity email");
		for (const auto* email : {L"no-at", L"@example.test", L"user@", L"user@@example.test", L"two@example.test;other@example.test", L"user@example.test\n"})
		{
			user.email = email;
			Expect(!ValidateNewUser(user, error), L"invalid federated identity email is refused");
		}
		user.email = L"fixture@example.test";
		Expect(ValidateNewUser(user, error), L"federated accounts accept an identity email without a ProjectWise password");
		user.password = L"unexpected";
		Expect(!ValidateNewUser(user, error), L"a federated request cannot accidentally include logical credentials");
		user.ClearCredentials(); user.type = static_cast<AccountType>(42);
		Expect(!ValidateNewUser(user, error), L"unknown account types are rejected");
		user.type = AccountType::Logical; user.email.clear();
		Expect(!ValidateNewUser(user, error, &field) && field == IDC_USER_PASSWORD, L"logical accounts require a password even without a contact email");
		user.password = L"Fixture P@ssword 42!"; user.confirmation = L"different";
		Expect(!ValidateNewUser(user, error), L"logical accounts reject mismatched password confirmation");
		user.confirmation = user.password;
		Expect(ValidateNewUser(user, error), L"logical accounts accept a confirmed password without an optional contact email");
		user.password = user.confirmation = std::wstring(kMaxLogicalPasswordLength, L'x');
		Expect(ValidateNewUser(user, error), L"logical passwords accept the SDK maximum length");
		user.password += L'x'; user.confirmation = user.password;
		Expect(!ValidateNewUser(user, error), L"overlong logical passwords are rejected rather than silently truncated");
		user.password = user.confirmation = std::wstring(L"ab\0cd", 5);
		Expect(!ValidateNewUser(user, error), L"embedded nulls cannot shorten a password at the native API boundary");
		user.password = user.confirmation = L"  Fixture \u00a3 password  ";
		Expect(ValidateNewUser(user, error), L"password whitespace and Unicode are preserved for server policy validation");
		const auto nativeError = UserCreationError(58004, L"Invalid data", L"Datasource policy requires an authentication identity.");
		Expect(nativeError.find(L"58004") != std::wstring::npos && nativeError.find(L"authentication identity") != std::wstring::npos,
			L"New User retains the native error code, message and extended server detail");
		Expect(UserCreationError(58004, L"", L"").find(L"does not identify a permissions problem") != std::wstring::npos,
			L"generic invalid-data errors are not misreported as proven permissions failures");
		const auto shown = AdminContext(L"datasource/user/session", true, false);
		for (const auto mode : {UserDialogMode::Federated, UserDialogMode::Logical, UserDialogMode::Cancel, UserDialogMode::Throw})
		{
			DialogFixture fixture{owner}; fixture.mode = mode;
			fixture.preview = mode == UserDialogMode::Federated ? federatedPreview : mode == UserDialogMode::Logical ? logicalPreview : nullptr;
			g_dialogFixture = &fixture;
			const UINT_PTR timer = SetTimer(nullptr, 0, 50, DriveUserDialog);
			Expect(timer != 0, L"native New User dialog driver available");
			if (timer == 0) continue;
			const auto result = ShowNewUserDialog(owner, GetModuleHandleW(nullptr), shown, L"Test datasource", &fixture, CreateFixtureUser);
			KillTimer(nullptr, timer);
			const bool creates = mode == UserDialogMode::Federated || mode == UserDialogMode::Logical;
			Expect(result == (creates ? IDOK : IDCANCEL) && fixture.calls == (creates ? 2U : mode == UserDialogMode::Throw ? 1U : 0U) && IsWindowEnabled(owner),
				L"native New User restores owner activation after successful creation, cancellation or a callback exception");
		}
		g_dialogFixture = nullptr;
	}

	struct ClassicFixture
	{
		HMENU menu = nullptr;
		std::array<HWND, 4> bars{};
		std::array<UINT, 4> ids{0xE800, 45000, 0xE802, 45002};
		unsigned commands = 0, unrelated = 0;
	};
	LRESULT CALLBACK ClassicFrame(HWND frame, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
	{
		auto& fixture = *reinterpret_cast<ClassicFixture*>(data);
		if (message == WM_INITMENUPOPUP)
			for (std::size_t i = 0; i < fixture.bars.size(); ++i) CheckMenuItem(fixture.menu, fixture.ids[i], MF_BYCOMMAND | (IsWindowVisible(fixture.bars[i]) ? MF_CHECKED : MF_UNCHECKED));
		if (message == WM_COMMAND)
		{
			for (std::size_t i = 0; i < fixture.bars.size(); ++i)
				if (LOWORD(wParam) == fixture.ids[i]) { ++fixture.commands; ShowWindow(fixture.bars[i], SW_SHOWNOACTIVATE); return 0; }
			++fixture.unrelated;
		}
		return DefSubclassProc(frame, message, wParam, lParam);
	}
	void CheckClassicRestoration()
	{
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		HWND frame = CreateWindowExW(0, L"STATIC", L"Native toolbar recovery", WS_OVERLAPPEDWINDOW, 0, 0, 700, 500, nullptr, nullptr, instance, nullptr);
		ClassicFixture fixture; fixture.menu = CreateMenu();
		HMENU view = CreatePopupMenu(), bars = CreatePopupMenu();
		AppendMenuW(fixture.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
		AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(bars), L"Native toolbar names may be localized");
		for (const UINT id : fixture.ids) AppendMenuW(bars, MF_STRING | (id == 0xE802 ? MF_GRAYED : 0), id, L"Toolbar");
		AppendMenuW(bars, MF_STRING, 0xE805, L"Customize"); AppendMenuW(view, MF_STRING, 45001, L"Unrelated command");
		SetMenu(frame, fixture.menu);
		HWND floating = CreateWindowExW(0, L"STATIC", L"Floating toolbar", WS_POPUP | WS_VISIBLE, 10, 10, 200, 100, frame, nullptr, instance, nullptr);
		for (std::size_t i = 0; i < fixture.bars.size(); ++i)
			fixture.bars[i] = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr, WS_CHILD | (i == 0 ? WS_VISIBLE : 0), 0, 0, 200, 28,
				i == 3 ? floating : frame, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(fixture.ids[i])), instance, nullptr);
		SetWindowSubclass(frame, ClassicFrame, 1, reinterpret_cast<DWORD_PTR>(&fixture)); ShowWindow(frame, SW_SHOWNOACTIVATE);
		EnableWindow(frame, FALSE);
		Expect(QuickAccess::RestoreClassicToolbars(frame).failed != 0 && fixture.commands == 0, L"Classic recovery does not dispatch toolbar commands behind a modal dialog");
		EnableWindow(frame, TRUE);
		const auto result = QuickAccess::RestoreClassicToolbars(frame);
		Expect(result.found == 4 && result.restored == 2 && result.skipped == 1 && result.failed == 0, L"Classic recovery restores hidden docked and floating native toolbars and skips unavailable bars");
		Expect(fixture.commands == 2 && fixture.unrelated == 0 && IsWindowVisible(fixture.bars[0]), L"Classic recovery leaves visible toolbars on and never invokes Customize or unrelated commands");
		const auto repeat = QuickAccess::RestoreClassicToolbars(frame);
		Expect(repeat.restored == 0 && fixture.commands == 2, L"repeated Classic recovery cannot toggle native toolbars off");
		const bool preference = QuickAccess::PreferClassicUi();
		Expect(QuickAccess::SaveClassicUiPreference(true) && QuickAccess::PreferClassicUi(), L"Classic UI preference survives a fresh registry read");
		Expect(QuickAccess::SaveClassicUiPreference(false) && !QuickAccess::PreferClassicUi(), L"explicit Ribbon selection can clear the Classic UI preference");
		QuickAccess::SaveClassicUiPreference(preference);
		DestroyWindow(floating); DestroyWindow(frame); if (IsMenu(fixture.menu)) DestroyMenu(fixture.menu);
	}

	void CheckAccessibility(HWND owner, HWND window, FakeHost& host, HWND address, HWND content)
	{
		using QuickAccess::DisplayMode;
		using QuickAccess::Tab;
		Expect(std::abs(QuickAccess::ContrastRatio(RGB(0, 0, 0), RGB(255, 255, 255)) - 21.0) < 0.000001,
			L"WCAG contrast calculation gives the reference 21:1 for black and white");
		Expect(std::abs(QuickAccess::ContrastRatio(RGB(119, 119, 119), RGB(255, 255, 255)) - 4.47808945) < 0.000001,
			L"WCAG sRGB luminance matches the independent gray-on-white reference below the AA threshold");
		Expect(QuickAccess::ContrastColour(RGB(119, 119, 119), RGB(255, 255, 255), 4.5) == RGB(0, 0, 0) &&
			QuickAccess::ContrastColour(RGB(118, 118, 118), RGB(255, 255, 255), 4.5) == RGB(118, 118, 118),
			L"text contrast enforces the exact 4.5:1 boundary rather than a rounded 4.5 value");
		for (const COLORREF background : {RGB(255, 255, 255), RGB(0, 0, 0), RGB(30, 30, 30), RGB(120, 120, 120), RGB(0, 0, 128), RGB(255, 255, 0)})
		{
			Expect(QuickAccess::ContrastRatio(QuickAccess::ContrastColour(background, background, 4.5), background) >= 4.5,
				L"text fallback meets AA on light, dark, middle-gray and contrast-theme surfaces");
			Expect(QuickAccess::ContrastRatio(QuickAccess::ContrastColour(RGB(0, 120, 215), background, 3.0), background) >= 3.0,
				L"symbols and focus fallback meet 3:1 on light, dark and contrast-theme surfaces");
		}
		QuickAccess::SetDisplayMode(window, DisplayMode::Expanded);
		QuickAccess::SelectTab(window, Tab::Home);
		const HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr), toolbar = Toolbar(window);
		Expect(AccessibleName(tabs) == L"ProjectWise ribbon tabs" && AccessibleName(toolbar) == L"Home commands",
			L"native accessibility exposes tab and command-container names");
		Expect(AccessibleName(address) == L"ProjectWise location" && AccessibleName(GetDlgItem(window, QuickAccess::kHide)) == L"Hide ribbon",
			L"native accessibility names the editable location and icon-independent ribbon actions");
		QuickAccess::SelectTab(window, Tab::Search);
		Expect(AccessibleName(toolbar) == L"Search commands" && AccessibleName(GetDlgItem(CommandPage(window), QuickAccess::kSearchText)) == L"Search ProjectWise",
			L"tab changes update the accessible page name and query field has a real name, not only placeholder text");
		const int minimum = MulDiv(24, static_cast<int>(GetDpiForWindow(window)), 96);
		for (UINT id : {QuickAccess::kMaximise, QuickAccess::kMinimise, QuickAccess::kHide})
		{
			RECT target{};
			GetWindowRect(GetDlgItem(window, id), &target);
			Expect(target.right - target.left >= minimum && target.bottom - target.top >= minimum, L"ribbon mode targets meet the 24 logical-pixel minimum");
		}
		HWND row = GetDlgItem(window, QuickAccess::kNavigationRow);
		for (UINT id = QuickAccess::kFirstNavigation; id < QuickAccess::kFirstNavigation + QuickAccess::kNavigationCount; ++id)
		{
			RECT target{};
			GetWindowRect(GetDlgItem(row, id), &target);
			Expect(target.right - target.left >= minimum && target.bottom - target.top >= minimum && !AccessibleName(GetDlgItem(row, id)).empty(),
				L"navigation arrows have accessible text names and minimum-size targets");
		}
		RECT target{};
		GetWindowRect(address, &target);
		Expect(target.bottom - target.top >= minimum, L"editable address target is at least 24 logical pixels high");
		QuickAccess::SelectTab(window, Tab::Home);
		SetFocus(content);
		Expect(QuickAccess::HandleKey(window, VK_F6) && GetFocus() == tabs, L"F6 enters the ribbon from Explorer content");
		Expect(QuickAccess::HandleKey(window, VK_F6) && (GetFocus() == address || IsChild(address, GetFocus())), L"F6 moves from ribbon to persistent navigation");
		Expect(QuickAccess::HandleKey(window, VK_F6) && GetFocus() == content, L"F6 returns to Explorer content without trapping keyboard focus");
		Expect(QuickAccess::HandleKey(window, 'R', false, false, true) && GetFocus() == tabs, L"Alt+R focuses ribbon tabs");
		SendMessageW(tabs, WM_KEYDOWN, VK_RIGHT, 0);
		Expect(QuickAccess::SelectedTab(window) == Tab::View && AccessibleName(toolbar) == L"View commands", L"native Right Arrow changes the selected tab and its accessible page name");
		SendMessageW(tabs, WM_KEYDOWN, VK_LEFT, 0);
		Expect(QuickAccess::SelectedTab(window) == Tab::Home, L"native Left Arrow returns to the previous tab");
		Expect(QuickAccess::HandleKey(window, VK_TAB) && GetFocus() == GetDlgItem(window, QuickAccess::kMinimise),
			L"Tab skips the disabled current-mode button");
		Expect(QuickAccess::HandleKey(window, VK_TAB) && GetFocus() == GetDlgItem(window, QuickAccess::kHide), L"Tab reaches Hide in visual order");
		Expect(QuickAccess::HandleKey(window, VK_TAB) && GetFocus() == toolbar, L"Tab reaches the command toolbar as a single arrow-navigated stop");
		Expect(QuickAccess::HandleKey(window, VK_END) && SendMessageW(toolbar, TB_GETHOTITEM, 0, 0) == 8, L"End selects the last enabled command on this tab");
		RECT item{}, viewport{};
		SendMessageW(toolbar, TB_GETITEMRECT, 8, reinterpret_cast<LPARAM>(&item));
		MapWindowPoints(toolbar, CommandPage(window), reinterpret_cast<POINT*>(&item), 2);
		GetClientRect(CommandPage(window), &viewport);
		Expect(item.left >= 0 && item.right <= viewport.right, L"keyboard-selected commands scroll fully into view instead of becoming obscured");
		Expect(QuickAccess::HandleKey(window, VK_HOME) && SendMessageW(toolbar, TB_GETHOTITEM, 0, 0) == 0, L"Home selects and reveals the first enabled command");
		const unsigned before = host.executions;
		Expect(QuickAccess::HandleKey(window, VK_RETURN) && host.executions == before + 1 && host.last == 0, L"Enter activates the keyboard-selected toolbar command exactly once");
		Expect(QuickAccess::HandleKey(window, VK_SPACE) && host.executions == before + 2, L"Space also activates the selected command");
		Expect(QuickAccess::HandleKey(window, VK_TAB, true) && GetFocus() == GetDlgItem(window, QuickAccess::kHide), L"Shift+Tab traverses ribbon controls in reverse");
		QuickAccess::SetDisplayMode(window, DisplayMode::Hidden);
		Expect(QuickAccess::HandleKey(window, 'R', false, false, true) && GetFocus() == tabs && QuickAccess::GetDisplayMode(window) == DisplayMode::Expanded,
			L"Alt+R restores hidden tabs without requiring the mouse");
		Expect(QuickAccess::HandleKey(window, VK_F1, false, true) && QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized,
			L"Ctrl+F1 collapses commands and keeps keyboard focus on tabs");
		Expect(QuickAccess::HandleKey(window, VK_RETURN) && QuickAccess::IsPopupOpen(window) && GetFocus() == toolbar,
			L"Enter opens the already-selected minimized tab and focuses commands without executing one");
		Expect(QuickAccess::HandleKey(window, VK_ESCAPE) && !QuickAccess::IsPopupOpen(window) && GetFocus() == tabs,
			L"Escape returns keyboard focus from commands to the selected minimized tab");
		QuickAccess::SelectTab(window, Tab::Search);
		SetFocus(GetDlgItem(CommandPage(window), QuickAccess::kSearchText));
		Expect(QuickAccess::HandleKey(window, VK_ESCAPE) && !QuickAccess::IsPopupOpen(window) && GetFocus() == tabs,
			L"Escape dismisses a command popup and restores a visible tab focus target");
		Expect(QuickAccess::HandleKey(window, VK_F1, false, true) && QuickAccess::GetDisplayMode(window) == DisplayMode::Expanded, L"Ctrl+F1 restores expanded commands");
		SetFocus(address);
		SendMessageW(address, CB_SHOWDROPDOWN, TRUE, 0);
		Expect(!QuickAccess::HandleKey(window, VK_ESCAPE) && SendMessageW(address, CB_GETDROPPEDSTATE, 0, 0) != 0,
			L"ribbon Escape handling leaves an open native address drop-down to its own keyboard handler");
		SendMessageW(address, CB_SHOWDROPDOWN, FALSE, 0);
		Expect(QuickAccess::HandleKey(window, VK_TAB) && GetFocus() == content, L"Tab exits the last ribbon control to Explorer content");
		SetFocus(tabs);
		PostMessageW(tabs, WM_KEYDOWN, VK_TAB, 0);
		MSG message{};
		PeekMessageW(&message, tabs, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE);
		Expect(message.message == WM_NULL && GetFocus() == GetDlgItem(window, QuickAccess::kMinimise), L"the real UI-thread message hook applies keyboard traversal before Explorer translation");
		QueuedKey(tabs, VK_TAB, WM_KEYUP);

		// Exercise queue retrieval and actual native controls, not only direct calls into HandleKey.
		SetFocus(GetDlgItem(window, QuickAccess::kMinimise));
		Expect(QueuedKey(GetFocus(), VK_RETURN) && QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized,
			L"queued Enter activates Minimise in an ordinary frame without IsDialogMessage");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		Expect(QueuedKey(tabs, VK_RETURN) && QuickAccess::IsPopupOpen(window) && GetFocus() == toolbar,
			L"queued Enter opens minimized commands without also invoking a command");
		QueuedKey(toolbar, VK_RETURN, WM_KEYUP);
		QuickAccess::DismissPopup(window);
		SetFocus(GetDlgItem(window, QuickAccess::kMaximise));
		Expect(QueuedKey(GetFocus(), VK_RETURN) && QuickAccess::GetDisplayMode(window) == DisplayMode::Expanded,
			L"queued Enter activates Maximise and restores a visible tab focus target");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		SetFocus(tabs);
		QuickAccess::SelectTab(window, Tab::Home);
		Expect(QueuedKey(tabs, VK_RIGHT) && QuickAccess::SelectedTab(window) == Tab::View,
			L"queued tab arrows select pages before the host can translate them");
		QueuedKey(tabs, VK_RIGHT, WM_KEYUP);
		QuickAccess::SelectTab(window, Tab::Home);
		Expect(QueuedKey(tabs, VK_DOWN) && GetFocus() == toolbar, L"Down Arrow enters the selected tab's commands");
		QueuedKey(toolbar, VK_DOWN, WM_KEYUP);
		QuickAccess::HandleKey(window, VK_END);
		NMTBHOTITEM hover{};
		hover.hdr = {toolbar, 0, TBN_HOTITEMCHANGE};
		hover.dwFlags = HICF_MOUSE | HICF_ENTERING;
		hover.idNew = QuickAccess::kFirstButton;
		Expect(SendMessageW(CommandPage(window), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&hover)) != 0,
			L"pointer hover cannot change the keyboard command target");
		hover.dwFlags = HICF_LEAVING;
		Expect(SendMessageW(CommandPage(window), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&hover)) != 0 &&
			SendMessageW(toolbar, TB_GETHOTITEM, 0, 0) == 8, L"pointer exit cannot erase the keyboard focus target");
		const unsigned beforeRepeat = host.executions;
		host.focusOnExecute = content;
		host.nestedRepeatTarget = content;
		Expect(QueuedKey(toolbar, VK_RETURN) && host.executions == beforeRepeat + 1 && host.last == 8 && GetFocus() == content,
			L"queued Enter executes the focused command once and permits Explorer to take focus");
		Expect(host.nestedRepeatConsumed, L"held Enter is already suppressed while a synchronous host command pumps nested messages");
		Expect(QueuedKey(content, VK_RETURN, WM_KEYDOWN, 1L << 30) && host.executions == beforeRepeat + 1,
			L"holding Enter after command dispatch does not repeat the action or leak to Explorer");
		QueuedKey(content, VK_RETURN, WM_KEYUP);
		host.focusOnExecute = nullptr;
		host.nestedRepeatTarget = nullptr;
		SetFocus(tabs);
		Expect(!QuickAccess::HandleKey(window, VK_F6, false, true) && GetFocus() == tabs,
			L"Ctrl+F6 remains an Explorer accelerator, not ribbon traversal");
		Expect(!QuickAccess::HandleKey(window, 'R', false, true, true), L"AltGr+R is not intercepted as Alt+R");
		Expect((SendMessageW(toolbar, WM_GETDLGCODE, VK_TAB, 0) & DLGC_WANTTAB) != 0 &&
			(SendMessageW(GetDlgItem(window, QuickAccess::kHide), WM_GETDLGCODE, VK_RETURN, 0) & DLGC_WANTMESSAGE) != 0,
			L"native controls request navigation keys from dialog-style host translation");
		QuickAccess::SelectTab(window, Tab::Search);
		SetFocus(tabs);
		QuickAccess::HandleKey(window, VK_DOWN);
		Expect(QueuedKey(toolbar, VK_TAB) && GetFocus() == GetDlgItem(CommandPage(window), QuickAccess::kSearchText),
			L"Tab reaches the search edit field through the real keyboard hook");
		QueuedKey(GetFocus(), VK_TAB, WM_KEYUP);
		RECT searchRect = RelativeRect(GetFocus(), CommandPage(window)), searchViewport{};
		GetClientRect(CommandPage(window), &searchViewport);
		Expect(searchRect.left >= 0 && searchRect.right <= searchViewport.right, L"focused search text scrolls fully into the narrow command page");
		SetWindowTextW(GetFocus(), L"keyboard regression");
		const unsigned searches = host.searches;
		Expect(QueuedKey(GetFocus(), VK_RETURN) && host.searches == searches + 1, L"queued Enter in the search field submits exactly once before host translation");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		Expect(QueuedKey(GetFocus(), VK_TAB) && GetFocus() == GetDlgItem(CommandPage(window), QuickAccess::kSearch),
			L"Tab reaches Start Search");
		QueuedKey(GetFocus(), VK_TAB, WM_KEYUP);
		searchRect = RelativeRect(GetFocus(), CommandPage(window));
		Expect(searchRect.left >= 0 && searchRect.right <= searchViewport.right, L"focused Start Search scrolls fully into view");
		Expect(QueuedKey(GetFocus(), VK_RETURN) && host.searches == searches + 2, L"queued Enter activates Start Search without a dialog manager");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		SetFocus(GetDlgItem(row, QuickAccess::kFirstNavigation));
		const unsigned navigations = host.navigations;
		Expect(QueuedKey(GetFocus(), VK_RETURN) && host.navigations == navigations + 1,
			L"queued Enter activates a native navigation button");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		QuickAccess::SelectTab(window, Tab::Home);
		SetFocus(tabs);
		QuickAccess::HandleKey(window, VK_DOWN);
		host.states[8].enabled = false;
		QuickAccess::HandleKey(window, VK_HOME);
		host.states[0].enabled = false;
		QuickAccess::Refresh(window);
		Expect(GetFocus() == toolbar && SendMessageW(toolbar, TB_GETHOTITEM, 0, 0) == 1,
			L"availability refresh moves keyboard focus away from a newly disabled command");
		host.states[0].enabled = host.states[8].enabled = true;
		QuickAccess::Refresh(window);
		SetFocus(GetDlgItem(window, QuickAccess::kHide));
		Expect(QueuedKey(GetFocus(), VK_RETURN) && QuickAccess::GetDisplayMode(window) == DisplayMode::Hidden &&
			(GetFocus() == address || IsChild(address, GetFocus())), L"queued Enter on Hide returns focus to the persistent address field");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		QuickAccess::SetDisplayMode(window, DisplayMode::Expanded);
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::HandleKey(window, VK_F6), L"ribbon shortcuts do not move focus behind a modal Explorer dialog");
		EnableWindow(owner, TRUE);
		HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
		const int dpi = static_cast<int>(GetDpiForWindow(window)), width = MulDiv(76, dpi, 96), height = MulDiv(32, dpi, 96);
		HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
		ReleaseDC(window, screen);
		Expect(memory != nullptr && bitmap != nullptr, L"focus-rendering measurement surface created");
		if (memory != nullptr && bitmap != nullptr)
		{
			const auto old = SelectObject(memory, bitmap);
			DRAWITEMSTRUCT draw{};
			draw.CtlType = ODT_BUTTON;
			draw.CtlID = QuickAccess::kHide;
			draw.itemState = ODS_FOCUS;
			draw.hwndItem = GetDlgItem(window, QuickAccess::kHide);
			draw.hDC = memory;
			draw.rcItem = {0, 0, width, height};
			SendMessageW(window, WM_DRAWITEM, draw.CtlID, reinterpret_cast<LPARAM>(&draw));
			const int inset = MulDiv(3, dpi, 96);
			const COLORREF focus = GetPixel(memory, inset, height / 2), background = GetPixel(memory, 0, height / 2);
			Expect(focus != CLR_INVALID && background != CLR_INVALID && QuickAccess::ContrastRatio(focus, background) >= 3.0,
				L"actual painted focus outline, not only its colour helper, has at least 3:1 contrast");
			SelectObject(memory, old);
		}
		if (bitmap != nullptr) DeleteObject(bitmap);
		if (memory != nullptr) DeleteDC(memory);
		QuickAccess::SelectTab(window, Tab::Home);
	}

	void CheckRibbon(HWND owner, HWND window, HWND content, FakeHost& host)
	{
		using QuickAccess::Tab;
		using QuickAccess::DisplayMode;
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		HWND navigation = CreateWindowExW(0, L"STATIC", L"Independent native navigation", WS_CHILD | WS_VISIBLE,
			0, 0, 1000, 40, owner, nullptr, instance, nullptr);
		const RECT navigationRect = RelativeRect(navigation, owner);
		const auto original = host.states;
		host.executeSucceeds = true;
		for (auto& state : host.states) state = {true, false};
		HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
		HWND toolbar = Toolbar(window);
		Expect(tabs != nullptr && TabCtrl_GetItemCount(tabs) == 4, L"ribbon has exactly four tabs");
		Expect(GetParent(toolbar) == CommandPage(window) && GetParent(CommandPage(window)) == tabs,
			L"commands are physically contained by the selected tab page, not siblings of the tab control");
		const wchar_t* names[] = {L"Home", L"View", L"Search", L"Tools"};
		for (int tab = 0; tab < 4; ++tab)
		{
			wchar_t text[32]{};
			TCITEMW item{};
			item.mask = TCIF_TEXT;
			item.pszText = text;
			item.cchTextMax = ARRAYSIZE(text);
			TabCtrl_GetItem(tabs, tab, &item);
			Expect(wcscmp(text, names[tab]) == 0, names[tab]);
			TabCtrl_SetCurSel(tabs, tab);
			NMHDR notification{tabs, 0, TCN_SELCHANGE};
			SendMessageW(window, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&notification));
			Expect(QuickAccess::SelectedTab(window) == static_cast<Tab>(tab), L"native tab notification switches ribbon page");
			for (std::size_t button = 0; button < QuickAccess::kButtonCount; ++button)
			{
				const bool onTab = QuickAccess::ButtonTab(button) == static_cast<Tab>(tab);
				Expect((SendMessageW(toolbar, TB_ISBUTTONHIDDEN, QuickAccess::kFirstButton + button, 0) == 0) == onTab,
					L"only selected tab commands are visible");
				const unsigned before = host.executions;
				Expect(QuickAccess::Invoke(window, QuickAccess::kFirstButton + static_cast<UINT>(button)) == onTab &&
					host.executions == before + (onTab ? 1 : 0), L"inactive tab commands cannot be dispatched");
			}
		}
		QuickAccess::SelectTab(window, Tab::Home);
		SendMessageW(GetDlgItem(window, QuickAccess::kMinimise), BM_CLICK, 0, 0);
		const int header = MulDiv(32, static_cast<int>(GetDpiForWindow(window)), 96);
		RECT ribbon = RelativeRect(window, owner), pane = RelativeRect(content, owner);
		Expect(QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized && ribbon.bottom - ribbon.top == header &&
			pane.top == ribbon.bottom && !IsWindowVisible(toolbar), L"Minimise button reserves tabs-only height and hides commands");
		Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2), L"collapsed commands cannot execute without opening a tab");
		Expect(QuickAccess::SelectTab(window, Tab::View) && QuickAccess::IsPopupOpen(window), L"clicking a minimized tab temporarily reveals commands");
		RECT revealed = RelativeRect(window, owner), unchanged = RelativeRect(content, owner);
		Expect(revealed.bottom > pane.top && EqualRect(&pane, &unchanged) != FALSE && IsWindowVisible(toolbar),
			L"temporary command area overlays content without permanently expanding it");
		PostMessageW(navigation, WM_LBUTTONDOWN, 0, 0);
		MSG message{};
		PeekMessageW(&message, navigation, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
		Expect(!QuickAccess::IsPopupOpen(window) && !IsWindowVisible(toolbar), L"outside navigation click dismisses minimized commands");
		QuickAccess::SelectTab(window, Tab::View);
		PostMessageW(tabs, WM_KEYDOWN, VK_ESCAPE, 0);
		PeekMessageW(&message, tabs, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE);
		Expect(!QuickAccess::IsPopupOpen(window) && message.message == WM_NULL, L"Escape dismisses temporary commands without reaching Explorer");
		QuickAccess::SelectTab(window, Tab::View);
		Expect(QuickAccess::Invoke(window, QuickAccess::kFirstButton + 9) && !QuickAccess::IsPopupOpen(window) &&
			QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized, L"choosing a minimized command closes popup and retains tabs-only mode");
		SendMessageW(GetDlgItem(window, QuickAccess::kHide), BM_CLICK, 0, 0);
		pane = RelativeRect(content, owner);
		Expect(QuickAccess::GetDisplayMode(window) == DisplayMode::Hidden && !IsWindowVisible(window) && pane.top == 40,
			L"Hide button removes entire ribbon and returns space to content");
		Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 9), L"hidden ribbon cannot dispatch commands");
		Expect(QuickAccess::Restore(window) && IsWindowVisible(window) && QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized,
			L"restore returns hidden ribbon to its previous minimized mode");
		const RECT after = RelativeRect(navigation, owner);
		Expect(IsWindowVisible(navigation) && EqualRect(&navigationRect, &after) != FALSE,
			L"navigation remains visible and unmoved through minimize, peek, hide and restore");
		SendMessageW(GetDlgItem(window, QuickAccess::kMaximise), BM_CLICK, 0, 0);
		Expect(QuickAccess::GetDisplayMode(window) == DisplayMode::Expanded && IsWindowVisible(toolbar), L"Maximise button pins the full command area");
		SendMessageW(tabs, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(20, 10));
		Expect(QuickAccess::GetDisplayMode(window) == DisplayMode::Minimized, L"double-clicking a tab minimizes the ribbon");
		SendMessageW(tabs, WM_LBUTTONUP, 0, MAKELPARAM(20, 10));
		Expect(QuickAccess::IsPopupOpen(window), L"clicking the already-selected minimized tab reveals its commands");
		SendMessageW(tabs, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(20, 10));
		Expect(QuickAccess::GetDisplayMode(window) == DisplayMode::Expanded && !QuickAccess::IsPopupOpen(window),
			L"double-clicking a minimized tab pins commands and removes the popup hook");
		SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(500, 60));
		Expect(QuickAccess::IsDocked(window), L"clicking unused ribbon space does not accidentally undock it");
		QuickAccess::SelectTab(window, Tab::Home);
		RECT expanded = RelativeRect(window, owner);
		QuickAccess::SetDisplayMode(window, DisplayMode::Minimized);
		QuickAccess::SetDisplayMode(window, DisplayMode::Expanded);
		const RECT restored = RelativeRect(window, owner);
		Expect(EqualRect(&expanded, &restored) != FALSE, L"Maximise restores the original expanded dimensions exactly");
		SendMessageW(window, WM_HSCROLL, SB_RIGHT, 0);
		const RECT scrolled = RelativeRect(toolbar, window);
		Expect(scrolled.left < 0, L"narrow docked ribbon scrolls to otherwise clipped commands");
		QuickAccess::SelectTab(window, Tab::Search);
		HWND search = GetDlgItem(CommandPage(window), QuickAccess::kSearchText);
		Expect(IsWindowVisible(search) && IsWindowVisible(GetDlgItem(CommandPage(window), QuickAccess::kSearch)), L"Search tab exposes query field and Start Search");
		const unsigned searches = host.searches;
		SetWindowTextW(search, L"   ");
		Expect(!QuickAccess::Search(window) && host.searches == searches, L"blank query is rejected before host search");
		SetWindowTextW(search, L"drawing reference");
		SetFocus(search);
		SendMessageW(search, WM_KEYDOWN, VK_RETURN, 0);
		Expect(host.searches == searches + 1 && host.query == L"drawing reference", L"Enter submits the exact query to the host");
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::Search(window) && host.searches == searches + 1, L"modal Explorer blocks search");
		EnableWindow(owner, TRUE);
		host.searchSucceeds = false;
		Expect(!QuickAccess::Search(window) && host.searches == searches + 2, L"host search failure is reported rather than converted to success");
		host.searchSucceeds = true;
		host.available = false;
		Expect(!QuickAccess::Search(window) && host.searches == searches + 2, L"search rechecks host availability before submission");
		host.available = true;
		QuickAccess::SetDisplayMode(window, DisplayMode::Minimized);
		QuickAccess::SelectTab(window, Tab::Search);
		SendMessageW(GetDlgItem(CommandPage(window), QuickAccess::kSearch), BM_CLICK, 0, 0);
		Expect(host.searches == searches + 3 && !QuickAccess::IsPopupOpen(window), L"Start Search submits and dismisses a temporary Search tab");
		QuickAccess::SetDisplayMode(window, DisplayMode::Expanded);
		QuickAccess::SelectTab(window, Tab::Home);
		host.states = original;
		DestroyWindow(navigation);
	}

	void CheckDocking(HWND owner, HWND window, FakeHost& host)
	{
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		HWND content = CreateWindowExW(0, L"STATIC", L"Explorer content", WS_CHILD | WS_VISIBLE,
			0, 40, 880, 500, owner, reinterpret_cast<HMENU>(0xE900), instance, nullptr);
		Expect(content != nullptr, L"docking content pane created");
		ShowWindow(owner, SW_SHOWNOACTIVATE);
		ShowWindow(window, SW_SHOWNOACTIVATE);
		Expect(QuickAccess::Dock(window, true) && QuickAccess::IsDocked(window), L"toolbar docks at top");
		Expect(GetParent(window) == owner && (GetWindowLongW(window, GWL_STYLE) & WS_CHILD) != 0,
			L"docked toolbar is a child of Explorer, not a floating popup");
		RECT dock = RelativeRect(window, owner), pane = RelativeRect(content, owner);
		Expect(dock.top == 40 && dock.left == 0 && dock.right == 880 && pane.top == dock.bottom && pane.bottom == 540,
			L"docking reserves a row without overlapping Explorer content");
		SetWindowPos(content, nullptr, 0, 40, 1000, 600, SWP_NOZORDER | SWP_NOACTIVATE);
		dock = RelativeRect(window, owner);
		pane = RelativeRect(content, owner);
		Expect(dock.right == 1000 && pane.top == dock.bottom && pane.bottom == 640,
			L"native frame relayout resizes docked toolbar and preserves content space");
		SetWindowPos(content, nullptr, 0, 40, 1000, 600, SWP_NOZORDER | SWP_NOACTIVATE);
		const RECT repeated = RelativeRect(content, owner);
		Expect(EqualRect(&pane, &repeated) != FALSE, L"repeated frame layout does not progressively shrink content");
		ShowWindow(window, SW_HIDE);
		SendMessageW(window, WM_APP, 0, 0);
		pane = RelativeRect(content, owner);
		Expect(pane.top == 40 && pane.bottom == 640, L"hiding docked toolbar releases its row");
		ShowWindow(window, SW_SHOWNOACTIVATE);
		SendMessageW(window, WM_APP, 0, 0);
		pane = RelativeRect(content, owner);
		Expect(pane.top == dock.bottom && pane.bottom == 640, L"showing docked toolbar reserves its row again");
		CheckRibbon(owner, window, content, host);
		host.states[2].enabled = true;
		host.executeSucceeds = true;
		QuickAccess::Refresh(window);
		const unsigned before = host.executions;
		Expect(QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == before + 1,
			L"docked toolbar can execute an enabled command");
		const HWND toolbar = Toolbar(window);
		SendMessageW(CommandPage(window), WM_COMMAND, MAKEWPARAM(QuickAccess::kFirstButton + 2, 0), reinterpret_cast<LPARAM>(toolbar));
		Expect(host.executions == before + 2 && host.last == 2, L"docked native toolbar click notification executes the matching command");
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == before + 2,
			L"modal Explorer still blocks docked commands");
		EnableWindow(owner, TRUE);
		Expect(QuickAccess::Dock(window, false) && !QuickAccess::IsDocked(window), L"docked toolbar can float again");
		pane = RelativeRect(content, owner);
		Expect(pane.top == 40 && pane.bottom == 640 && GetWindow(window, GW_OWNER) == owner &&
			(GetWindowLongW(window, GWL_STYLE) & WS_POPUP) != 0,
			L"floating restores full content and Explorer window ownership");
		Expect(QuickAccess::Dock(window, true), L"toolbar can dock again after floating");
		DestroyWindow(window);
		pane = RelativeRect(content, owner);
		Expect(pane.top == 40 && pane.bottom == 640, L"destroying docked toolbar restores content layout");
		DestroyWindow(content);
	}

	void CheckNavigation(HWND owner)
	{
		using QuickAccess::DisplayMode;
		using QuickAccess::Tab;
		FakeHost host;
		for (auto& state : host.states) state = {true, false};
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		const QuickAccess::Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute, FakeHost::Search,
			FakeHost::CreateAddress, FakeHost::RefreshNavigation, FakeHost::Navigate, nullptr, FakeHost::RefreshAddress};
		HWND content = CreateWindowExW(0, L"STATIC", L"Navigation test content", WS_CHILD | WS_VISIBLE,
			0, 40, 1000, 600, owner, reinterpret_cast<HMENU>(0xE900), instance, nullptr);
		HWND window = QuickAccess::Create(owner, instance, callbacks);
		Expect(window != nullptr, L"ribbon with native address host created");
		if (window == nullptr) { DestroyWindow(content); return; }
		ShowWindow(window, SW_SHOWNOACTIVATE);
		Expect(QuickAccess::Dock(window, true), L"ribbon with persistent navigation docks");
		const HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
		const HWND navigation = GetDlgItem(window, QuickAccess::kNavigationRow);
		const HWND address = FindWindowExW(navigation, nullptr, L"COMBOBOX", nullptr);
		const int rowHeight = MulDiv(44, static_cast<int>(GetDpiForWindow(window)), 96);
		Expect(IsWindow(address) && GetParent(navigation) == window && !IsChild(tabs, navigation),
			L"native address control belongs to a dedicated row outside the collapsible tab page");
		for (int tab = 0; tab < 4; ++tab)
		{
			QuickAccess::SelectTab(window, static_cast<Tab>(tab));
			RECT tabRect = RelativeRect(tabs, window), nav = RelativeRect(navigation, window), page = RelativeRect(CommandPage(window), tabs);
			RECT tabClient{};
			GetClientRect(tabs, &tabClient);
			Expect(IsWindowVisible(address) && nav.top == tabRect.bottom && nav.bottom - nav.top == rowHeight,
				L"expanded navigation sits directly below the tab-and-command container on every tab");
			Expect(page.left >= 0 && page.right <= tabClient.right && page.bottom <= tabClient.bottom && page.top > 0,
				L"selected command page is geometrically inside the native tab control");
		}
		for (const auto mode : {DisplayMode::Minimized, DisplayMode::Hidden, DisplayMode::Expanded})
		{
			QuickAccess::SetDisplayMode(window, mode);
			Expect(IsWindowVisible(address) && IsWindowEnabled(address), L"navigation remains visible and enabled in every display mode");
			for (UINT button = 0; button < QuickAccess::kNavigationCount; ++button)
			{
				const unsigned before = host.navigations;
				SendMessageW(GetDlgItem(navigation, QuickAccess::kFirstNavigation + button), BM_CLICK, 0, 0);
				Expect(host.navigations == before + 1 && host.lastNavigation == button,
					L"Back, Forward and Up dispatch through native child-row notifications in every mode");
			}
		}
		QuickAccess::SetDisplayMode(window, DisplayMode::Hidden);
		RECT ribbon = RelativeRect(window, owner), pane = RelativeRect(content, owner), nav = RelativeRect(navigation, window);
		Expect(!IsWindowVisible(tabs) && IsWindowVisible(window) && nav.top == 0 && pane.top == 40 + rowHeight &&
			ribbon.bottom - ribbon.top == rowHeight, L"Hide removes tabs and commands but reserves exactly the persistent navigation row");
		QuickAccess::SetDisplayMode(window, DisplayMode::Minimized);
		nav = RelativeRect(navigation, window);
		pane = RelativeRect(content, owner);
		QuickAccess::SelectTab(window, Tab::Home);
		const RECT peekNavigation = RelativeRect(navigation, window), peekPane = RelativeRect(content, owner);
		Expect(QuickAccess::IsPopupOpen(window) && EqualRect(&nav, &peekNavigation) && EqualRect(&pane, &peekPane),
			L"minimized command popup leaves navigation and content positions unchanged");
		Expect(QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && !QuickAccess::IsPopupOpen(window),
			L"navigation dismisses a temporary command popup without collapsing its own row");
		const unsigned before = host.navigations;
		host.navigationStates[0].enabled = false;
		Expect(!QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == before,
			L"navigation availability is rechecked at dispatch, not trusted from a stale button state");
		host.navigationStates[0].enabled = true;
		host.navigationAvailable = false;
		QuickAccess::Refresh(window);
		Expect(IsWindowEnabled(address) && !IsWindowEnabled(GetDlgItem(navigation, QuickAccess::kFirstNavigation)) &&
			!QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == before,
			L"unavailable history blocks navigation without disabling a healthy address control");
		host.navigationAvailable = true;
		host.addressAvailable = false;
		QuickAccess::Refresh(window);
		Expect(!IsWindowEnabled(address) && IsWindowEnabled(GetDlgItem(navigation, QuickAccess::kFirstNavigation)) &&
			QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == before + 1,
			L"address synchronization failure does not disable or block Back/Forward/Up");
		host.addressAvailable = true;
		const unsigned beforeModal = host.navigations;
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == beforeModal, L"modal Explorer blocks navigation");
		EnableWindow(owner, TRUE);
		QuickAccess::SetDisplayMode(window, DisplayMode::Expanded);
		RECT resized{};
		SetWindowPos(content, nullptr, 0, 40, 640, 600, SWP_NOZORDER | SWP_NOACTIVATE);
		GetClientRect(navigation, &resized);
		const RECT field = RelativeRect(address, navigation);
		Expect(field.left >= 0 && field.right <= resized.right && field.right - field.left > 300,
			L"address field resizes with the frame without clipping its visible width");
		QuickAccess::SetDisplayMode(window, DisplayMode::Hidden);
		Expect(QuickAccess::Restore(window) && IsWindowVisible(tabs) && IsWindowVisible(address), L"restore reveals the tab container without recreating navigation");
		CheckAccessibility(owner, window, host, address, content);
		host.destroyOnNavigationRefresh = window;
		Expect(!QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && !IsWindow(window), L"destruction during navigation validation is safe");
		window = QuickAccess::Create(owner, instance, callbacks);
		host.destroyOnNavigate = window;
		Expect(QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && !IsWindow(window), L"destruction during navigation execution is safe");
		window = QuickAccess::Create(owner, instance, callbacks);
		host.destroyOnAddressRefresh = window;
		QuickAccess::Refresh(window);
		Expect(!IsWindow(window), L"destruction during independent address refresh is safe");
		DestroyWindow(content);
	}

	void CheckWorkflowRibbon(HWND owner, const wchar_t* previewPath)
	{
		using QuickAccess::Tab;
		using QuickAccess::DisplayMode;
		using Kind = QuickAccess::WorkflowActionKind;
		FakeHost host;
		for (auto& item : host.states) item.enabled = true;
		const QuickAccess::Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute, FakeHost::Search, FakeHost::CreateAddress,
			FakeHost::RefreshNavigation, FakeHost::Navigate, nullptr, FakeHost::RefreshAddress, FakeHost::RefreshWorkflow, FakeHost::ExecuteWorkflow};
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		HWND content = CreateWindowExW(0, L"STATIC", L"Workflow test content", WS_CHILD | WS_VISIBLE,
			0, 40, 640, 600, owner, reinterpret_cast<HMENU>(0xE900), instance, nullptr);
		HWND window = QuickAccess::Create(owner, instance, callbacks);
		Expect(window != nullptr, L"workflow-aware ribbon created");
		if (window == nullptr) { DestroyWindow(content); return; }
		ShowWindow(window, SW_SHOWNOACTIVATE);
		QuickAccess::Dock(window, true);
		HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
		Expect(TabCtrl_GetItemCount(tabs) == 4 && !QuickAccess::SelectTab(window, Tab::Workflow), L"Workflow tab is absent and cannot be selected without an applicable workflow");
		host.workflow = {true, false, L"source/folder/10,11", {{501, L"Next", true, Kind::Next}, {502, L"Previous", false, Kind::Previous}, {503, L"Change...", true, Kind::Change}}};
		QuickAccess::Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 5, L"applicable workflow inserts exactly one conditional tab");
		QuickAccess::Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 5, L"repeated workflow refresh does not duplicate tabs or controls");
		SetFocus(tabs);
		Expect(QueuedKey(tabs, VK_END) && QuickAccess::SelectedTab(window) == Tab::Workflow, L"End on the tab strip reaches the conditional Workflow tab");
		QueuedKey(tabs, VK_END, WM_KEYUP);
		HWND page = CommandPage(window), next = GetDlgItem(page, QuickAccess::kFirstWorkflow), previous = GetDlgItem(page, QuickAccess::kFirstWorkflow + 1), change = GetDlgItem(page, QuickAccess::kFirstWorkflow + 2);
		Expect(IsWindowVisible(next) && !IsWindowVisible(Toolbar(window)) && !IsWindowEnabled(previous), L"workflow commands replace the ordinary toolbar and preserve disabled Previous state");
		Expect(AccessibleName(next) == L"Next" && AccessibleName(change) == L"Change...", L"native workflow buttons expose their complete accessible names");
		Expect(QueuedKey(tabs, VK_DOWN) && GetFocus() == next, L"Down Arrow enters the first enabled workflow action");
		QueuedKey(next, VK_DOWN, WM_KEYUP);
		Expect(QueuedKey(next, VK_RIGHT) && GetFocus() == change, L"workflow arrow navigation skips disabled actions");
		QueuedKey(change, VK_RIGHT, WM_KEYUP);
		Expect(QueuedKey(change, VK_HOME) && GetFocus() == next, L"Home returns to the first workflow action");
		QueuedKey(next, VK_HOME, WM_KEYUP);
		const unsigned before = host.workflowExecutions;
		Expect(QueuedKey(next, VK_RETURN) && host.workflowExecutions == before + 1 && host.lastWorkflowCommand == 501,
			L"queued Enter dispatches the selected native workflow command exactly once");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		SendMessageW(next, WM_KEYDOWN, VK_SPACE, 0);
		SendMessageW(next, WM_KEYUP, VK_SPACE, 0);
		Expect(host.workflowExecutions == before + 2, L"native Space activation works for workflow buttons");
		Expect(!QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow + 1) && host.workflowExecutions == before + 2,
			L"disabled workflow actions cannot dispatch through the direct API");
		SetFocus(change);
		Expect(QueuedKey(change, VK_TAB) && GetDlgCtrlID(GetFocus()) == QuickAccess::kFirstNavigation, L"Tab leaves the whole workflow action group for navigation");
		QueuedKey(GetFocus(), VK_TAB, WM_KEYUP);
		SetFocus(change);
		Expect(QuickAccess::HandleKey(window, VK_TAB, true) && GetFocus() == GetDlgItem(window, QuickAccess::kHide), L"Shift+Tab leaves the workflow group in reverse visual order");
		host.workflow.context = L"source/folder/12";
		const unsigned beforeStale = host.workflowExecutions;
		Expect(!QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow) && host.workflowExecutions == beforeStale,
			L"a selection change between rendering and clicking rejects the stale workflow action");
		Expect(GetFocus() == tabs || GetFocus() == GetDlgItem(window, QuickAccess::kHide), L"selection-driven workflow rebuilding retains visible focus");
		host.workflow.rulesEngine = true;
		host.workflow.actions = {{801, L"Start quality review", true, Kind::Operation}, {802, L"Approve & publish", true, Kind::Operation},
			{803, L"Revise", false, Kind::Operation}, {804, L"Review", true, Kind::Operation}, {805, L"Issue for construction", true, Kind::Operation},
			{806, L"Reject", true, Kind::Operation}, {807, L"Archive", true, Kind::Operation}, {808, L"Reopen", true, Kind::Operation}};
		QuickAccess::Refresh(window);
		page = CommandPage(window);
		if (previewPath != nullptr) Expect(SaveRibbonPreview(window, previewPath), L"native Workflow ribbon preview written");
		Expect(GetDlgItem(page, QuickAccess::kFirstWorkflow + 7) != nullptr && AccessibleName(GetDlgItem(page, QuickAccess::kFirstWorkflow + 1)) == L"Approve & publish",
			L"WRE changes replace standard buttons with all configured operations and preserve literal accessible labels");
		Expect(!QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow + 2), L"disabled WRE operations remain non-executable");
		SetFocus(tabs);
		QuickAccess::HandleKey(window, VK_DOWN);
		Expect(QuickAccess::HandleKey(window, VK_END) && GetDlgCtrlID(GetFocus()) == QuickAccess::kFirstWorkflow + 7,
			L"End reaches the last enabled WRE operation");
		RECT button = RelativeRect(GetFocus(), page), viewport{};
		GetClientRect(page, &viewport);
		Expect(button.left >= 0 && button.right <= viewport.right, L"keyboard-focused WRE actions scroll fully into a narrow window");
		const unsigned wreBefore = host.workflowExecutions;
		Expect(QueuedKey(GetFocus(), VK_RETURN) && host.workflowExecutions == wreBefore + 1 && host.lastWorkflowCommand == 808,
			L"keyboard activation dispatches the exact configured WRE operation ID");
		QueuedKey(GetFocus(), VK_RETURN, WM_KEYUP);
		host.workflow.actions[7].enabled = false;
		QuickAccess::Refresh(window);
		Expect(IsWindowVisible(GetFocus()) && IsWindowEnabled(GetFocus()), L"revoking a focused WRE action leaves focus on a visible enabled control");
		QuickAccess::SelectTab(window, Tab::Home);
		Expect(!QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow), L"workflow actions cannot run from another tab");
		QuickAccess::SelectTab(window, Tab::Workflow);
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::HandleKey(window, VK_RETURN) && !QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow), L"modal Explorer blocks workflow shortcuts and execution");
		QuickAccess::Refresh(window);
		Expect(QuickAccess::SelectedTab(window) == Tab::Workflow && TabCtrl_GetItemCount(tabs) == 5,
			L"an external modal ProjectWise dialog disables workflow actions without removing or closing the selected tab");
		EnableWindow(owner, TRUE);
		QuickAccess::Refresh(window);
		QuickAccess::SetDisplayMode(window, DisplayMode::Minimized);
		QuickAccess::SelectTab(window, Tab::Workflow);
		SetFocus(tabs);
		QuickAccess::HandleKey(window, VK_DOWN);
		Expect(QuickAccess::IsPopupOpen(window) && GetDlgCtrlID(GetFocus()) == QuickAccess::kFirstWorkflow, L"minimized Workflow tab opens a keyboard-accessible command popup");
		host.workflow = {};
		QuickAccess::Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 4 && QuickAccess::SelectedTab(window) == Tab::Home && !QuickAccess::IsPopupOpen(window) && GetFocus() == tabs,
			L"losing the workflow removes its tab and popup and returns focus to Home tabs");
		Expect(!QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow), L"removed workflow controls cannot dispatch");
		host.workflow = {true, false, L"folder with no selected documents", {}};
		host.workflow.status = L"The folder has a workflow. Select documents to see available actions.";
		QuickAccess::Refresh(window);
		Expect(QuickAccess::SelectTab(window, Tab::Workflow) && GetDlgItem(page, QuickAccess::kFirstWorkflow) == nullptr,
			L"a workflow folder without a document selection exposes no executable document actions");
		Expect(AccessibleName(page) == host.workflow.status, L"workflow selection guidance is available to screen readers as well as the painted caption");
		host.workflowAvailable = false;
		QuickAccess::Refresh(window);
		Expect(TabCtrl_GetItemCount(tabs) == 4 && QuickAccess::SelectedTab(window) == Tab::Home, L"failed workflow queries remove stale controls");
		host.workflowAvailable = true;
		host.workflow = {true, true, L"source/folder/doc", {{801, L"Approve", true, Kind::Operation}}};
		QuickAccess::Refresh(window);
		host.destroyOnWorkflowRefresh = window;
		QuickAccess::Refresh(window);
		Expect(!IsWindow(window), L"destruction during workflow refresh is safe");
		window = QuickAccess::Create(owner, instance, callbacks);
		ShowWindow(window, SW_SHOWNOACTIVATE);
		QuickAccess::SelectTab(window, Tab::Workflow);
		host.destroyOnWorkflowExecute = window;
		Expect(QuickAccess::InvokeWorkflow(window, QuickAccess::kFirstWorkflow) && !IsWindow(window), L"destruction during workflow execution is safe");
		DestroyWindow(content);
	}

	void CheckSelection(HWND owner)
	{
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		HWND tree = CreateWindowExW(0, L"STATIC", L"Tree", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, owner, nullptr, instance, nullptr);
		HWND list = CreateWindowExW(0, L"STATIC", L"List", WS_CHILD | WS_VISIBLE, 100, 0, 100, 100, owner, nullptr, instance, nullptr);
		SetFocus(tree);
		Expect(QuickAccess::SelectionControl(owner, tree, list, list) == tree, L"focused tree overrides previous list selection");
		SetFocus(list);
		Expect(QuickAccess::SelectionControl(owner, tree, list, tree) == list, L"focused list overrides previous tree selection");
		HWND nested = CreateWindowExW(0, L"STATIC", L"Document list child", WS_CHILD | WS_VISIBLE,
			0, 0, 50, 50, list, nullptr, instance, nullptr);
		SetFocus(nested);
		Expect(QuickAccess::SelectionControl(owner, tree, list, tree) == list,
			L"focus in a nested document-list control preserves document context");
		SetFocus(nullptr);
		Expect(QuickAccess::SelectionControl(owner, tree, list, list) == list,
			L"selection remains available when floating toolbar or another application has focus");
		Expect(QuickAccess::SelectionControl(owner, tree, list, nullptr) == tree,
			L"startup without keyboard focus uses the selected tree context");
		DestroyWindow(list);
		Expect(QuickAccess::SelectionControl(owner, tree, list, list) == tree, L"destroyed selection control is not reused");
		DestroyWindow(tree);
		Expect(QuickAccess::SelectionControl(owner, tree, list, nullptr) == nullptr, L"missing selection controls reject commands");
	}
}

int wmain(int argc, wchar_t* argv[])
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Expect(sizeof(void*) == 4, L"x86 Explorer architecture");
	CheckNavigationAvailability();
	CheckMenuStates();
	CheckWorkflowMenus();
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	HWND owner = CreateWindowExW(0, L"STATIC", L"Quick Access check owner", WS_OVERLAPPEDWINDOW,
								  0, 0, 900, 600, nullptr, nullptr, instance, nullptr);
	if (owner == nullptr) return 2;
	FakeHost host;
	const QuickAccess::Host callbacks{&host, FakeHost::Refresh, FakeHost::Execute, FakeHost::Search};
	Expect(QuickAccess::Create(nullptr, instance, callbacks) == nullptr, L"invalid owner rejected");
	HWND window = QuickAccess::Create(owner, instance, callbacks);
	Expect(window != nullptr, L"toolbar created");
	if (window == nullptr)
	{
		DestroyWindow(owner);
		return 2;
	}
	HWND toolbar = Toolbar(window);
	Expect(toolbar != nullptr, L"native toolbar control exists");
	Expect(GetWindow(window, GW_OWNER) == owner, L"toolbar belongs to Explorer owner");
	Expect((GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0, L"floating ribbon allows interactive keyboard focus");
	Expect(SendMessageW(window, WM_MOUSEACTIVATE, 0, 0) == MA_ACTIVATE, L"mouse interaction can activate floating input controls");
	Expect(SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) == QuickAccess::kButtonCount, L"all ribbon commands are present in the toolbar");
	constexpr const wchar_t* expected[] = {L"New Folder", L"Add Documents", L"Check Out", L"Check In", L"Free", L"Properties", L"Refresh",
		L"Open", L"New Document", L"Thumbnails", L"Large Icons", L"Small Icons", L"List", L"Details", L"Manage Views", L"Interface",
		L"Advanced Search", L"Find in Folder", L"Local Documents", L"User Settings", L"Wizard Manager", L"Help Index", L"Show all versions"};
	static_assert(ARRAYSIZE(expected) == QuickAccess::kButtonCount);
	for (std::size_t n = 0; n < QuickAccess::kButtonCount; ++n)
	{
		wchar_t label[64]{};
		SendMessageW(toolbar, TB_GETBUTTONTEXTW, QuickAccess::kFirstButton + n, reinterpret_cast<LPARAM>(label));
		Expect(wcscmp(label, expected[n]) == 0, expected[n]);
		Expect(!SendMessageW(toolbar, TB_ISBUTTONENABLED, QuickAccess::kFirstButton + n, 0), L"commands initially disabled");
		Expect((SendMessageW(toolbar, TB_ISBUTTONHIDDEN, QuickAccess::kFirstButton + n, 0) == 0) ==
			(QuickAccess::ButtonTab(n) == QuickAccess::Tab::Home), L"Home shows only its own commands, including disabled ones");
	}
	HIMAGELIST images = reinterpret_cast<HIMAGELIST>(SendMessageW(toolbar, TB_GETIMAGELIST, 0, 0));
	int width = 0, height = 0;
	ImageList_GetIconSize(images, &width, &height);
	const int expectedIcon = MulDiv(32, static_cast<int>(GetDpiForWindow(window)), 96);
	Expect(width == expectedIcon && height == expectedIcon, L"icons are 32 logical pixels at the window DPI");
	CheckCommandIcons(window, toolbar);
	if (argc >= 3 && wcscmp(argv[1], L"--preview") == 0)
		Expect(SaveCommandPreview(window, toolbar, argv[2]), L"native enabled/disabled command preview written");
	RECT rect{};
	SendMessageW(toolbar, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rect));
	Expect(rect.bottom - rect.top >= MulDiv(64, static_cast<int>(GetDpiForWindow(window)), 96), L"large buttons have space for icon and label");
	RECT client{}, lastButton{};
	GetClientRect(toolbar, &client);
	SendMessageW(toolbar, TB_GETITEMRECT, 8, reinterpret_cast<LPARAM>(&lastButton));
	Expect(client.right >= lastButton.right && client.bottom >= lastButton.bottom, L"toolbar client area contains every button without clipping");

	host.states[2] = {true, true};
	QuickAccess::Refresh(window);
	Expect(SendMessageW(toolbar, TB_ISBUTTONENABLED, QuickAccess::kFirstButton + 2, 0) != 0, L"host state enables Check Out");
	Expect(SendMessageW(toolbar, TB_ISBUTTONCHECKED, QuickAccess::kFirstButton + 2, 0) != 0, L"host checked state mirrored");
	Expect(QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == 1 && host.last == 2,
		   L"enabled button executes its matching command exactly once");
	host.states[2].enabled = false;
	Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == 1, L"selection rechecked before dispatch");
	Expect(!QuickAccess::Invoke(window, 99) && !QuickAccess::Invoke(window, QuickAccess::kFirstButton + QuickAccess::kButtonCount) &&
		host.executions == 1, L"unknown command IDs never dispatched");
	host.states[2].enabled = true;
	host.available = false;
	QuickAccess::Refresh(window);
	Expect(!SendMessageW(toolbar, TB_ISBUTTONENABLED, QuickAccess::kFirstButton + 2, 0), L"host query failure disables stale commands");
	Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == 1, L"host failure blocks execution");
	host.available = true;
	EnableWindow(owner, FALSE);
	Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == 1, L"modal owner blocks execution");
	EnableWindow(owner, TRUE);
	CheckSelection(owner);
	host.executeSucceeds = false;
	Expect(!QuickAccess::Invoke(window, QuickAccess::kFirstButton + 2) && host.executions == 2, L"dispatch failure reported to caller");
	ShowWindow(window, SW_SHOWNOACTIVATE);
	SendMessageW(window, WM_CLOSE, 0, 0);
	Expect(IsWindow(window) && !IsWindowVisible(window), L"close hides the toolbar without destroying it");
	HWND closing = QuickAccess::Create(owner, instance, callbacks);
	Expect(closing != nullptr, L"shutdown check toolbar created");
	host.destroyOnRefresh = closing;
	QuickAccess::Refresh(closing);
	Expect(!IsWindow(closing), L"toolbar destruction during refresh is safe");
	closing = QuickAccess::Create(owner, instance, callbacks);
	Expect(closing != nullptr, L"dispatch shutdown check toolbar created");
	host.destroyOnRefresh = closing;
	const unsigned beforeShutdown = host.executions;
	Expect(!QuickAccess::Invoke(closing, QuickAccess::kFirstButton + 2) && !IsWindow(closing) && host.executions == beforeShutdown,
		   L"destruction during revalidation blocks dispatch safely");
	HWND docking = QuickAccess::Create(owner, instance, callbacks);
	Expect(docking != nullptr, L"docking check toolbar created");
	CheckDocking(owner, docking, host);
	HWND searching = QuickAccess::Create(owner, instance, callbacks);
	Expect(searching != nullptr && QuickAccess::SelectTab(searching, QuickAccess::Tab::Search), L"search shutdown check ribbon created");
	SetWindowTextW(GetDlgItem(CommandPage(searching), QuickAccess::kSearchText), L"shutdown check");
	host.destroyOnSearch = searching;
	Expect(QuickAccess::Search(searching) && !IsWindow(searching), L"ribbon destruction during search is safe");
	CheckNavigation(owner);
	CheckWorkflowRibbon(owner, argc >= 5 && wcscmp(argv[3], L"--workflow-preview") == 0 ? argv[4] : nullptr);
	CheckVersionsToggle(owner, argc >= 7 && wcscmp(argv[5], L"--view-preview") == 0 ? argv[6] : nullptr);
	CheckSelectors(owner, argc >= 9 && wcscmp(argv[7], L"--selectors-preview") == 0 ? argv[8] : nullptr);
	CheckAdminAndRecovery(owner, argc >= 11 && wcscmp(argv[9], L"--admin-preview") == 0 ? argv[10] : nullptr);
	CheckProjectAdmin(owner, argc >= 21 && wcscmp(argv[19], L"--project-admin-preview") == 0 ? argv[20] : nullptr);
	CheckUserDialog(owner, argc >= 17 && wcscmp(argv[15], L"--new-user-preview") == 0 ? argv[16] : nullptr,
		argc >= 19 && wcscmp(argv[17], L"--logical-user-preview") == 0 ? argv[18] : nullptr);
	CheckClassicRestoration();
	CheckVersionStacks(argc >= 13 && wcscmp(argv[11], L"--stacks-preview") == 0 ? argv[12] : nullptr, Expect, SaveRibbonPreview);
	CheckNotifications(owner, argc >= 15 && wcscmp(argv[13], L"--notification-preview") == 0 ? argv[14] : nullptr, Expect, SaveRibbonPreview);
	HWND keyboard = QuickAccess::Create(owner, instance, callbacks);
	SetFocus(owner);
	host.destroyOnRefresh = keyboard;
	Expect(QuickAccess::HandleKey(keyboard, VK_F1, false, true) && !IsWindow(keyboard), L"Ctrl+F1 remains safe if host refresh destroys the ribbon");
	keyboard = QuickAccess::Create(owner, instance, callbacks);
	QuickAccess::SetDisplayMode(keyboard, QuickAccess::DisplayMode::Hidden);
	SetFocus(owner);
	host.destroyOnRefresh = keyboard;
	Expect(QuickAccess::HandleKey(keyboard, 'R', false, false, true) && !IsWindow(keyboard), L"Alt+R remains safe if restore refresh destroys the ribbon");
	DestroyWindow(owner);
	Expect(!IsWindow(window), L"destroying Explorer destroys its owned toolbar");
	std::wprintf(L"%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
