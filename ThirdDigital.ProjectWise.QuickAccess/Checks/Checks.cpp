#include "..\src\QuickAccess.h"

#include <commctrl.h>
#include <oleacc.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

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
		QuickAccess::NavigationStates navigationStates{{{true, false}, {true, false}, {true, false}}};
		unsigned navigations = 0;
		std::size_t lastNavigation = QuickAccess::kNavigationCount;
		HWND destroyOnNavigationRefresh = nullptr;
		HWND destroyOnNavigate = nullptr;

		static bool Refresh(void* context, QuickAccess::States& result)
		{
			auto& host = *static_cast<FakeHost*>(context);
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
			++host.executions;
			host.last = button;
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

		static HWND CreateAddress(void*, HWND parent)
		{
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
	};

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
		const int cellWidth = MulDiv(112, dpi, 96), cellHeight = MulDiv(64, dpi, 96), header = MulDiv(32, dpi, 96);
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
			FakeHost::CreateAddress, FakeHost::RefreshNavigation, FakeHost::Navigate};
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
		Expect(!IsWindowEnabled(address) && !QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == before,
			L"navigation host failure disables the address control and blocks dispatch");
		host.navigationAvailable = true;
		EnableWindow(owner, FALSE);
		Expect(!QuickAccess::Navigate(window, QuickAccess::kFirstNavigation) && host.navigations == before, L"modal Explorer blocks navigation");
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
	Expect(sizeof(void*) == 4, L"x86 Explorer architecture");
	CheckMenuStates();
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
		L"Advanced Search", L"Find in Folder", L"Local Documents", L"User Settings", L"Wizard Manager", L"Help Index"};
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
	if (argc == 3 && wcscmp(argv[1], L"--preview") == 0)
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
