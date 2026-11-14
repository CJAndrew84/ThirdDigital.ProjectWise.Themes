#include "QuickAccess.h"

#include <commctrl.h>
#include <initguid.h>
#include <oleacc.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <new>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace QuickAccess
{
	namespace
	{
		constexpr wchar_t kClass[] = L"AtkinsRealis.ProjectWise.QuickAccess";
		constexpr const wchar_t* kLabels[] = {L"New Folder", L"Add Documents", L"Check Out", L"Check In", L"Free", L"Properties", L"Refresh",
			L"Open", L"New Document", L"Thumbnails", L"Large Icons", L"Small Icons", L"List", L"Details", L"Manage Views", L"Interface",
			L"Advanced Search", L"Find in Folder", L"Local Documents", L"User Settings", L"Wizard Manager", L"Help Index", L"Show all versions"};
		constexpr const wchar_t* kTabs[] = {L"Home", L"View", L"Search", L"Tools", L"Workflow", L"Admin"};
		constexpr UINT kRecover = WM_APP + 101;
		static_assert(ARRAYSIZE(kLabels) == kButtonCount);
		thread_local HWND g_popup = nullptr;

		struct WindowState
		{
			Host host;
			HWND owner = nullptr;
			HWND window = nullptr;
			WindowState* keyboardNext = nullptr;
			HWND lastContentFocus = nullptr;
			IAccPropServices* accessibility = nullptr;
			bool comInitialised = false;
			HWND toolbar = nullptr;
			int focusedCommand = -1;
			HWND tabs = nullptr;
			HWND page = nullptr;
			HWND navigation = nullptr;
			HWND address = nullptr;
			std::array<HWND, kNavigationCount> navigationButtons{};
			std::array<HWND, 3> modeButtons{};
			HWND searchText = nullptr;
			HWND searchButton = nullptr;
			HWND classic = nullptr;
			std::array<HWND, kSelectorCount> selectors{}, selectorLabels{};
			SelectorStates selectorStates;
			bool forceSelectors = false;
			WorkflowState workflow;
			std::vector<HWND> workflowButtons;
			ContextState admin;
			std::vector<HWND> adminButtons;
			bool failurePending = false;
			Tab tab = Tab::Home;
			DisplayMode mode = DisplayMode::Expanded;
			DisplayMode restoreMode = DisplayMode::Expanded;
			bool popup = false;
			HHOOK popupHook = nullptr;
			bool layoutBusy = false;
			int scroll = 0;
			HWND dockClient = nullptr;
			RECT dockRect{};
			int dockHeight = 0;
			bool arranging = false;
			HIMAGELIST images = nullptr;
			HFONT font = nullptr;
			bool busy = false;
			bool creating = true;
		};
		thread_local WindowState* g_keyboardWindows = nullptr;
		thread_local HHOOK g_keyboardHook = nullptr;
		thread_local std::array<bool, 256> g_consumedKeys{};

		WindowState* State(HWND window) noexcept
		{
			return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		}

		void Trace(const wchar_t* message) noexcept
		{
			OutputDebugStringW(L"ProjectWise Quick Access: ");
			OutputDebugStringW(message);
			OutputDebugStringW(L"\n");
		}

		bool LayoutRibbon(HWND window, WindowState& state) noexcept;
		bool Contextual(Tab tab) noexcept { return tab == Tab::Workflow || tab == Tab::Admin; }
		ContextState& Context(WindowState& state, Tab tab) noexcept { return tab == Tab::Admin ? state.admin : state.workflow; }
		const ContextState& Context(const WindowState& state, Tab tab) noexcept { return tab == Tab::Admin ? state.admin : state.workflow; }
		std::vector<HWND>& ContextButtons(WindowState& state, Tab tab) noexcept { return tab == Tab::Admin ? state.adminButtons : state.workflowButtons; }
		const std::vector<HWND>& ContextButtons(const WindowState& state, Tab tab) noexcept { return tab == Tab::Admin ? state.adminButtons : state.workflowButtons; }
		Tab TabAt(HWND tabs, int index) noexcept
		{
			TCITEMW item{}; item.mask = TCIF_PARAM;
			return index >= 0 && TabCtrl_GetItem(tabs, index, &item) ? static_cast<Tab>(item.lParam) : Tab::Home;
		}
		int TabIndex(HWND tabs, Tab tab) noexcept
		{
			for (int i = 0; i < TabCtrl_GetItemCount(tabs); ++i) if (TabAt(tabs, i) == tab) return i;
			return -1;
		}
		void RequestRecovery(HWND window, const wchar_t* reason) noexcept
		{
			auto* state = State(window);
			if (state == nullptr || state->failurePending) return;
			state->failurePending = true;
			Trace(reason);
			PostMessageW(window, kRecover, 0, reinterpret_cast<LPARAM>(reason));
		}

		bool HasSelectors(const WindowState& state) noexcept
		{
			return state.host.refreshSelectors != nullptr && state.host.selectChoice != nullptr;
		}

		bool ButtonOnPage(const WindowState& state, std::size_t button) noexcept
		{
			return ButtonTab(button) == state.tab && !(HasSelectors(state) && button == kInterfaceDialog);
		}

		int SelectorIndex(const WindowState& state, HWND control) noexcept
		{
			for (std::size_t i = 0; i < state.selectors.size(); ++i) if (state.selectors[i] == control && control != nullptr) return static_cast<int>(i);
			return -1;
		}

		bool ValidSelector(const SelectorState& selector) noexcept
		{
			if (selector.choices.size() > 1024 || selector.selected < -1 || selector.selected >= static_cast<int>(selector.choices.size())) return false;
			if (!selector.choices.empty() && selector.context.empty()) return false;
			for (std::size_t i = 0; i < selector.choices.size(); ++i)
			{
				if (selector.choices[i].label.empty()) return false;
				for (std::size_t j = 0; j < i; ++j) if (selector.choices[i].id == selector.choices[j].id) return false;
			}
			return true;
		}

		bool ComboPopupContains(HWND combo, HWND target) noexcept
		{
			if (combo == nullptr || SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) == 0) return false;
			COMBOBOXINFO info{sizeof(info)};
			return target == combo || IsChild(combo, target) || (GetComboBoxInfo(combo, &info) && target == info.hwndList);
		}

		void UpdateSelectors(WindowState& state, SelectorStates fresh)
		{
			for (const auto& selector : fresh) if (!ValidSelector(selector)) { fresh = {}; break; }
			for (std::size_t i = 0; i < state.selectors.size(); ++i)
			{
				const HWND combo = state.selectors[i];
				auto& before = state.selectorStates[i];
				const auto& after = fresh[i];
				if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0 && after.enabled && before.context == after.context) continue;
				if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0) SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
				if (before.context != after.context || before.choices != after.choices)
				{
					SendMessageW(combo, CB_RESETCONTENT, 0, 0);
					for (const auto& choice : after.choices)
						if (SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label.c_str())) < 0) throw std::bad_alloc();
					SendMessageW(combo, CB_SETDROPPEDWIDTH, MulDiv(360, static_cast<int>(GetDpiForWindow(combo)), 96), 0);
					NotifyWinEvent(EVENT_OBJECT_REORDER, combo, OBJID_CLIENT, CHILDID_SELF);
				}
				SendMessageW(combo, CB_SETCURSEL, after.selected, 0);
				EnableWindow(combo, after.enabled && !after.choices.empty());
				before = std::move(fresh[i]);
			}
		}

		int WorkflowButton(const WindowState& state, HWND control) noexcept
		{
			const auto& buttons = ContextButtons(state, state.tab);
			const auto found = std::find(buttons.begin(), buttons.end(), control);
			return found == buttons.end() ? -1 : static_cast<int>(found - buttons.begin());
		}

		HWND WorkflowEntry(const WindowState& state) noexcept
		{
			for (HWND button : ContextButtons(state, state.tab))
				if (IsWindowVisible(button) && IsWindowEnabled(button)) return button;
			return nullptr;
		}

		LRESULT CALLBACK KeyboardMessages(int code, WPARAM wParam, LPARAM lParam)
		{
			if (code >= 0 && wParam == PM_REMOVE)
			{
				auto& message = *reinterpret_cast<MSG*>(lParam);
				const UINT key = static_cast<UINT>(message.wParam);
				if ((message.message == WM_KEYUP || message.message == WM_SYSKEYUP) && key < g_consumedKeys.size() && g_consumedKeys[key])
				{
					g_consumedKeys[key] = false;
					message.message = WM_NULL;
				}
				if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)
				{
					// A held activation key must not execute again after focus moves to Explorer or another ribbon control.
					if (key < g_consumedKeys.size() && g_consumedKeys[key] && (message.lParam & (1L << 30)) != 0 &&
						(key == VK_RETURN || key == VK_SPACE || key == VK_F1 || key == 'R'))
					{
						message.message = WM_NULL;
						return CallNextHookEx(nullptr, code, wParam, lParam);
					}
					for (auto* state = g_keyboardWindows; state != nullptr; state = state->keyboardNext)
					{
						if (message.hwnd != state->window && !IsChild(state->window, message.hwnd) &&
							message.hwnd != state->owner && !IsChild(state->owner, message.hwnd)) continue;
						// SDK commands may enter a modal message loop before HandleKey returns.
						const bool previouslyConsumed = key < g_consumedKeys.size() && g_consumedKeys[key];
						if (key < g_consumedKeys.size()) g_consumedKeys[key] = true;
						if (HandleKey(state->window, static_cast<UINT>(message.wParam), (GetKeyState(VK_SHIFT) & 0x8000) != 0,
							(GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0))
						{
							message.message = WM_NULL;
						}
						else if (key < g_consumedKeys.size()) g_consumedKeys[key] = previouslyConsumed;
						break;
					}
				}
			}
			return CallNextHookEx(nullptr, code, wParam, lParam);
		}

		bool RegisterKeyboard(WindowState& state) noexcept
		{
			if (g_keyboardHook == nullptr) g_keyboardHook = SetWindowsHookExW(WH_GETMESSAGE, KeyboardMessages, nullptr, GetCurrentThreadId());
			if (g_keyboardHook == nullptr) { Trace(L"could not install ribbon keyboard navigation"); return false; }
			state.keyboardNext = g_keyboardWindows;
			g_keyboardWindows = &state;
			return true;
		}

		void UnregisterKeyboard(WindowState& state) noexcept
		{
			for (auto** item = &g_keyboardWindows; *item != nullptr; item = &(*item)->keyboardNext)
			{
				if (*item != &state) continue;
				*item = state.keyboardNext;
				break;
			}
			if (g_keyboardWindows == nullptr && g_keyboardHook != nullptr)
			{
				UnhookWindowsHookEx(g_keyboardHook);
				g_keyboardHook = nullptr;
				g_consumedKeys.fill(false);
			}
		}

		bool Annotate(WindowState& state, HWND control, const wchar_t* name, const wchar_t* description, const wchar_t* shortcut = L"") noexcept
		{
			if (control == nullptr) return true;
			constexpr DWORD client = static_cast<DWORD>(OBJID_CLIENT);
			const bool result = SUCCEEDED(state.accessibility->SetHwndPropStr(control, client, CHILDID_SELF, PROPID_ACC_NAME, name)) &&
				SUCCEEDED(state.accessibility->SetHwndPropStr(control, client, CHILDID_SELF, PROPID_ACC_DESCRIPTION, description)) &&
				SUCCEEDED(state.accessibility->SetHwndPropStr(control, client, CHILDID_SELF, PROPID_ACC_KEYBOARDSHORTCUT, shortcut));
			if (!result) Trace(L"could not set a ribbon control's accessibility properties");
			return result;
		}

		bool InitialiseAccessibility(WindowState& state) noexcept
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			state.comInitialised = SUCCEEDED(com);
			if ((FAILED(com) && com != RPC_E_CHANGED_MODE) || FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&state.accessibility))))
			{
				Trace(L"could not initialise native accessibility annotations");
				return false;
			}
			return Annotate(state, state.selectors[0], L"Interface", L"Choose a ProjectWise attribute interface. Alt+Down opens the list; Enter applies a selection.") &&
				Annotate(state, state.classic, L"Re-enable Classic UI", L"Restore native Explorer toolbars and stop the ribbon.") &&
				Annotate(state, state.selectors[1], L"View", L"Choose a document-list column view. Alt+Down opens the list; Enter applies a selection.") &&
				Annotate(state, state.tabs, L"ProjectWise ribbon tabs", L"Use Left and Right Arrow to select a tab. F6 moves to navigation.", L"Alt+R") &&
				Annotate(state, state.toolbar, L"Home commands", L"Use arrow keys to select a command and Enter or Space to run it.") &&
				Annotate(state, state.searchText, L"Search ProjectWise", L"Enter a simple search expression, then press Enter.") &&
				Annotate(state, state.address, L"ProjectWise location", L"Editable ProjectWise address. Enter a location and press Enter to navigate.", L"F6") &&
				Annotate(state, state.modeButtons[0], L"Maximise ribbon", L"Keep the selected tab's commands visible.", L"Ctrl+F1") &&
				Annotate(state, state.modeButtons[1], L"Minimise ribbon", L"Collapse commands while keeping tabs and navigation visible.", L"Ctrl+F1") &&
				Annotate(state, state.modeButtons[2], L"Hide ribbon", L"Hide tabs and commands, leaving navigation visible. Alt+R restores the ribbon.");
		}

		void ClearAccessibility(WindowState& state) noexcept
		{
			if (state.accessibility == nullptr) return;
			const HWND controls[] = {state.tabs, state.toolbar, state.searchText, state.address,
				state.modeButtons[0], state.modeButtons[1], state.modeButtons[2], state.selectors[0], state.selectors[1], state.classic};
			const MSAAPROPID properties[] = {PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION, PROPID_ACC_KEYBOARDSHORTCUT};
			for (const HWND control : controls)
				if (control != nullptr) state.accessibility->ClearHwndProps(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, ARRAYSIZE(properties));
			for (const Tab tab : {Tab::Workflow, Tab::Admin}) for (const HWND control : ContextButtons(state, tab))
				state.accessibility->ClearHwndProps(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, ARRAYSIZE(properties));
			state.accessibility->Release();
			state.accessibility = nullptr;
		}

		void RevealCommand(WindowState& state, std::size_t index) noexcept
		{
			state.focusedCommand = static_cast<int>(index);
			SendMessageW(state.toolbar, TB_SETHOTITEM, index, 0);
			RECT item{}, viewport{};
			SendMessageW(state.toolbar, TB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item));
			GetClientRect(state.page, &viewport);
			if (item.left < state.scroll) state.scroll = item.left;
			else if (item.right > state.scroll + viewport.right) state.scroll = item.right - viewport.right;
			if (!LayoutRibbon(state.window, state)) Trace(L"could not reveal the keyboard-focused command");
			if (GetFocus() == state.toolbar)
				NotifyWinEvent(EVENT_OBJECT_FOCUS, state.toolbar, OBJID_CLIENT, static_cast<LONG>(index) + 1);
		}

		void RevealControl(WindowState& state, HWND control) noexcept
		{
			if (control != state.searchText && control != state.searchButton && control != state.classic && WorkflowButton(state, control) < 0 && SelectorIndex(state, control) < 0) return;
			RECT item{}, viewport{};
			GetWindowRect(control, &item);
			MapWindowPoints(nullptr, state.page, reinterpret_cast<POINT*>(&item), 2);
			GetClientRect(state.page, &viewport);
			if (item.left < 0 || item.right - item.left > viewport.right) state.scroll += item.left;
			else if (item.right > viewport.right) state.scroll += item.right - viewport.right;
			if (!LayoutRibbon(state.window, state)) Trace(L"could not reveal the keyboard-focused search control");
		}

		bool FocusControl(WindowState& state, HWND control) noexcept
		{
			if (!IsWindowVisible(control) || !IsWindowEnabled(control)) return false;
			if (control == state.toolbar)
			{
				int candidate = state.focusedCommand;
				if (candidate < 0 || !ButtonOnPage(state, static_cast<std::size_t>(candidate)) ||
					SendMessageW(control, TB_ISBUTTONENABLED, kFirstButton + candidate, 0) == 0) candidate = -1;
				for (std::size_t i = 0; i < kButtonCount; ++i)
				{
					if (candidate < 0 && ButtonOnPage(state, i) && SendMessageW(control, TB_ISBUTTONENABLED, kFirstButton + i, 0) != 0)
					{
						candidate = static_cast<int>(i);
						break;
					}
				}
				if (candidate < 0) return false;
				SetFocus(control);
				RevealCommand(state, static_cast<std::size_t>(candidate));
			}
			else
			{
				RevealControl(state, control);
				SetFocus(control);
			}
			InvalidateRect(control, nullptr, FALSE);
			return GetFocus() == control || IsChild(control, GetFocus());
		}

		bool FocusContent(WindowState& state) noexcept
		{
			if (state.host.focusContent != nullptr)
			{
				const HWND window = state.window;
				try { return state.host.focusContent(state.host.context); }
				catch (...) { RequestRecovery(window, L"ribbon host focus failed"); return false; }
			}
			const HWND target = IsWindow(state.lastContentFocus) && IsChild(state.owner, state.lastContentFocus) ? state.lastContentFocus : state.owner;
			SetFocus(target);
			return GetFocus() == target || IsChild(target, GetFocus());
		}

		COLORREF Blend(COLORREF a, COLORREF b, int weight) noexcept
		{
			return RGB((GetRValue(a) * weight + GetRValue(b) * (100 - weight)) / 100,
				(GetGValue(a) * weight + GetGValue(b) * (100 - weight)) / 100,
				(GetBValue(a) * weight + GetBValue(b) * (100 - weight)) / 100);
		}

		void Fill(HDC dc, const RECT& rect, COLORREF colour) noexcept
		{
			SetDCBrushColor(dc, colour);
			FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		}

		void Text(HDC dc, HFONT font, const wchar_t* text, RECT rect, COLORREF colour, UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE) noexcept
		{
			const auto old = SelectObject(dc, font);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, colour);
			DrawTextW(dc, text, -1, &rect, flags | DT_END_ELLIPSIS | DT_NOPREFIX);
			SelectObject(dc, old);
		}

		void FocusRing(HDC dc, RECT rect, COLORREF background, int dpi) noexcept
		{
			const int inset = MulDiv(3, dpi, 96);
			InflateRect(&rect, -inset, -inset);
			HPEN pen = CreatePen(PS_SOLID, std::max(2, MulDiv(2, dpi, 96)), ContrastColour(GetSysColor(COLOR_HOTLIGHT), background, 3.0));
			if (pen == nullptr) return;
			const auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
			Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
		}

		void Glyph(HDC dc, std::size_t command, int x, int y, int size, COLORREF colour) noexcept
		{
			const int saved = SaveDC(dc);
			if (saved == 0) return;
			SetMapMode(dc, MM_ANISOTROPIC);
			SetWindowExtEx(dc, 32, 32, nullptr);
			SetViewportExtEx(dc, size, size, nullptr);
			SetViewportOrgEx(dc, x, y, nullptr);
			LOGBRUSH brush{BS_SOLID, colour, 0};
			HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 2, &brush, 0, nullptr);
			if (pen == nullptr) { RestoreDC(dc, saved); return; }
			SelectObject(dc, pen);
			SelectObject(dc, GetStockObject(NULL_BRUSH));
			const auto line = [dc](int a, int b, int c, int d) { MoveToEx(dc, a, b, nullptr); LineTo(dc, c, d); };
			const auto path = [dc](std::initializer_list<POINT> points) { Polyline(dc, points.begin(), static_cast<int>(points.size())); };
			const auto curve = [dc](std::initializer_list<POINT> points) { PolyBezier(dc, points.begin(), static_cast<DWORD>(points.size())); };
			const auto document = [&path]() {
				path({{6, 29}, {6, 3}, {19, 3}, {26, 10}, {26, 29}, {6, 29}});
				path({{19, 3}, {19, 10}, {26, 10}});
			};
			const auto folder = [&path]() { path({{3, 27}, {3, 7}, {12, 7}, {16, 11}, {29, 11}, {29, 27}, {3, 27}}); };
			switch (command)
			{
			case 0: // Folder and a clear creation badge.
				folder(); line(16, 15, 16, 23); line(12, 19, 20, 19); break;
			case 1: // A stack distinguishes multiple uploads from a new document.
				path({{3, 23}, {3, 3}, {19, 3}});
				path({{8, 28}, {8, 7}, {22, 7}, {28, 13}, {28, 28}, {8, 28}});
				path({{22, 7}, {22, 13}, {28, 13}});
				line(18, 17, 18, 25); line(14, 21, 22, 21); break;
			case 2: case 3: // Opposite arrows, rather than nearly identical padlocks.
				path({{19, 8}, {19, 3}, {4, 3}, {4, 29}, {19, 29}, {19, 24}});
				line(12, 16, 29, 16);
				if (command == 2) path({{24, 11}, {29, 16}, {24, 21}});
				else path({{17, 11}, {12, 16}, {17, 21}});
				break;
			case 4: // Release a lock; no delete/trash metaphor for Free.
				Rectangle(dc, 5, 14, 25, 29);
				curve({{10, 14}, {10, 0}, {26, 0}, {26, 10}});
				line(15, 20, 15, 24); break;
			case 5:
				document();
				line(10, 16, 22, 16); line(10, 23, 22, 23);
				line(14, 13, 14, 19); line(19, 20, 19, 26); break;
			case 6: // Explicit curves avoid GDI Arc's easy-to-reverse sweep direction.
				curve({{5, 15}, {5, 4}, {19, 1}, {26, 10}});
				path({{26, 4}, {26, 10}, {20, 10}});
				curve({{27, 17}, {27, 28}, {13, 31}, {6, 22}});
				path({{6, 28}, {6, 22}, {12, 22}}); break;
			case 7:
				path({{3, 27}, {3, 7}, {12, 7}, {16, 11}, {25, 11}});
				path({{3, 27}, {8, 16}, {29, 16}, {24, 27}, {3, 27}}); break;
			case 8:
				document(); line(16, 15, 16, 25); line(11, 20, 21, 20); break;
			case 9:
				Rectangle(dc, 3, 5, 29, 27); Ellipse(dc, 7, 9, 13, 15);
				path({{5, 25}, {14, 16}, {20, 22}, {24, 18}, {28, 23}}); break;
			case 10:
				for (int row = 0; row < 2; ++row) for (int col = 0; col < 2; ++col)
					Rectangle(dc, 4 + col * 14, 4 + row * 14, 14 + col * 14, 14 + row * 14);
				break;
			case 11:
				for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col)
					Rectangle(dc, 4 + col * 9, 4 + row * 9, 9 + col * 9, 9 + row * 9);
				break;
			case 12:
				for (int row = 0; row < 3; ++row) { Rectangle(dc, 4, 5 + row * 9, 8, 9 + row * 9); line(12, 7 + row * 9, 28, 7 + row * 9); }
				break;
			case 13:
				Rectangle(dc, 3, 4, 29, 28); line(3, 11, 29, 11); line(11, 4, 11, 28);
				line(3, 19, 29, 19); line(21, 4, 21, 28); break;
			case 14:
				path({{13, 28}, {3, 28}, {3, 4}, {29, 4}, {29, 12}});
				line(3, 11, 29, 11); line(10, 11, 10, 28);
				line(16, 19, 29, 19); line(16, 26, 29, 26);
				line(20, 16, 20, 22); line(25, 23, 25, 29); break;
			case 15:
				Rectangle(dc, 3, 4, 29, 28); line(3, 11, 29, 11); line(11, 11, 11, 28);
				line(16, 16, 25, 16); line(16, 22, 23, 22); break;
			case 16:
				Ellipse(dc, 3, 3, 23, 23); line(21, 21, 29, 29);
				line(7, 10, 18, 10); line(7, 16, 18, 16); line(11, 7, 11, 12); line(15, 14, 15, 19); break;
			case 17:
				path({{13, 27}, {3, 27}, {3, 7}, {11, 7}, {15, 11}, {28, 11}, {28, 15}});
				Ellipse(dc, 15, 15, 27, 27); line(25, 25, 30, 30); break;
			case 18:
				path({{8, 19}, {8, 3}, {19, 3}, {24, 8}, {24, 19}});
				line(12, 10, 19, 10); line(12, 15, 20, 15);
				path({{3, 19}, {10, 19}, {13, 23}, {19, 23}, {22, 19}, {29, 19}, {29, 29}, {3, 29}, {3, 19}}); break;
			case 19:
				Ellipse(dc, 4, 3, 16, 15);
				curve({{2, 28}, {2, 14}, {18, 14}, {18, 28}});
				line(21, 9, 30, 9); line(21, 18, 30, 18); line(21, 27, 30, 27);
				line(25, 6, 25, 12); line(27, 15, 27, 21); line(24, 24, 24, 30); break;
			case 20:
				path({{4, 25}, {20, 9}, {25, 14}, {9, 30}, {4, 25}}); line(16, 13, 21, 18);
				line(8, 3, 8, 11); line(4, 7, 12, 7); line(26, 2, 26, 8); line(23, 5, 29, 5); break;
			case 22: // Layered documents distinguish version visibility from upload/create.
				path({{3, 21}, {3, 3}, {18, 3}}); path({{8, 25}, {8, 7}, {23, 7}});
				path({{13, 29}, {13, 11}, {24, 11}, {29, 16}, {29, 29}, {13, 29}});
				line(17, 20, 25, 20); line(17, 25, 25, 25); break;
			case 23: case 24:
				line(4, 16, 28, 16);
				if (command == 23) path({{20, 8}, {28, 16}, {20, 24}});
				else path({{12, 8}, {4, 16}, {12, 24}});
				break;
			case 25:
				path({{4, 10}, {10, 4}, {22, 4}, {28, 10}}); path({{22, 10}, {28, 10}, {28, 4}});
				path({{28, 22}, {22, 28}, {10, 28}, {4, 22}}); path({{4, 28}, {4, 22}, {10, 22}}); break;
			case 26:
				Rectangle(dc, 3, 11, 12, 21); Rectangle(dc, 21, 3, 30, 12); Rectangle(dc, 21, 21, 30, 30);
				path({{12, 16}, {17, 16}, {17, 7}, {21, 7}}); path({{17, 16}, {17, 25}, {21, 25}}); break;
			case 27:
				Ellipse(dc, 4, 3, 16, 15); curve({{2, 29}, {2, 15}, {18, 15}, {18, 29}});
				line(25, 15, 25, 29); line(18, 22, 31, 22); break;
			case 28:
				Ellipse(dc, 3, 3, 15, 15); Ellipse(dc, 18, 7, 28, 17);
				curve({{1, 29}, {1, 15}, {18, 15}, {18, 29}}); curve({{18, 29}, {18, 18}, {31, 18}, {31, 29}}); break;
			case 29:
				path({{10, 3}, {27, 3}, {27, 29}, {10, 29}}); line(2, 16, 22, 16);
				path({{16, 10}, {22, 16}, {16, 22}}); break;
			default:
				Ellipse(dc, 3, 3, 29, 29);
				curve({{11, 11}, {11, 5}, {23, 6}, {21, 13}, {20, 16}, {16, 16}, {16, 20}});
				line(16, 24, 16, 25); break;
			}
			RestoreDC(dc, saved);
			DeleteObject(pen);
		}

		LRESULT DrawToolbar(WindowState& state, NMCUSTOMDRAW& draw) noexcept
		{
			if (draw.dwDrawStage == CDDS_PREPAINT)
			{
				Fill(draw.hdc, draw.rc, GetSysColor(COLOR_WINDOW));
				return CDRF_NOTIFYITEMDRAW;
			}
			if (draw.dwDrawStage != CDDS_ITEMPREPAINT || draw.dwItemSpec < kFirstButton || draw.dwItemSpec >= kFirstButton + kButtonCount)
				return CDRF_DODEFAULT;
			const auto button = static_cast<std::size_t>(draw.dwItemSpec - kFirstButton);
			const bool enabled = SendMessageW(state.toolbar, TB_ISBUTTONENABLED, draw.dwItemSpec, 0) != 0;
			const bool active = enabled && (draw.uItemState & (CDIS_HOT | CDIS_SELECTED | CDIS_CHECKED)) != 0;
			const COLORREF surface = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
			RECT rect = draw.rc;
			InflateRect(&rect, -2, -2);
			const COLORREF background = active ? Blend(GetSysColor(COLOR_HOTLIGHT), surface, 16) : surface;
			Fill(draw.hdc, rect, background);
			const int dpi = static_cast<int>(GetDpiForWindow(state.toolbar)), icon = MulDiv(32, dpi, 96);
			Glyph(draw.hdc, button, (rect.left + rect.right - icon) / 2, rect.top + MulDiv(4, dpi, 96), icon,
				ContrastColour(enabled ? (active ? GetSysColor(COLOR_HOTLIGHT) : text) : Blend(text, surface, 55), background, 3.0));
			rect.top += MulDiv(39, dpi, 96);
			Text(draw.hdc, state.font, kLabels[button], rect, ContrastColour(enabled ? text : Blend(text, surface, 60), background, 4.5));
			if (enabled && SendMessageW(state.toolbar, TB_ISBUTTONCHECKED, draw.dwItemSpec, 0) != 0)
			{
				RECT marker{draw.rc.left + MulDiv(8, dpi, 96), draw.rc.bottom - MulDiv(3, dpi, 96),
					draw.rc.right - MulDiv(8, dpi, 96), draw.rc.bottom - MulDiv(1, dpi, 96)};
				Fill(draw.hdc, marker, ContrastColour(GetSysColor(COLOR_HOTLIGHT), background, 3.0));
			}
			if (GetFocus() == state.toolbar && state.focusedCommand ==
				static_cast<int>(button)) FocusRing(draw.hdc, draw.rc, background, dpi);
			return CDRF_SKIPDEFAULT;
		}

		bool ControlKey(HWND window, UINT message, WPARAM wParam) noexcept
		{
			return (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && HandleKey(window, static_cast<UINT>(wParam),
				(GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0);
		}

		bool RibbonKey(UINT key) noexcept
		{
			return key == VK_TAB || key == VK_RETURN || key == VK_ESCAPE || key == VK_F6;
		}

		LRESULT CALLBACK ToolbarProc(HWND toolbar, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			const HWND window = reinterpret_cast<HWND>(data);
			if (message == WM_NCDESTROY) RemoveWindowSubclass(toolbar, ToolbarProc, id);
			if (message == WM_GETDLGCODE) return DefSubclassProc(toolbar, message, wParam, lParam) | DLGC_WANTARROWS | DLGC_WANTTAB;
			if (ControlKey(window, message, wParam)) return 0;
			if (message == WM_LBUTTONDOWN)
			{
				SetFocus(toolbar);
				POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
				const int item = static_cast<int>(SendMessageW(toolbar, TB_HITTEST, 0, reinterpret_cast<LPARAM>(&point)));
				if (auto* state = State(window); state != nullptr && item >= 0 && item < static_cast<int>(kButtonCount) &&
					SendMessageW(toolbar, TB_ISBUTTONENABLED, kFirstButton + item, 0) != 0) RevealCommand(*state, static_cast<std::size_t>(item));
			}
			const LRESULT result = DefSubclassProc(toolbar, message, wParam, lParam);
			if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(toolbar, nullptr, FALSE);
			return result;
		}

		LRESULT CALLBACK ButtonProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			if (message == WM_GETDLGCODE && GetDlgCtrlID(button) >= static_cast<int>(kFirstWorkflow))
				return DefSubclassProc(button, message, wParam, lParam) | DLGC_WANTARROWS | (RibbonKey(static_cast<UINT>(wParam)) ? DLGC_WANTMESSAGE : 0);
			if (message == WM_GETDLGCODE && RibbonKey(static_cast<UINT>(wParam)))
				return DefSubclassProc(button, message, wParam, lParam) | DLGC_WANTMESSAGE;
			if (ControlKey(reinterpret_cast<HWND>(data), message, wParam)) return 0;
			if (message == WM_MOUSEMOVE)
			{
				TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
				TrackMouseEvent(&track);
			}
			if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS)
				InvalidateRect(button, nullptr, FALSE);
			if (message == WM_NCDESTROY) RemoveWindowSubclass(button, ButtonProc, id);
			return DefSubclassProc(button, message, wParam, lParam);
		}

		void DestroyWorkflowButtons(WindowState& state, std::vector<HWND>& buttons) noexcept
		{
			const MSAAPROPID properties[] = {PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION, PROPID_ACC_KEYBOARDSHORTCUT};
			for (HWND button : buttons)
			{
				if (state.accessibility != nullptr)
					state.accessibility->ClearHwndProps(button, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, ARRAYSIZE(properties));
				DestroyWindow(button);
			}
			buttons.clear();
		}

		void ResetContext(WindowState& state, Tab tab) noexcept
		{
			auto& context = Context(state, tab);
			auto& buttons = ContextButtons(state, tab);
			if (std::find(buttons.begin(), buttons.end(), GetFocus()) != buttons.end()) FocusControl(state, state.tabs);
			DestroyWorkflowButtons(state, buttons);
			context = {};
			if (state.tab == tab)
			{
				DismissPopup(state.window);
				state.tab = Tab::Home;
				state.scroll = 0;
				TabCtrl_SetCurSel(state.tabs, 0);
				if (state.accessibility != nullptr)
					Annotate(state, state.toolbar, L"Home commands", L"Use arrow keys to select a command and Enter or Space to run it.");
			}
			const int index = TabIndex(state.tabs, tab);
			if (index >= 0) TabCtrl_DeleteItem(state.tabs, index);
			TabCtrl_SetCurSel(state.tabs, TabIndex(state.tabs, state.tab));
			LayoutRibbon(state.window, state);
		}

		bool UpdateContext(WindowState& state, Tab tab, ContextState fresh) noexcept
		{
			auto& context = Context(state, tab);
			auto& buttons = ContextButtons(state, tab);
			const UINT firstButton = tab == Tab::Admin ? kFirstAdmin : kFirstWorkflow;
			std::vector<HWND> created;
			try
			{
				if (!fresh.visible) fresh = {};
				if (fresh == context) return true;
				if (fresh.actions.size() > kMaxWorkflowActions) return false;
				const bool rebuild = fresh.visible != context.visible || fresh.rulesEngine != context.rulesEngine ||
					fresh.context != context.context || fresh.actions.size() != context.actions.size() ||
					!std::equal(fresh.actions.begin(), fresh.actions.end(), context.actions.begin(), [](const auto& a, const auto& b) {
						return a.command == b.command && a.label == b.label && a.kind == b.kind;
					});
				if (rebuild)
				{
					created.reserve(fresh.actions.size());
					const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(state.window, GWLP_HINSTANCE));
					for (std::size_t i = 0; i < fresh.actions.size(); ++i)
					{
						HWND button = CreateWindowExW(0, L"BUTTON", fresh.actions[i].label.c_str(), WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
							0, 0, 0, 0, state.page, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(firstButton + i)), instance, nullptr);
						if (button == nullptr) throw std::bad_alloc();
						created.push_back(button);
						if (!SetWindowSubclass(button, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(state.window)) ||
							!Annotate(state, button, fresh.actions[i].label.c_str(), L"ProjectWise contextual action. Use arrows to choose an action; Enter or Space runs it."))
							throw std::bad_alloc();
						SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), FALSE);
					}
				}
				if (fresh.visible && !context.visible)
				{
					TCITEMW item{};
					item.mask = TCIF_TEXT | TCIF_PARAM;
					item.lParam = static_cast<LPARAM>(tab);
					item.pszText = const_cast<LPWSTR>(kTabs[static_cast<int>(tab)]);
					if (TabCtrl_InsertItem(state.tabs, tab == Tab::Workflow ? 4 : TabCtrl_GetItemCount(state.tabs), &item) == -1) throw std::bad_alloc();
				}
				if (rebuild)
				{
					if (std::find(buttons.begin(), buttons.end(), GetFocus()) != buttons.end()) FocusControl(state, state.tabs);
					DestroyWorkflowButtons(state, buttons);
					buttons.swap(created);
				}
				const bool visible = fresh.visible;
				context = std::move(fresh);
				TabCtrl_SetCurSel(state.tabs, TabIndex(state.tabs, state.tab));
				if (!visible) ResetContext(state, tab);
				else
				{
					for (std::size_t i = 0; i < buttons.size(); ++i) EnableWindow(buttons[i], context.actions[i].enabled);
					LayoutRibbon(state.window, state);
					const auto found = std::find(buttons.begin(), buttons.end(), GetFocus());
					const int focused = found == buttons.end() ? -1 : static_cast<int>(found - buttons.begin());
					if (focused >= 0 && !context.actions[focused].enabled)
						if (state.tab == tab && !FocusControl(state, WorkflowEntry(state))) FocusControl(state, state.tabs);
				}
				if (rebuild) NotifyWinEvent(EVENT_OBJECT_REORDER, state.tabs, OBJID_CLIENT, CHILDID_SELF);
				return true;
			}
			catch (...)
			{
				DestroyWorkflowButtons(state, created);
				return false;
			}
		}

		void DrawButton(const WindowState& state, const DRAWITEMSTRUCT& draw) noexcept
		{
			const bool navigation = draw.CtlID >= kFirstNavigation && draw.CtlID < kFirstNavigation + kNavigationCount;
			POINT cursor{};
			GetCursorPos(&cursor);
			ScreenToClient(draw.hwndItem, &cursor);
			const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
			const bool hot = enabled && PtInRect(&draw.rcItem, cursor);
			const COLORREF face = GetSysColor(COLOR_BTNFACE), text = GetSysColor(COLOR_BTNTEXT);
			const COLORREF background = hot || (draw.itemState & ODS_SELECTED) != 0 ? Blend(GetSysColor(COLOR_HOTLIGHT), face, 18) : face;
			Fill(draw.hDC, draw.rcItem, background);
			const COLORREF foreground = ContrastColour(enabled ? text : Blend(text, face, 60), background, 4.5);
			const Tab contextTab = draw.CtlID >= kFirstAdmin ? Tab::Admin : Tab::Workflow;
			const UINT firstAction = contextTab == Tab::Admin ? kFirstAdmin : kFirstWorkflow;
			if (draw.CtlID >= firstAction && draw.CtlID - firstAction < Context(state, contextTab).actions.size())
			{
				const auto& action = Context(state, contextTab).actions[draw.CtlID - firstAction];
				const int dpi = static_cast<int>(GetDpiForWindow(draw.hwndItem)), icon = MulDiv(32, dpi, 96);
				Glyph(draw.hDC, kButtonCount + static_cast<std::size_t>(action.kind), (draw.rcItem.right - icon) / 2,
					draw.rcItem.top + MulDiv(4, dpi, 96), icon, ContrastColour(foreground, background, 3.0));
				RECT label = draw.rcItem;
				InflateRect(&label, -MulDiv(6, dpi, 96), -MulDiv(4, dpi, 96));
				label.top += MulDiv(38, dpi, 96);
				Text(draw.hDC, state.font, action.label.c_str(), label, foreground, DT_CENTER | DT_WORDBREAK);
			}
			else if (navigation)
			{
				const int dpi = static_cast<int>(GetDpiForWindow(draw.hwndItem));
				const int x = (draw.rcItem.left + draw.rcItem.right) / 2, y = (draw.rcItem.top + draw.rcItem.bottom) / 2, d = MulDiv(5, dpi, 96);
				HPEN pen = CreatePen(PS_SOLID, std::max(1, MulDiv(2, dpi, 96)), foreground);
				if (pen != nullptr)
				{
					const auto old = SelectObject(draw.hDC, pen);
					const UINT index = draw.CtlID - kFirstNavigation;
					if (index == 2)
					{
						MoveToEx(draw.hDC, x - d, y, nullptr); LineTo(draw.hDC, x, y - d); LineTo(draw.hDC, x + d, y);
						MoveToEx(draw.hDC, x, y - d, nullptr); LineTo(draw.hDC, x, y + d + 1);
					}
					else
					{
						const int direction = index == 0 ? -1 : 1;
						MoveToEx(draw.hDC, x, y - d, nullptr); LineTo(draw.hDC, x + direction * d, y); LineTo(draw.hDC, x, y + d);
						MoveToEx(draw.hDC, x + direction * d, y, nullptr); LineTo(draw.hDC, x - direction * d, y);
					}
					SelectObject(draw.hDC, old);
					DeleteObject(pen);
				}
			}
			else
			{
				wchar_t label[32]{};
				GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
				Text(draw.hDC, state.font, label, draw.rcItem, foreground);
			}
			if ((draw.itemState & ODS_FOCUS) != 0)
				FocusRing(draw.hDC, draw.rcItem, background, static_cast<int>(GetDpiForWindow(draw.hwndItem)));
		}

		void PaintTabs(HWND tabs, HDC dc, const WindowState& state) noexcept
		{
			RECT rect{};
			GetClientRect(tabs, &rect);
			const COLORREF face = GetSysColor(COLOR_BTNFACE), surface = GetSysColor(COLOR_WINDOW);
			Fill(dc, rect, face);
			const int dpi = static_cast<int>(GetDpiForWindow(tabs));
			const int header = MulDiv(32, dpi, 96);
			RECT body{0, header, rect.right, rect.bottom};
			Fill(dc, body, surface);
			RECT rule{0, header - 1, rect.right, header};
			Fill(dc, rule, Blend(GetSysColor(COLOR_BTNTEXT), face, 18));
			for (int i = 0; i < TabCtrl_GetItemCount(tabs); ++i)
			{
				RECT tab{};
				TabCtrl_GetItemRect(tabs, i, &tab);
				tab.top = 0;
				tab.bottom = header;
				const bool selected = TabAt(tabs, i) == state.tab;
				if (selected)
				{
					Fill(dc, tab, surface);
					RECT accent{tab.left + MulDiv(12, dpi, 96), header - MulDiv(3, dpi, 96), tab.right - MulDiv(12, dpi, 96), header};
					Fill(dc, accent, ContrastColour(GetSysColor(COLOR_HOTLIGHT), surface, 3.0));
				}
				Text(dc, state.font, kTabs[static_cast<int>(TabAt(tabs, i))], tab, ContrastColour(GetSysColor(COLOR_BTNTEXT), selected ? surface : face, 4.5));
				if (GetFocus() == tabs && selected)
					FocusRing(dc, tab, surface, dpi);
			}
		}

		void PaintGroups(HWND page, HDC dc, const WindowState& state) noexcept
		{
			struct Group { std::size_t first, end; const wchar_t* label; };
			constexpr Group groups[] = {{0, 2, L"Create"}, {2, 6, L"Document control"}, {6, 9, L"Open & refresh"},
				{9, 14, L"Layout"}, {14, 16, L"Configuration"}, {16, 18, L"Find"}, {18, 21, L"Administration"}, {21, 22, L"Help"},
				{kShowAllVersions, kShowAllVersions + 1, L"Versions"}};
			RECT client{};
			GetClientRect(page, &client);
			Fill(dc, client, GetSysColor(COLOR_WINDOW));
			const int dpi = static_cast<int>(GetDpiForWindow(page));
			if (Contextual(state.tab))
			{
				RECT caption{0, client.bottom - MulDiv(24, dpi, 96), client.right, client.bottom};
				Text(dc, state.font, state.tab == Tab::Admin ? L"Datasource administration" : state.workflow.actions.empty() ? L"Select documents to see workflow actions" :
					state.workflow.rulesEngine ? L"Rules Engine operations" : L"Workflow transitions", caption,
					ContrastColour(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 4.5));
				return;
			}
			RECT toolbar{};
			GetWindowRect(state.toolbar, &toolbar);
			MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&toolbar), 2);
			for (const auto& group : groups)
			{
				if (ButtonTab(group.first) != state.tab) continue;
				RECT first{}, last{};
				SendMessageW(state.toolbar, TB_GETITEMRECT, group.first, reinterpret_cast<LPARAM>(&first));
				const auto lastIndex = HasSelectors(state) && group.end - 1 == kInterfaceDialog ? group.end - 2 : group.end - 1;
				SendMessageW(state.toolbar, TB_GETITEMRECT, lastIndex, reinterpret_cast<LPARAM>(&last));
				RECT caption{toolbar.left + first.left, toolbar.bottom, toolbar.left + last.right, client.bottom};
				Text(dc, state.font, group.label, caption, ContrastColour(Blend(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 72), GetSysColor(COLOR_WINDOW), 4.5));
				RECT divider{caption.right - 1, MulDiv(8, dpi, 96), caption.right, client.bottom - MulDiv(6, dpi, 96)};
				Fill(dc, divider, Blend(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 15));
			}
			if (state.tab == Tab::View && HasSelectors(state))
			{
				RECT caption{toolbar.right + MulDiv(12, dpi, 96), toolbar.bottom, client.right, client.bottom};
				Text(dc, state.font, L"Interface & View", caption, ContrastColour(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 4.5));
			}
			if (state.tab == Tab::Search)
			{
				RECT caption{toolbar.right + MulDiv(12, dpi, 96), toolbar.bottom, client.right, client.bottom};
				Text(dc, state.font, L"Simple search", caption, ContrastColour(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 4.5));
			}
		}

		LRESULT CALLBACK PanelProc(HWND panel, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			const HWND window = reinterpret_cast<HWND>(data);
			const auto* state = State(window);
			if (message == WM_NCDESTROY) RemoveWindowSubclass(panel, PanelProc, id);
			if (message == WM_CTLCOLORSTATIC && state != nullptr && panel == state->page)
			{
				const HDC dc = reinterpret_cast<HDC>(wParam);
				SetTextColor(dc, ContrastColour(GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), 4.5));
				SetBkColor(dc, GetSysColor(COLOR_WINDOW));
				return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
			}
			if (message == WM_COMMAND || message == WM_NOTIFY || message == WM_DRAWITEM)
				return SendMessageW(window, message, wParam, lParam);
			if (message == WM_ERASEBKGND) return 1;
			if (message == WM_PRINTCLIENT && state != nullptr)
			{
				HDC dc = reinterpret_cast<HDC>(wParam);
				if (panel == state->page) PaintGroups(panel, dc, *state);
				else { RECT rect{}; GetClientRect(panel, &rect); Fill(dc, rect, GetSysColor(COLOR_BTNFACE)); }
				return 0;
			}
			if (message == WM_PAINT && state != nullptr)
			{
				PAINTSTRUCT paint{};
				HDC dc = BeginPaint(panel, &paint);
				if (panel == state->page) PaintGroups(panel, dc, *state);
				else
				{
					RECT rect{};
					GetClientRect(panel, &rect);
					Fill(dc, rect, GetSysColor(COLOR_BTNFACE));
					rect.bottom = 1;
					Fill(dc, rect, Blend(GetSysColor(COLOR_BTNTEXT), GetSysColor(COLOR_BTNFACE), 18));
				}
				EndPaint(panel, &paint);
				return 0;
			}
			return DefSubclassProc(panel, message, wParam, lParam);
		}

		int ReservedHeight(HWND window, const WindowState& state) noexcept
		{
			if (!IsWindowVisible(window)) return 0;
			const int dpi = static_cast<int>(GetDpiForWindow(window));
			const int navigation = state.navigation != nullptr ? MulDiv(44, dpi, 96) : 0;
			if (state.mode == DisplayMode::Hidden) return navigation;
			return state.mode == DisplayMode::Minimized ? MulDiv(32, dpi, 96) + navigation : state.dockHeight;
		}

		LRESULT CALLBACK PopupMessages(int code, WPARAM wParam, LPARAM lParam)
		{
			if (code >= 0 && wParam == PM_REMOVE && g_popup != nullptr)
			{
				auto& message = *reinterpret_cast<MSG*>(lParam);
				bool escape = message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE;
				const auto* state = State(g_popup);
				bool comboPopup = state != nullptr && ComboPopupContains(state->address, message.hwnd);
				if (state != nullptr) for (const HWND combo : state->selectors) comboPopup = comboPopup || ComboPopupContains(combo, message.hwnd);
				if (escape && comboPopup) escape = false;
				if (escape && state != nullptr && state->address != nullptr &&
					(message.hwnd == state->address || IsChild(state->address, message.hwnd)) &&
					SendMessageW(state->address, CB_GETDROPPEDSTATE, 0, 0) != 0) escape = false;
				const bool click = message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
					message.message == WM_MBUTTONDOWN || message.message == WM_NCLBUTTONDOWN;
				if (escape || (click && message.hwnd != g_popup && !IsChild(g_popup, message.hwnd) && !comboPopup))
				{
					DismissPopup(g_popup);
					if (escape) message.message = WM_NULL;
				}
			}
			return CallNextHookEx(nullptr, code, wParam, lParam);
		}

		LRESULT CALLBACK TabProc(HWND tabs, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			const HWND window = reinterpret_cast<HWND>(data);
			if (message == WM_GETDLGCODE) return DefSubclassProc(tabs, message, wParam, lParam) | DLGC_WANTARROWS | DLGC_WANTTAB;
			if (ControlKey(window, message, wParam)) return 0;
			if (message == WM_ERASEBKGND) return 1;
			if (message == WM_PAINT)
			{
				PAINTSTRUCT paint{};
				HDC dc = BeginPaint(tabs, &paint);
				if (const auto* state = State(window)) PaintTabs(tabs, dc, *state);
				EndPaint(tabs, &paint);
				return 0;
			}
			if (message == WM_PRINTCLIENT)
			{
				if (const auto* state = State(window)) PaintTabs(tabs, reinterpret_cast<HDC>(wParam), *state);
				return 0;
			}
			if (message == WM_NCDESTROY) RemoveWindowSubclass(tabs, TabProc, id);
			const LRESULT result = DefSubclassProc(tabs, message, wParam, lParam);
			if (message == WM_LBUTTONUP && GetDisplayMode(window) == DisplayMode::Minimized)
				SelectTab(window, TabAt(tabs, TabCtrl_GetCurSel(tabs)));
			if (message == WM_LBUTTONDBLCLK)
				SetDisplayMode(window, GetDisplayMode(window) == DisplayMode::Minimized ? DisplayMode::Expanded : DisplayMode::Minimized);
			if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_KEYDOWN) InvalidateRect(tabs, nullptr, FALSE);
			return result;
		}

		LRESULT CALLBACK SearchTextProc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, SearchTextProc, id);
			if (message == WM_GETDLGCODE && RibbonKey(static_cast<UINT>(wParam)))
				return DefSubclassProc(edit, message, wParam, lParam) | DLGC_WANTMESSAGE;
			if (ControlKey(reinterpret_cast<HWND>(data), message, wParam)) return 0;
			if (message == WM_CHAR && wParam == VK_RETURN) return 0;
			return DefSubclassProc(edit, message, wParam, lParam);
		}

		LRESULT CALLBACK SelectorProc(HWND combo, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
		{
			const HWND window = reinterpret_cast<HWND>(data);
			if (message == WM_NCDESTROY) RemoveWindowSubclass(combo, SelectorProc, id);
			if (message == WM_GETDLGCODE)
			{
				const auto* key = reinterpret_cast<MSG*>(lParam);
				return DefSubclassProc(combo, message, wParam, lParam) | DLGC_WANTARROWS |
					(key != nullptr && (key->wParam == VK_RETURN || key->wParam == VK_TAB || key->wParam == VK_ESCAPE) ? DLGC_WANTMESSAGE : 0);
			}
			if (message == WM_SETFOCUS) if (auto* state = State(window)) RevealControl(*state, combo);
			if (ControlKey(window, message, wParam)) return 0;
			return DefSubclassProc(combo, message, wParam, lParam);
		}

		bool ArrangeDock(HWND window, WindowState& state) noexcept
		{
			const int height = ReservedHeight(window, state);
			state.arranging = true;
			const bool moved = SetWindowPos(state.dockClient, nullptr, state.dockRect.left, state.dockRect.top + height,
				state.dockRect.right - state.dockRect.left, std::max(0L, state.dockRect.bottom - state.dockRect.top - height),
				SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
			state.arranging = false;
			return moved && SetWindowPos(window, HWND_TOP, state.dockRect.left, state.dockRect.top,
				state.dockRect.right - state.dockRect.left, state.dockHeight, SWP_NOACTIVATE) != FALSE;
		}

		LRESULT CALLBACK DockClientProc(HWND client, UINT message, WPARAM wParam, LPARAM lParam,
			UINT_PTR id, DWORD_PTR data)
		{
			const HWND window = reinterpret_cast<HWND>(data);
			auto* state = State(window);
			if (message == WM_NCDESTROY)
			{
				RemoveWindowSubclass(client, DockClientProc, id);
				if (state != nullptr) state->dockClient = nullptr;
			}
			else if (message == WM_WINDOWPOSCHANGING && state != nullptr && !state->arranging)
			{
				auto& pos = *reinterpret_cast<WINDOWPOS*>(lParam);
				if ((pos.flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
				{
					const int x = (pos.flags & SWP_NOMOVE) ? state->dockRect.left : pos.x;
					const int y = (pos.flags & SWP_NOMOVE) ? state->dockRect.top : pos.y;
					const int width = (pos.flags & SWP_NOSIZE) ? state->dockRect.right - state->dockRect.left : pos.cx;
					const int height = (pos.flags & SWP_NOSIZE) ? state->dockRect.bottom - state->dockRect.top : pos.cy;
					state->dockRect = {x, y, x + width, y + height};
					const int reserved = ReservedHeight(window, *state);
					pos.x = x;
					pos.y = y + reserved;
					pos.cx = width;
					pos.cy = std::max(0, height - reserved);
					pos.flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
					if (!SetWindowPos(window, nullptr, x, y, width, state->dockHeight, SWP_NOZORDER | SWP_NOACTIVATE))
						Trace(L"could not update the docked toolbar layout");
				}
			}
			const LRESULT result = DefSubclassProc(client, message, wParam, lParam);
			if (message == WM_WINDOWPOSCHANGED && state != nullptr && State(window) == state && !state->arranging &&
				!LayoutRibbon(window, *state)) Trace(L"could not relayout the ribbon after Explorer resized");
			return result;
		}

		HWND DockClient(HWND owner) noexcept
		{
			// MFC reserves AFX_IDW_PANE_FIRST for the frame's client/splitter pane.
			HWND client = GetDlgItem(owner, 0xE900);
			if (IsWindow(client) && GetParent(client) == owner) return client;
			RECT frame{};
			GetClientRect(owner, &frame);
			LONG best = 0;
			for (HWND child = GetWindow(owner, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
			{
				if (!IsWindowVisible(child)) continue;
				RECT rect{};
				GetWindowRect(child, &rect);
				const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
				if (width < frame.right / 2 || height < frame.bottom / 2) continue;
				if (width * height > best)
				{
					best = width * height;
					client = child;
				}
			}
			return client;
		}

		bool LayoutRibbon(HWND window, WindowState& state) noexcept
		{
			if (state.layoutBusy) return true;
			state.layoutBusy = true;
			const UINT dpi = GetDpiForWindow(window);
			const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
			const bool hidden = state.mode == DisplayMode::Hidden;
			const int header = hidden ? 0 : scale(32), grip = state.dockClient != nullptr && !hidden ? scale(12) : 0;
			const int navigationHeight = state.navigation != nullptr ? scale(44) : 0;
			const bool commands = !hidden && (state.mode == DisplayMode::Expanded || state.popup);
			const bool workflow = Contextual(state.tab);
			for (std::size_t i = 0; i < kButtonCount; ++i)
				SendMessageW(state.toolbar, TB_HIDEBUTTON, kFirstButton + i, MAKELPARAM(!ButtonOnPage(state, i), 0));
			SendMessageW(state.toolbar, TB_AUTOSIZE, 0, 0);
			SIZE size{};
			bool success = SendMessageW(state.toolbar, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&size)) != 0;
			if (workflow) size = {scale(160) * static_cast<LONG>(std::max<std::size_t>(1, ContextButtons(state, state.tab).size())), scale(80)};
			const bool search = state.tab == Tab::Search;
			const bool selectors = state.tab == Tab::View && HasSelectors(state);
			const bool classic = state.tab == Tab::Tools && state.classic != nullptr;
			const int contentWidth = size.cx + (search ? scale(420) : selectors ? scale(372) : classic ? scale(212) : 0);
			const int width = state.dockClient != nullptr ? state.dockRect.right - state.dockRect.left :
				std::max(contentWidth + grip + scale(8), scale(800));
			const int available = std::max(1, width - grip - scale(8));
			const bool overflow = commands && contentWidth > available;
			state.scroll = std::clamp(state.scroll, 0, std::max(0, contentWidth - available));
			SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
			info.nMax = commands ? contentWidth - 1 : 0;
			info.nPage = static_cast<UINT>(available);
			info.nPos = state.scroll;
			SetScrollInfo(window, SB_HORZ, &info, TRUE);
			ShowScrollBar(window, SB_HORZ, overflow);
			const int modeWidth = std::min(scale(76), std::max(scale(40), (width - grip) / 7));
			const int tabsWidth = std::max(1, width - grip);
			const int commandHeight = commands ? size.cy + scale(28) : 0;
			const int pageTop = header + (state.popup ? navigationHeight : 0);
			TabCtrl_SetItemSize(state.tabs, std::min(scale(80), std::max(scale(24), (tabsWidth - 3 * modeWidth - scale(4)) / TabCtrl_GetItemCount(state.tabs))), scale(32));
			success = SetWindowPos(state.tabs, nullptr, grip, 0, tabsWidth, pageTop + commandHeight, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			success = SetWindowPos(state.page, nullptr, scale(4), pageTop, available, commandHeight,
				SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			ShowWindow(state.tabs, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
			ShowWindow(state.page, commands ? SW_SHOWNOACTIVATE : SW_HIDE);
			for (std::size_t i = 0; i < state.modeButtons.size(); ++i)
			{
				success = SetWindowPos(state.modeButtons[i], HWND_TOP, width - 3 * modeWidth + static_cast<int>(i) * modeWidth, 0,
					modeWidth, header, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				ShowWindow(state.modeButtons[i], hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
			}
			const int x = -state.scroll;
			success = SetWindowPos(state.toolbar, nullptr, x, scale(4), size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			ShowWindow(state.toolbar, commands && !workflow ? SW_SHOWNOACTIVATE : SW_HIDE);
			for (const Tab context : {Tab::Workflow, Tab::Admin}) for (std::size_t i = 0; i < ContextButtons(state, context).size(); ++i)
			{
				const HWND button = ContextButtons(state, context)[i];
				success = SetWindowPos(button, nullptr, x + scale(160) * static_cast<int>(i), scale(4), scale(160), scale(80),
					SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				ShowWindow(button, commands && state.tab == context ? SW_SHOWNOACTIVATE : SW_HIDE);
			}
			success = SetWindowPos(state.searchText, nullptr, x + size.cx + scale(12), scale(20), scale(280), scale(28),
				SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			success = SetWindowPos(state.searchButton, nullptr, x + size.cx + scale(300), scale(20), scale(112), scale(28),
				SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			ShowWindow(state.searchText, commands && search ? SW_SHOWNOACTIVATE : SW_HIDE);
			ShowWindow(state.searchButton, commands && search ? SW_SHOWNOACTIVATE : SW_HIDE);
			if (state.classic != nullptr)
			{
				success = SetWindowPos(state.classic, nullptr, x + size.cx + scale(12), scale(20), scale(192), scale(28), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				ShowWindow(state.classic, commands && classic ? SW_SHOWNOACTIVATE : SW_HIDE);
			}
			for (std::size_t i = 0; i < state.selectors.size(); ++i)
			{
				if (state.selectors[i] == nullptr) continue;
				const int top = scale(4 + static_cast<int>(i) * 32);
				success = SetWindowPos(state.selectorLabels[i], nullptr, x + size.cx + scale(12), top + scale(3), scale(76), scale(24), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				success = SetWindowPos(state.selectors[i], nullptr, x + size.cx + scale(92), top, scale(268), scale(260), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				ShowWindow(state.selectorLabels[i], commands && selectors ? SW_SHOWNOACTIVATE : SW_HIDE);
				ShowWindow(state.selectors[i], commands && selectors ? SW_SHOWNOACTIVATE : SW_HIDE);
			}
			if (state.navigation != nullptr)
			{
				const int navigationTop = state.mode == DisplayMode::Expanded ? header + commandHeight : header;
				success = SetWindowPos(state.navigation, HWND_TOP, 0, navigationTop, width, navigationHeight, SWP_NOACTIVATE) != FALSE && success;
				for (std::size_t i = 0; i < state.navigationButtons.size(); ++i)
					success = SetWindowPos(state.navigationButtons[i], nullptr, scale(8 + static_cast<int>(i) * 36), scale(6),
						scale(32), scale(32), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
				success = SetWindowPos(state.address, nullptr, scale(124), scale(9), std::max(1, width - scale(136)), scale(240),
					SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			}
			state.dockHeight = header + commandHeight + navigationHeight + (overflow ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi) : 0);
			if (state.dockClient != nullptr) success = ArrangeDock(window, state) && success;
			else
			{
				RECT rect{0, 0, width, state.dockHeight - (overflow ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi) : 0)};
				success = AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
					static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)), dpi) != FALSE && success;
				success = SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
					SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE && success;
			}
			EnableWindow(state.modeButtons[0], state.mode != DisplayMode::Expanded);
			EnableWindow(state.modeButtons[1], state.mode != DisplayMode::Minimized);
			RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
			state.layoutBusy = false;
			if (!success) RequestRecovery(window, L"ribbon layout failed");
			return success;
		}

		bool SizeToolbar(HWND window, WindowState& state) noexcept
		{
			const UINT dpi = GetDpiForWindow(window);
			const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
			const int iconSize = scale(32);
			HIMAGELIST images = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, static_cast<int>(kButtonCount), 0);
			if (images == nullptr) return false;
			BITMAPINFO bitmapInfo{};
			bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bitmapInfo.bmiHeader.biWidth = iconSize;
			bitmapInfo.bmiHeader.biHeight = -iconSize;
			bitmapInfo.bmiHeader.biPlanes = 1;
			bitmapInfo.bmiHeader.biBitCount = 32;
			void* bits = nullptr;
			HDC dc = CreateCompatibleDC(nullptr);
			HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (dc == nullptr || bitmap == nullptr)
			{
				if (dc != nullptr) DeleteDC(dc);
				if (bitmap != nullptr) DeleteObject(bitmap);
				ImageList_Destroy(images);
				return false;
			}
			const auto oldBitmap = SelectObject(dc, bitmap);
			bool complete = true;
			for (std::size_t i = 0; i < kButtonCount && complete; ++i)
			{
				const RECT rect{0, 0, iconSize, iconSize};
				Fill(dc, rect, RGB(255, 0, 255));
				Glyph(dc, i, 0, 0, iconSize, GetSysColor(COLOR_HOTLIGHT));
				complete = ImageList_AddMasked(images, bitmap, RGB(255, 0, 255)) != -1;
			}
			SelectObject(dc, oldBitmap);
			DeleteObject(bitmap);
			DeleteDC(dc);
			if (!complete) { ImageList_Destroy(images); return false; }
			NONCLIENTMETRICSW metrics{sizeof(metrics)};
			if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
			{
				ImageList_Destroy(images);
				return false;
			}
			metrics.lfMessageFont.lfHeight = std::min<LONG>(metrics.lfMessageFont.lfHeight, -MulDiv(11, static_cast<int>(dpi), 72));
			HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
			if (font == nullptr)
			{
				ImageList_Destroy(images);
				return false;
			}
			SendMessageW(state.toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
			SendMessageW(state.toolbar, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			SendMessageW(state.tabs, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			for (HWND button : state.modeButtons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			SendMessageW(state.searchText, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			SendMessageW(state.searchButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			if (state.address != nullptr)
			{
				SendMessageW(state.address, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
				SendMessageW(state.address, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
			}
			for (HWND button : state.navigationButtons)
				if (button != nullptr) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			for (const Tab tab : {Tab::Workflow, Tab::Admin}) for (HWND button : ContextButtons(state, tab)) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			if (state.classic != nullptr) SendMessageW(state.classic, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			for (std::size_t i = 0; i < state.selectors.size(); ++i)
			{
				if (state.selectors[i] == nullptr) continue;
				SendMessageW(state.selectorLabels[i], WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
				SendMessageW(state.selectors[i], WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
				SendMessageW(state.selectors[i], CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
			}
			if (state.images != nullptr) ImageList_Destroy(state.images);
			if (state.font != nullptr) DeleteObject(state.font);
			state.images = images;
			state.font = font;
			SendMessageW(state.toolbar, TB_SETBUTTONWIDTH, 0, MAKELPARAM(scale(112), scale(112)));
			SendMessageW(state.toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(scale(112), scale(64)));
			TBBUTTONINFOW versions{sizeof(versions)};
			versions.dwMask = TBIF_SIZE;
			versions.cx = static_cast<WORD>(scale(160));
			if (!SendMessageW(state.toolbar, TB_SETBUTTONINFOW, kFirstButton + kShowAllVersions, reinterpret_cast<LPARAM>(&versions))) return false;
			return LayoutRibbon(window, state);
		}

		bool Initialise(HWND window, WindowState& state) noexcept
		{
			INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
			if (!InitCommonControlsEx(&controls)) return false;
			const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
			state.tabs = CreateWindowExW(WS_EX_CONTROLPARENT, WC_TABCONTROLW, L"Ribbon tabs", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | TCS_FIXEDWIDTH,
				0, 0, 0, 0, window, nullptr, instance, nullptr);
			if (state.tabs == nullptr || !SetWindowSubclass(state.tabs, TabProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			state.page = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"Ribbon commands", WS_CHILD | WS_CLIPCHILDREN,
				0, 0, 0, 0, state.tabs, reinterpret_cast<HMENU>(kCommandPage), instance, nullptr);
			if (state.page == nullptr || !SetWindowSubclass(state.page, PanelProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			for (int i = 0; i < 4; ++i)
			{
				TCITEMW item{};
				item.mask = TCIF_TEXT | TCIF_PARAM;
				item.lParam = i;
				item.pszText = const_cast<LPWSTR>(kTabs[i]);
				if (TabCtrl_InsertItem(state.tabs, i, &item) == -1) return false;
			}
			constexpr const wchar_t* labels[] = {L"Maximise", L"Minimise", L"Hide"};
			for (std::size_t i = 0; i < state.modeButtons.size(); ++i)
			{
				state.modeButtons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
					0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kMaximise + i)), instance, nullptr);
				if (state.modeButtons[i] == nullptr || !SetWindowSubclass(state.modeButtons[i], ButtonProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			}
			state.searchText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
				0, 0, 0, 0, state.page, reinterpret_cast<HMENU>(kSearchText), instance, nullptr);
			state.searchButton = CreateWindowExW(0, L"BUTTON", L"Start Search", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
				0, 0, 0, 0, state.page, reinterpret_cast<HMENU>(kSearch), instance, nullptr);
			if (state.searchText == nullptr || state.searchButton == nullptr ||
				!SetWindowSubclass(state.searchText, SearchTextProc, 1, reinterpret_cast<DWORD_PTR>(window)) ||
				!SetWindowSubclass(state.searchButton, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			SendMessageW(state.searchText, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search ProjectWise"));
			SendMessageW(state.searchText, EM_SETLIMITTEXT, 1024, 0);
			state.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
				0, 0, 0, 0, state.page, nullptr, instance, nullptr);
			if (state.toolbar == nullptr || !SetWindowSubclass(state.toolbar, ToolbarProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			SendMessageW(state.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
			SendMessageW(state.toolbar, TB_SETMAXTEXTROWS, 1, 0);
			std::array<TBBUTTON, kButtonCount> buttons{};
			for (std::size_t i = 0; i < buttons.size(); ++i)
			{
				auto& button = buttons[i];
				button.iBitmap = static_cast<int>(i);
				button.idCommand = kFirstButton + static_cast<int>(i);
				button.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
				if (i == kShowAllVersions) button.fsStyle |= BTNS_CHECK;
				button.iString = reinterpret_cast<INT_PTR>(kLabels[i]);
			}
			if (!SendMessageW(state.toolbar, TB_ADDBUTTONS, buttons.size(), reinterpret_cast<LPARAM>(buttons.data()))) return false;
			if (state.host.restoreClassic != nullptr)
			{
				state.classic = CreateWindowExW(0, L"BUTTON", L"Re-enable Classic UI", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
					0, 0, 0, 0, state.page, reinterpret_cast<HMENU>(kClassic), instance, nullptr);
				if (state.classic == nullptr || !SetWindowSubclass(state.classic, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
			}
			if (HasSelectors(state))
			{
				constexpr const wchar_t* selectorNames[] = {L"Interface", L"View"};
				for (std::size_t i = 0; i < state.selectors.size(); ++i)
				{
					state.selectorLabels[i] = CreateWindowExW(0, L"STATIC", selectorNames[i], WS_CHILD | SS_LEFT, 0, 0, 0, 0, state.page, nullptr, instance, nullptr);
					state.selectors[i] = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
						0, 0, 268, 260, state.page, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kFirstSelector + i)), instance, nullptr);
					if (state.selectorLabels[i] == nullptr || state.selectors[i] == nullptr ||
						!SetWindowSubclass(state.selectors[i], SelectorProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
				}
			}
			if (state.host.createAddress != nullptr)
			{
				state.navigation = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"ProjectWise navigation", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
					0, 0, 0, 0, window, reinterpret_cast<HMENU>(kNavigationRow), instance, nullptr);
				if (state.navigation == nullptr || !SetWindowSubclass(state.navigation, PanelProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
				constexpr const wchar_t* navigationLabels[] = {L"Back", L"Forward", L"Up"};
				for (std::size_t i = 0; i < state.navigationButtons.size(); ++i)
				{
					state.navigationButtons[i] = CreateWindowExW(0, L"BUTTON", navigationLabels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
						0, 0, 0, 0, state.navigation, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kFirstNavigation + i)), instance, nullptr);
					if (state.navigationButtons[i] == nullptr || !SetWindowSubclass(state.navigationButtons[i], ButtonProc, 1, reinterpret_cast<DWORD_PTR>(window))) return false;
				}
				state.address = state.host.createAddress(state.host.context, state.navigation);
				if (!IsWindow(state.address) || GetParent(state.address) != state.navigation)
				{
					Trace(L"could not create the native ProjectWise navigation address control");
					return false;
				}
			}
			if (!SizeToolbar(window, state) || !InitialiseAccessibility(state) || !RegisterKeyboard(state)) return false;
			if (SetTimer(window, 1, 500, nullptr) == 0) return false;
			Refresh(window);
			return true;
		}

		LRESULT WindowProcImpl(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			auto* state = State(window);
			switch (message)
			{
			case WM_NCCREATE:
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
				State(window)->window = window;
				return TRUE;
			case WM_CREATE:
				return state != nullptr && Initialise(window, *state) ? 0 : -1;
			case WM_MOUSEACTIVATE:
				// Capture Explorer's selection context before an interactive floating control takes focus.
				Refresh(window);
				return MA_ACTIVATE;
			case WM_CONTEXTMENU:
			{
				HMENU menu = CreatePopupMenu();
				if (menu == nullptr) { Trace(L"could not create docking menu"); return 0; }
				AppendMenuW(menu, MF_STRING | (IsDocked(window) ? MF_CHECKED : 0), 1, L"Dock at top");
				AppendMenuW(menu, MF_STRING | (!IsDocked(window) ? MF_CHECKED : 0), 2, L"Float");
				AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
				AppendMenuW(menu, MF_STRING, kMaximise, L"Maximise ribbon");
				AppendMenuW(menu, MF_STRING, kMinimise, L"Minimise ribbon");
				AppendMenuW(menu, MF_STRING, kHide, L"Hide ribbon");
				POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
				if (point.x == -1 && point.y == -1)
				{
					const HWND focus = GetFocus();
					RECT target{};
					if (state != nullptr && focus == state->toolbar && state->focusedCommand >= 0)
					{
						SendMessageW(focus, TB_GETITEMRECT, state->focusedCommand, reinterpret_cast<LPARAM>(&target));
						MapWindowPoints(focus, nullptr, reinterpret_cast<POINT*>(&target), 2);
					}
					else GetWindowRect(focus != nullptr ? focus : window, &target);
					point = {target.left, target.bottom};
				}
				const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
					point.x, point.y, 0, window, nullptr);
				DestroyMenu(menu);
				if (command == kMaximise || command == kMinimise || command == kHide)
					SetDisplayMode(window, command == kMaximise ? DisplayMode::Expanded :
						command == kMinimise ? DisplayMode::Minimized : DisplayMode::Hidden);
				else if (command != 0 && !Dock(window, command == 1))
					MessageBoxW(window, L"The toolbar could not change docking mode. See the debug output for details.",
						L"ProjectWise Quick Access", MB_OK | MB_ICONERROR);
				return 0;
			}
			case WM_NCLBUTTONDBLCLK:
				if (wParam == HTCAPTION) { Dock(window, true); return 0; }
				break;
			case WM_LBUTTONDOWN:
				if (IsDocked(window) && static_cast<short>(LOWORD(lParam)) >= 0 &&
					static_cast<short>(LOWORD(lParam)) < MulDiv(12, static_cast<int>(GetDpiForWindow(window)), 96) && Dock(window, false))
				{
					ReleaseCapture();
					POINT cursor{};
					GetCursorPos(&cursor);
					SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
				}
				return 0;
			case WM_PAINT:
			{
				PAINTSTRUCT paint{};
				HDC dc = BeginPaint(window, &paint);
				RECT client{};
				GetClientRect(window, &client);
				Fill(dc, client, GetSysColor(COLOR_BTNFACE));
				if (state != nullptr && state->dockClient != nullptr && state->mode != DisplayMode::Hidden)
				{
					const int dpi = static_cast<int>(GetDpiForWindow(window));
					for (int y = 10; y < 25; y += 5)
					{
						RECT dot{MulDiv(5, dpi, 96), MulDiv(y, dpi, 96), MulDiv(7, dpi, 96), MulDiv(y + 2, dpi, 96)};
						Fill(dc, dot, GetSysColor(COLOR_GRAYTEXT));
					}
				}
				EndPaint(window, &paint);
				return 0;
			}
			case WM_ERASEBKGND:
				return 1;
			case WM_DRAWITEM:
				if (state != nullptr && reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON)
				{
					DrawButton(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
					return TRUE;
				}
				break;
			case WM_EXITSIZEMOVE:
				if (state != nullptr && !IsDocked(window))
				{
					RECT frame{}, rect{};
					GetWindowRect(state->owner, &frame);
					GetWindowRect(window, &rect);
					const int snap = MulDiv(100, static_cast<int>(GetDpiForWindow(window)), 96);
					if (rect.left < frame.right && rect.right > frame.left &&
						rect.top >= frame.top && rect.top <= frame.top + snap) Dock(window, true);
				}
				return 0;
			case WM_SHOWWINDOW:
				if (state != nullptr && state->dockClient != nullptr)
				{
					// WM_SHOWWINDOW precedes the visibility style change.
					PostMessageW(window, WM_APP, 0, 0);
				}
				break;
			case WM_APP:
				if (state != nullptr && !LayoutRibbon(window, *state))
					Trace(L"could not reserve or release toolbar docking space");
				return 0;
			case WM_NOTIFY:
				if (state != nullptr && reinterpret_cast<NMHDR*>(lParam)->hwndFrom == state->toolbar &&
					reinterpret_cast<NMHDR*>(lParam)->code == TBN_HOTITEMCHANGE && GetFocus() == state->toolbar &&
					(reinterpret_cast<NMTBHOTITEM*>(lParam)->dwFlags & (HICF_MOUSE | HICF_LEAVING)) != 0)
					return TRUE; // Hover must not move the keyboard target or erase its focus outline.
				if (state != nullptr && reinterpret_cast<NMHDR*>(lParam)->hwndFrom == state->toolbar &&
					reinterpret_cast<NMHDR*>(lParam)->code == NM_CUSTOMDRAW)
					return DrawToolbar(*state, *reinterpret_cast<NMCUSTOMDRAW*>(lParam));
				if (state != nullptr && reinterpret_cast<NMHDR*>(lParam)->hwndFrom == state->tabs &&
					reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE)
				{
					SelectTab(window, TabAt(state->tabs, TabCtrl_GetCurSel(state->tabs)));
					return 0;
				}
				break;
			case WM_HSCROLL:
				if (state != nullptr)
				{
					SCROLLINFO info{sizeof(info), SIF_ALL};
					GetScrollInfo(window, SB_HORZ, &info);
					switch (LOWORD(wParam))
					{
					case SB_LINELEFT: state->scroll -= 32; break;
					case SB_LINERIGHT: state->scroll += 32; break;
					case SB_PAGELEFT: state->scroll -= static_cast<int>(info.nPage); break;
					case SB_PAGERIGHT: state->scroll += static_cast<int>(info.nPage); break;
					case SB_THUMBTRACK: state->scroll = info.nTrackPos; break;
					case SB_LEFT: state->scroll = 0; break;
					case SB_RIGHT: state->scroll = info.nMax; break;
					}
					if (!LayoutRibbon(window, *state)) Trace(L"could not scroll ribbon commands");
				}
				return 0;
			case WM_COMMAND:
				if (state != nullptr && LOWORD(wParam) == kClassic && reinterpret_cast<HWND>(lParam) == state->classic)
				{
					if (IsWindowEnabled(state->owner) && state->host.restoreClassic != nullptr) state->host.restoreClassic(state->host.context);
					return 0;
				}
				if (state != nullptr && LOWORD(wParam) >= kFirstAdmin && LOWORD(wParam) < kFirstAdmin + kMaxWorkflowActions &&
					state->tab == Tab::Admin && WorkflowButton(*state, reinterpret_cast<HWND>(lParam)) == static_cast<int>(LOWORD(wParam) - kFirstAdmin))
				{
					InvokeAdmin(window, LOWORD(wParam));
					return 0;
				}
				if (state != nullptr && LOWORD(wParam) >= kFirstSelector && LOWORD(wParam) < kFirstSelector + kSelectorCount &&
					SelectorIndex(*state, reinterpret_cast<HWND>(lParam)) == static_cast<int>(LOWORD(wParam) - kFirstSelector))
				{
					if (state->busy) return 0;
					const int index = static_cast<int>(LOWORD(wParam) - kFirstSelector);
					if (HIWORD(wParam) == CBN_SELENDOK)
						SelectChoice(window, static_cast<Selector>(index), static_cast<std::size_t>(SendMessageW(state->selectors[index], CB_GETCURSEL, 0, 0)));
					else if (HIWORD(wParam) == CBN_DROPDOWN || HIWORD(wParam) == CBN_SETFOCUS || HIWORD(wParam) == CBN_CLOSEUP)
					{
						state->forceSelectors = HIWORD(wParam) != CBN_CLOSEUP;
						Refresh(window);
					}
					return 0;
				}
				if (LOWORD(wParam) >= kFirstWorkflow && LOWORD(wParam) < kFirstWorkflow + kMaxWorkflowActions &&
					state != nullptr && WorkflowButton(*state, reinterpret_cast<HWND>(lParam)) == static_cast<int>(LOWORD(wParam) - kFirstWorkflow))
				{
					InvokeWorkflow(window, LOWORD(wParam));
					return 0;
				}
				if (LOWORD(wParam) >= kFirstNavigation && LOWORD(wParam) < kFirstNavigation + kNavigationCount)
				{
					if (!Navigate(window, LOWORD(wParam))) MessageBoxW(window, L"Navigation is unavailable for the current ProjectWise location. See the debug output for details.",
						L"ProjectWise Quick Access", MB_OK | MB_ICONINFORMATION);
					return 0;
				}
				if (LOWORD(wParam) == kMaximise || LOWORD(wParam) == kMinimise || LOWORD(wParam) == kHide)
				{
					SetDisplayMode(window, LOWORD(wParam) == kMaximise ? DisplayMode::Expanded :
						LOWORD(wParam) == kMinimise ? DisplayMode::Minimized : DisplayMode::Hidden);
					return 0;
				}
				if (LOWORD(wParam) == kSearch)
				{
					if (!Search(window)) MessageBoxW(window, L"Enter a search term and ensure an active ProjectWise document list is available. If the search fails, see the debug output.",
						L"ProjectWise Quick Access", MB_OK | MB_ICONINFORMATION);
					return 0;
				}
				if (state != nullptr && reinterpret_cast<HWND>(lParam) == state->toolbar)
				{
					if (!Invoke(window, LOWORD(wParam)))
						MessageBoxW(window, L"This command is unavailable for the current selection, or Explorer could not accept it.",
									L"ProjectWise Quick Access", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
					return 0;
				}
				break;
			case WM_TIMER:
				if (wParam == 1 && !IsHealthy(window)) { RequestRecovery(window, L"ribbon controls are no longer available"); return 0; }
				if (wParam == 1 && IsWindowVisible(window)) Refresh(window);
				state = State(window);
				if (state != nullptr && state->popup)
				{
					const HWND foreground = GetForegroundWindow();
					if (foreground != state->owner && foreground != window && !IsChild(state->owner, foreground)) DismissPopup(window);
				}
				return 0;
			case WM_SYSCOLORCHANGE:
			case WM_THEMECHANGED:
				RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
				return 0;
			case WM_DPICHANGED:
				if (state != nullptr)
				{
					const auto* rect = reinterpret_cast<const RECT*>(lParam);
					SetWindowPos(window, nullptr, rect->left, rect->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
					if (!SizeToolbar(window, *state)) RequestRecovery(window, L"ribbon DPI refresh failed");
				}
				return 0;
			case WM_DPICHANGED_AFTERPARENT:
				if (state != nullptr && !SizeToolbar(window, *state)) RequestRecovery(window, L"docked ribbon DPI refresh failed");
				return 0;
			case kRecover:
				if (state != nullptr)
				{
					const auto failed = state->host.failed;
					void* context = state->host.context;
					try { if (failed != nullptr) failed(context, reinterpret_cast<const wchar_t*>(lParam)); } catch (...) {}
					if (IsWindow(window)) DestroyWindow(window);
				}
				return 0;
			case WM_CLOSE:
				SetDisplayMode(window, DisplayMode::Hidden);
				return 0;
			case WM_DESTROY:
				if (state != nullptr) ClearAccessibility(*state);
				break;
			case WM_NCDESTROY:
				if (state != nullptr) UnregisterKeyboard(*state);
				if (state != nullptr && state->popupHook != nullptr) UnhookWindowsHookEx(state->popupHook);
				if (g_popup == window) g_popup = nullptr;
				if (state != nullptr && state->dockClient != nullptr)
				{
					RemoveWindowSubclass(state->dockClient, DockClientProc, reinterpret_cast<UINT_PTR>(window));
					state->arranging = true;
					SetWindowPos(state->dockClient, nullptr, state->dockRect.left, state->dockRect.top,
						state->dockRect.right - state->dockRect.left, state->dockRect.bottom - state->dockRect.top,
						SWP_NOZORDER | SWP_NOACTIVATE);
				}
				SetWindowLongPtrW(window, GWLP_USERDATA, 0);
				if (state != nullptr)
				{
					ClearAccessibility(*state);
					if (state->images != nullptr) ImageList_Destroy(state->images);
					if (state->font != nullptr) DeleteObject(state->font);
					if (state->comInitialised) CoUninitialize();
					if (!state->creating) delete state;
				}
				break;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			try { return WindowProcImpl(window, message, wParam, lParam); }
			catch (...) { RequestRecovery(window, L"ribbon window handler failed"); return message == WM_CREATE ? -1 : 0; }
		}

	}

	bool CanSelectChoice(const SelectorState& shown, const SelectorState& current, std::size_t choice) noexcept
	{
		if (!shown.enabled || !current.enabled || shown.context.empty() || shown.context != current.context ||
			!ValidSelector(shown) || !ValidSelector(current) || choice >= shown.choices.size()) return false;
		const auto& item = shown.choices[choice];
		return std::find(current.choices.begin(), current.choices.end(), item) != current.choices.end();
	}

	bool IsHealthy(HWND window) noexcept
	{
		const auto* state = State(window);
		if (state == nullptr || state->failurePending || !IsWindow(state->tabs) || !IsWindow(state->page) || !IsWindow(state->toolbar)) return false;
		for (const HWND button : state->modeButtons) if (!IsWindow(button)) return false;
		for (const Tab tab : {Tab::Workflow, Tab::Admin}) for (const HWND button : ContextButtons(*state, tab)) if (!IsWindow(button)) return false;
		for (const HWND combo : state->selectors) if (combo != nullptr && !IsWindow(combo)) return false;
		return state->classic == nullptr || IsWindow(state->classic);
	}

	double ContrastRatio(COLORREF first, COLORREF second) noexcept
	{
		const auto luminance = [](COLORREF colour) {
			const auto channel = [](BYTE value) {
				const double normalised = value / 255.0;
				return normalised <= 0.04045 ? normalised / 12.92 : std::pow((normalised + 0.055) / 1.055, 2.4);
			};
			return 0.2126 * channel(GetRValue(colour)) + 0.7152 * channel(GetGValue(colour)) + 0.0722 * channel(GetBValue(colour));
		};
		const double a = luminance(first), b = luminance(second);
		return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
	}

	COLORREF ContrastColour(COLORREF preferred, COLORREF background, double minimum) noexcept
	{
		if (ContrastRatio(preferred, background) >= minimum) return preferred;
		return ContrastRatio(RGB(0, 0, 0), background) >= ContrastRatio(RGB(255, 255, 255), background) ? RGB(0, 0, 0) : RGB(255, 255, 255);
	}

	bool HandleKey(HWND window, UINT key, bool shift, bool control, bool alt) noexcept
	{
		auto* state = State(window);
		if (state == nullptr || state->failurePending || !IsWindowEnabled(state->owner)) return false;
		const HWND focus = GetFocus();
		const bool inside = focus == window || IsChild(window, focus);
		if (!inside && focus != state->owner && !IsChild(state->owner, focus)) return false;
		if (!inside && IsChild(state->owner, focus)) state->lastContentFocus = focus;
		if (alt && !control && !shift && key == 'R')
		{
			if ((state->mode == DisplayMode::Hidden || !IsWindowVisible(window)) && !Restore(window)) return false;
			state = State(window);
			if (state == nullptr) return true;
			return FocusControl(*state, state->tabs);
		}
		if (control && !alt && !shift && key == VK_F1)
		{
			if (!SetDisplayMode(window, state->mode == DisplayMode::Expanded ? DisplayMode::Minimized : DisplayMode::Expanded)) return false;
			state = State(window);
			if (state == nullptr) return true;
			return FocusControl(*state, state->tabs);
		}
		if (key == VK_F6 && !control && !alt)
		{
			DismissPopup(window);
			const bool inNavigation = state->navigation != nullptr && (focus == state->navigation || IsChild(state->navigation, focus));
			if (!inside) return FocusControl(*state, shift || state->mode == DisplayMode::Hidden ? state->address : state->tabs);
			if (inNavigation) return shift && state->mode != DisplayMode::Hidden ? FocusControl(*state, state->tabs) : FocusContent(*state);
			return shift || state->address == nullptr ? FocusContent(*state) : FocusControl(*state, state->address);
		}
		if (!inside) return false;
		if ((key == VK_F10 && shift && !control && !alt) || key == VK_APPS)
		{
			SendMessageW(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(focus), MAKELPARAM(-1, -1));
			return true;
		}
		if (control || alt) return false; // Preserve Explorer accelerators, AltGr and native editing shortcuts.
		if (focus == state->tabs && (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END))
		{
			const int current = TabIndex(state->tabs, state->tab);
			const int count = TabCtrl_GetItemCount(state->tabs);
			const int next = key == VK_HOME ? 0 : key == VK_END ? count - 1 : (current + (key == VK_LEFT ? count - 1 : 1)) % count;
			return SelectTab(window, TabAt(state->tabs, next));
		}
		if (focus == state->tabs && (key == VK_RETURN || key == VK_SPACE || key == VK_DOWN))
		{
			if (!SelectTab(window, state->tab)) return false;
			state = State(window);
			if (state == nullptr) return true;
			if (!FocusControl(*state, Contextual(state->tab) ? WorkflowEntry(*state) : state->toolbar)) FocusControl(*state, state->searchText);
			return true;
		}
		const int workflowFocus = WorkflowButton(*state, focus);
		const int selectorFocus = SelectorIndex(*state, focus);
		if (workflowFocus >= 0 && (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END))
		{
			const int count = static_cast<int>(ContextButtons(*state, state->tab).size()), direction = key == VK_LEFT || key == VK_END ? -1 : 1;
			const int current = key == VK_HOME ? -1 : key == VK_END ? count : workflowFocus;
			for (int i = current + direction; i >= 0 && i < count; i += direction)
				if (FocusControl(*state, ContextButtons(*state, state->tab)[i])) break;
			return true;
		}
		if (focus == state->toolbar && (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END))
		{
			int current = state->focusedCommand;
			const int direction = key == VK_LEFT || key == VK_END ? -1 : 1;
			if (key == VK_HOME) current = -1;
			if (key == VK_END) current = static_cast<int>(kButtonCount);
			for (int i = current + direction; i >= 0 && i < static_cast<int>(kButtonCount); i += direction)
			{
				if (!ButtonOnPage(*state, static_cast<std::size_t>(i)) || SendMessageW(state->toolbar, TB_ISBUTTONENABLED, kFirstButton + i, 0) == 0) continue;
				RevealCommand(*state, static_cast<std::size_t>(i));
				break;
			}
			return true;
		}
		if (key == VK_ESCAPE)
		{
			if (selectorFocus >= 0 && SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0) != 0) return false;
			if (state->address != nullptr && (focus == state->address || IsChild(state->address, focus)) &&
				SendMessageW(state->address, CB_GETDROPPEDSTATE, 0, 0) != 0) return false;
			if (state->popup) { DismissPopup(window); return FocusControl(*state, state->tabs); }
			return FocusContent(*state);
		}
		if (focus == state->toolbar && (key == VK_RETURN || key == VK_SPACE))
		{
			const int index = state->focusedCommand;
			if (index >= 0 && index < static_cast<int>(kButtonCount))
			{
				if (!Invoke(window, kFirstButton + static_cast<UINT>(index)))
					MessageBoxW(window, L"This command is unavailable for the current ProjectWise selection.", L"ProjectWise Quick Access", MB_OK | MB_ICONINFORMATION);
				return true;
			}
		}
		if (key == VK_RETURN)
		{
			if (selectorFocus >= 0)
			{
				SelectChoice(window, static_cast<Selector>(selectorFocus), static_cast<std::size_t>(SendMessageW(focus, CB_GETCURSEL, 0, 0)));
				return true;
			}
			// These controls are hosted in a normal MFC frame, without a dialog manager to translate Enter.
			if (focus == state->searchText)
			{
				SendMessageW(window, WM_COMMAND, MAKEWPARAM(kSearch, BN_CLICKED), reinterpret_cast<LPARAM>(state->searchButton));
				return true;
			}
			if (focus == state->searchButton || focus == state->classic || workflowFocus >= 0 ||
				std::find(state->modeButtons.begin(), state->modeButtons.end(), focus) != state->modeButtons.end() ||
				std::find(state->navigationButtons.begin(), state->navigationButtons.end(), focus) != state->navigationButtons.end())
			{
				SendMessageW(focus, BM_CLICK, 0, 0);
				return true;
			}
		}
		if (key != VK_TAB || control || alt) return false;
		if (selectorFocus >= 0)
		{
			SendMessageW(focus, CB_SHOWDROPDOWN, FALSE, 0);
			if (State(window) != state) return true;
		}
		const HWND targets[] = {state->tabs, state->modeButtons[0], state->modeButtons[1], state->modeButtons[2],
			Contextual(state->tab) ? WorkflowEntry(*state) : state->toolbar,
			state->searchText, state->searchButton, state->selectors[0], state->selectors[1], state->classic,
			state->navigationButtons[0], state->navigationButtons[1], state->navigationButtons[2], state->address};
		int current = -1;
		for (int i = 0; i < static_cast<int>(ARRAYSIZE(targets)); ++i)
			if (targets[i] != nullptr && (focus == targets[i] || IsChild(targets[i], focus))) current = i;
		if (workflowFocus >= 0) current = 4;
		const int direction = shift ? -1 : 1;
		for (int i = current + direction; i >= 0 && i < static_cast<int>(ARRAYSIZE(targets)); i += direction)
			if (targets[i] != nullptr && FocusControl(*state, targets[i])) return true;
		return FocusContent(*state);
	}

	Tab ButtonTab(std::size_t button) noexcept
	{
		if (button == kShowAllVersions) return Tab::View;
		if (button < 9) return Tab::Home;
		if (button < 16) return Tab::View;
		if (button < 18) return Tab::Search;
		return Tab::Tools;
	}

	NavigationStates NavigationAvailability(bool historyEnabled, LONG count, LONG position, bool hasParent) noexcept
	{
		const bool validHistory = historyEnabled && count > 0 && position >= 0 && position < count;
		return {{{validHistory && position > 0, false}, {validHistory && position < count - 1, false}, {hasParent, false}}};
	}

	DisplayMode GetDisplayMode(HWND window) noexcept
	{
		const auto* state = State(window);
		return state != nullptr ? state->mode : DisplayMode::Hidden;
	}

	bool SetDisplayMode(HWND window, DisplayMode mode) noexcept
	{
		auto* state = State(window);
		if (state == nullptr || (mode != DisplayMode::Expanded && mode != DisplayMode::Minimized && mode != DisplayMode::Hidden))
		{
			Trace(L"invalid ribbon window or display mode");
			return false;
		}
		const HWND focusBefore = GetFocus();
		DismissPopup(window);
		if (mode == DisplayMode::Hidden && state->mode != DisplayMode::Hidden) state->restoreMode = state->mode;
		state->mode = mode;
		ShowWindow(window, mode == DisplayMode::Hidden && state->navigation == nullptr ? SW_HIDE : SW_SHOWNOACTIVATE);
		if (!LayoutRibbon(window, *state)) { Trace(L"could not change ribbon display mode"); return false; }
		if (focusBefore != nullptr && IsChild(window, focusBefore) && (!IsWindowVisible(focusBefore) || !IsWindowEnabled(focusBefore)))
		{
			if (mode == DisplayMode::Hidden && state->address != nullptr) FocusControl(*state, state->address);
			else if (mode == DisplayMode::Hidden) FocusContent(*state);
			else FocusControl(*state, state->tabs);
		}
		NotifyWinEvent(EVENT_OBJECT_STATECHANGE, window, OBJID_WINDOW, CHILDID_SELF);
		Refresh(window);
		return true;
	}

	bool Restore(HWND window) noexcept
	{
		const auto* state = State(window);
		return state != nullptr && SetDisplayMode(window, state->restoreMode);
	}

	Tab SelectedTab(HWND window) noexcept
	{
		const auto* state = State(window);
		return state != nullptr ? state->tab : Tab::Home;
	}

	bool IsPopupOpen(HWND window) noexcept
	{
		const auto* state = State(window);
		return state != nullptr && state->popup;
	}

	void DismissPopup(HWND window) noexcept
	{
		auto* state = State(window);
		if (state == nullptr || !state->popup) return;
		const HWND focus = GetFocus();
		if (focus == state->page || IsChild(state->page, focus)) FocusControl(*state, state->tabs);
		state->popup = false;
		if (state->popupHook != nullptr) UnhookWindowsHookEx(state->popupHook);
		state->popupHook = nullptr;
		if (g_popup == window) g_popup = nullptr;
		if (!LayoutRibbon(window, *state)) Trace(L"could not dismiss the minimized ribbon popup");
	}

	bool SelectTab(HWND window, Tab tab, bool reveal) noexcept
	{
		auto* state = State(window);
		if (state == nullptr || static_cast<unsigned>(tab) >= ARRAYSIZE(kTabs) || (Contextual(tab) && !Context(*state, tab).visible))
		{
			Trace(L"invalid ribbon window or tab");
			return false;
		}
		if (reveal && state->mode == DisplayMode::Minimized && !state->popup)
		{
			if (g_popup != nullptr && g_popup != window) DismissPopup(g_popup);
			state->popupHook = SetWindowsHookExW(WH_GETMESSAGE, PopupMessages, nullptr, GetCurrentThreadId());
			if (state->popupHook == nullptr) { Trace(L"could not monitor minimized ribbon dismissal"); return false; }
			g_popup = window;
			state->popup = true;
		}
		if (state->tab != tab) state->focusedCommand = -1;
		state->tab = tab;
		state->scroll = 0;
		TabCtrl_SetCurSel(state->tabs, TabIndex(state->tabs, tab));
		if (!LayoutRibbon(window, *state)) { Trace(L"could not switch ribbon tabs"); return false; }
		if (state->accessibility != nullptr)
		{
			wchar_t label[64]{};
			swprintf_s(label, L"%s commands", kTabs[static_cast<int>(tab)]);
			Annotate(*state, state->toolbar, label, L"Use arrow keys to select a command and Enter or Space to run it.");
		}
		NotifyWinEvent(EVENT_OBJECT_SELECTION, state->tabs, OBJID_CLIENT, static_cast<LONG>(TabIndex(state->tabs, tab)) + 1);
		Refresh(window);
		return true;
	}

	bool Search(HWND window) noexcept
	{
		auto* state = State(window);
		try
		{

			if (state == nullptr || state->busy || state->failurePending || state->tab != Tab::Search || state->mode == DisplayMode::Hidden ||
				(state->mode == DisplayMode::Minimized && !state->popup) || !IsWindowEnabled(state->owner) || state->host.search == nullptr)
			{
				Trace(L"ribbon search is unavailable");
				return false;
			}
			wchar_t text[1025]{};
			GetWindowTextW(state->searchText, text, ARRAYSIZE(text));
			wchar_t* query = text;
			while (iswspace(*query)) ++query;
			if (*query == L'\0') { Trace(L"enter a search term before starting a search"); return false; }
			state->busy = true;
			States states{};
			const bool available = state->host.refresh(state->host.context, states);
			if (State(window) != state) return false;
			const bool result = available && state->host.search(state->host.context, query);
			if (State(window) != state) return result;
			state->busy = false;
			DismissPopup(window);
			if (!result) Trace(L"ProjectWise search failed");
			return result;
		}
		catch (...)
		{
			if (State(window) == state && state != nullptr) { state->busy = false; RequestRecovery(window, L"ribbon host search failed"); }
			return false;
		}
	}

	bool ReadMenuState(HMENU menu, UINT command, ButtonState& state) noexcept
	{
		state = {};
		for (int i = 0, count = GetMenuItemCount(menu); i < count; ++i)
		{
			MENUITEMINFOW info{sizeof(info)};
			info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
			if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) return false;
			if (info.hSubMenu != nullptr)
			{
				if (ReadMenuState(info.hSubMenu, command, state))
				{
					state.enabled = state.enabled && (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
					return true;
				}
			}
			else if ((info.fType & MFT_SEPARATOR) == 0 && info.wID == command)
			{
				state.enabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
				state.checked = (info.fState & MFS_CHECKED) != 0;
				return true;
			}
		}
		return false;
	}

	HWND SelectionControl(HWND owner, HWND tree, HWND list, HWND previous) noexcept
	{
		const auto valid = [owner](HWND control) {
			return IsWindow(control) && IsChild(owner, control) &&
				GetWindowThreadProcessId(control, nullptr) == GetCurrentThreadId();
		};
		const HWND focus = GetFocus();
		for (HWND control : {tree, list})
			if (valid(control) && (focus == control || IsChild(control, focus))) return control;
		if ((previous == tree || previous == list) && valid(previous)) return previous;
		if (valid(tree)) return tree;
		return valid(list) ? list : nullptr;
	}

	bool IsDocked(HWND window) noexcept
	{
		const auto* state = State(window);
		return state != nullptr && state->dockClient != nullptr;
	}

	bool Dock(HWND window, bool dock) noexcept
	{
		auto* state = State(window);
		if (state == nullptr) return false;
		if (dock == IsDocked(window)) return true;
		RECT rect{};
		GetWindowRect(window, &rect);
		const bool visible = (GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) != 0;
		if (dock)
		{
			const HWND client = DockClient(state->owner);
			if (!IsWindow(client) || client == window ||
				!SetWindowSubclass(client, DockClientProc, reinterpret_cast<UINT_PTR>(window), reinterpret_cast<DWORD_PTR>(window)))
			{
				Trace(L"no suitable Explorer content pane is available for top docking");
				return false;
			}
			state->dockClient = client;
			GetWindowRect(client, &state->dockRect);
			MapWindowPoints(nullptr, state->owner, reinterpret_cast<POINT*>(&state->dockRect), 2);
			SetWindowLongW(window, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS | (visible ? WS_VISIBLE : 0));
			SetParent(window, state->owner);
		}
		else
		{
			const HWND client = state->dockClient;
			RemoveWindowSubclass(client, DockClientProc, reinterpret_cast<UINT_PTR>(window));
			state->dockClient = nullptr;
			if (!SetWindowPos(client, nullptr, state->dockRect.left, state->dockRect.top,
				state->dockRect.right - state->dockRect.left, state->dockRect.bottom - state->dockRect.top,
				SWP_NOZORDER | SWP_NOACTIVATE)) Trace(L"could not restore the Explorer content pane");
			SetParent(window, nullptr);
			SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(state->owner));
			SetWindowLongW(window, GWL_STYLE, WS_POPUP | WS_CAPTION | WS_SYSMENU | (visible ? WS_VISIBLE : 0));
		}
		if (!SetWindowPos(window, nullptr, rect.left, rect.top, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) || !SizeToolbar(window, *state))
		{
			Trace(L"could not resize the toolbar after changing docking mode");
			return false;
		}
		return true;
	}

	HWND Create(HWND owner, HINSTANCE instance, Host host) noexcept
	{
		if (!IsWindow(owner) || host.refresh == nullptr || host.execute == nullptr)
		{
			Trace(L"invalid toolbar owner or host callbacks");
			return nullptr;
		}
		WNDCLASSEXW cls{sizeof(cls)};
		cls.lpfnWndProc = WindowProc;
		cls.hInstance = instance;
		cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		cls.lpszClassName = kClass;
		if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			Trace(L"could not register toolbar window class");
			return nullptr;
		}
		auto* state = new (std::nothrow) WindowState{host, owner};
		if (state == nullptr)
		{
			Trace(L"could not allocate toolbar state");
			return nullptr;
		}
		RECT parent{};
		GetWindowRect(owner, &parent);
		HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClass, L"AtkinsRealis - Quick Access",
			WS_POPUP | WS_CAPTION | WS_SYSMENU, parent.left + 24, parent.top + 100, 800, 100, owner, nullptr, instance, state);
		if (window == nullptr)
		{
			delete state;
			Trace(L"could not create toolbar window");
		}
		else
		{
			state->creating = false;
		}
		return window;
	}

	void Refresh(HWND window) noexcept
	{
		auto* state = State(window);
		try
		{

			if (state == nullptr || state->busy || state->failurePending) return;
			const HWND focusBefore = GetFocus();
			state->busy = true;
			States states{};
			const bool available = IsWindowEnabled(state->owner) && state->host.refresh(state->host.context, states);
			if (State(window) != state) return;
			for (std::size_t i = 0; i < states.size(); ++i)
			{
				SendMessageW(state->toolbar, TB_ENABLEBUTTON, kFirstButton + i, MAKELPARAM(available && states[i].enabled, 0));
				SendMessageW(state->toolbar, TB_CHECKBUTTON, kFirstButton + i, MAKELPARAM(available && states[i].checked, 0));
			}
			EnableWindow(state->searchText, available && state->host.search != nullptr);
			EnableWindow(state->searchButton, available && state->host.search != nullptr);
			if (state->navigation != nullptr)
			{
				NavigationStates navigation{};
				const bool ready = IsWindowEnabled(state->owner) && state->host.refreshNavigation != nullptr &&
					state->host.refreshNavigation(state->host.context, state->address, navigation);
				if (State(window) != state) return;
				for (std::size_t i = 0; i < navigation.size(); ++i)
					EnableWindow(state->navigationButtons[i], ready && navigation[i].enabled && state->host.navigate != nullptr);
				const bool addressReady = state->host.refreshAddress != nullptr ? IsWindowEnabled(state->owner) &&
					state->host.refreshAddress(state->host.context, state->address) : ready;
				if (State(window) != state) return;
				EnableWindow(state->address, addressReady);
			}
			for (const Tab tab : {Tab::Workflow, Tab::Admin})
			{
				const auto query = tab == Tab::Admin ? state->host.refreshAdmin : state->host.refreshWorkflow;
				if (query == nullptr) continue;
				try
				{
					ContextState context;
					const bool ready = IsWindowEnabled(state->owner) && query(state->host.context, context);
					if (State(window) != state) return;
					if (!ready) context = {};
					if (!UpdateContext(*state, tab, std::move(context))) ResetContext(*state, tab);
				}
				catch (...)
				{
					if (State(window) != state) return;
					ResetContext(*state, tab);
				}
			}
			if (HasSelectors(*state))
			{
				try
				{
					SelectorStates selectors;
					const bool force = state->forceSelectors;
					state->forceSelectors = false;
					const bool ready = IsWindowEnabled(state->owner) && state->host.refreshSelectors(state->host.context, selectors, force);
					if (State(window) != state) return;
					if (!ready) selectors = {};
					UpdateSelectors(*state, std::move(selectors));
				}
				catch (...)
				{
					if (State(window) != state) return;
					state->selectorStates = {};
					for (const HWND combo : state->selectors) { SendMessageW(combo, CB_RESETCONTENT, 0, 0); EnableWindow(combo, FALSE); }
					Trace(L"could not refresh Interface and View selectors");
				}
			}
			state->busy = false;
			if (GetFocus() == state->toolbar && (state->focusedCommand < 0 ||
				SendMessageW(state->toolbar, TB_ISBUTTONENABLED, kFirstButton + state->focusedCommand, 0) == 0))
			{
				if (!FocusControl(*state, state->toolbar)) FocusControl(*state, state->tabs);
			}
			else if (focusBefore != nullptr && IsChild(window, focusBefore) &&
				(!IsWindowEnabled(focusBefore) || !IsWindowVisible(focusBefore)))
			{
				if (!FocusControl(*state, state->tabs)) FocusContent(*state);
			}
		}
		catch (...)
		{
			if (State(window) == state && state != nullptr) { state->busy = false; RequestRecovery(window, L"ribbon host refresh failed"); }
		}
	}

	bool Navigate(HWND window, UINT buttonId) noexcept
	{
		auto* state = State(window);
		try
		{

			if (state == nullptr || state->busy || state->failurePending || state->address == nullptr || state->host.refreshNavigation == nullptr ||
				state->host.navigate == nullptr || buttonId < kFirstNavigation || buttonId >= kFirstNavigation + kNavigationCount ||
				!IsWindowEnabled(state->owner))
			{
				Trace(L"navigation is unavailable");
				return false;
			}
			DismissPopup(window);
			state->busy = true;
			NavigationStates states{};
			const bool ready = state->host.refreshNavigation(state->host.context, state->address, states);
			if (State(window) != state) return false;
			const auto index = buttonId - kFirstNavigation;
			const bool result = ready && states[index].enabled && state->host.navigate(state->host.context, index);
			if (State(window) != state) return result;
			state->busy = false;
			if (!result) Trace(L"navigation command unavailable or failed");
			Refresh(window);
			return result;
		}
		catch (...)
		{
			if (State(window) == state && state != nullptr) { state->busy = false; RequestRecovery(window, L"ribbon host navigate failed"); }
			return false;
		}
	}

	bool Invoke(HWND window, UINT buttonId) noexcept
	{
		auto* state = State(window);
		try
		{

			if (state == nullptr || state->busy || state->failurePending || buttonId < kFirstButton || buttonId >= kFirstButton + kButtonCount) return false;
			if (state->mode == DisplayMode::Hidden || (state->mode == DisplayMode::Minimized && !state->popup) ||
				!ButtonOnPage(*state, buttonId - kFirstButton)) return false;
			state->busy = true;
			States states{};
			const auto button = buttonId - kFirstButton;
			const bool available = IsWindowEnabled(state->owner) && state->host.refresh(state->host.context, states);
			if (State(window) != state) return false;
			const bool executed = available && states[button].enabled && state->host.execute(state->host.context, button);
			if (State(window) != state) return executed;
			if (!executed) Trace(L"command unavailable or dispatch failed");
			state->busy = false;
			DismissPopup(window);
			Refresh(window);
			return executed;
		}
		catch (...)
		{
			if (State(window) == state && state != nullptr) { state->busy = false; RequestRecovery(window, L"ribbon host invoke failed"); }
			return false;
		}
	}

	bool InvokeContext(HWND window, Tab tab, UINT buttonId) noexcept
	{
		auto* state = State(window);
		const UINT firstButton = tab == Tab::Admin ? kFirstAdmin : kFirstWorkflow;
		if (state == nullptr) return false;
		const auto query = tab == Tab::Admin ? state->host.refreshAdmin : state->host.refreshWorkflow;
		const auto execute = tab == Tab::Admin ? state->host.executeAdmin : state->host.executeWorkflow;
		if (state == nullptr || state->busy || state->failurePending || !IsWindowEnabled(state->owner) || state->tab != tab ||
			state->mode == DisplayMode::Hidden || (state->mode == DisplayMode::Minimized && !state->popup) ||
			buttonId < firstButton || buttonId - firstButton >= Context(*state, tab).actions.size() ||
			query == nullptr || execute == nullptr) return false;
		try
		{
			const WorkflowState shown = Context(*state, tab);
			const UINT command = shown.actions[buttonId - firstButton].command;
			state->busy = true;
			WorkflowState current;
			const bool ready = query(state->host.context, current);
			if (State(window) != state) return false;
			const bool result = ready && CanInvokeWorkflow(shown, current, command) && execute(state->host.context, shown, command);
			if (State(window) != state) return result;
			state->busy = false;
			DismissPopup(window);
			Refresh(window);
			return result;
		}
		catch (...)
		{
			if (State(window) == state) { state->busy = false; ResetContext(*state, tab); }
			Trace(L"workflow operation could not be dispatched");
			return false;
		}
	}

	bool InvokeWorkflow(HWND window, UINT buttonId) noexcept { return InvokeContext(window, Tab::Workflow, buttonId); }
	bool InvokeAdmin(HWND window, UINT buttonId) noexcept { return InvokeContext(window, Tab::Admin, buttonId); }

	bool SelectChoice(HWND window, Selector selector, std::size_t choice) noexcept
	{
		auto* state = State(window);
		const auto index = static_cast<std::size_t>(selector);
		if (state == nullptr || state->busy || state->failurePending || index >= kSelectorCount || !HasSelectors(*state) || !IsWindowEnabled(state->owner) ||
			state->tab != Tab::View || state->mode == DisplayMode::Hidden || (state->mode == DisplayMode::Minimized && !state->popup)) return false;
		try
		{
			const SelectorState shown = state->selectorStates[index];
			state->busy = true;
			SendMessageW(state->selectors[index], CB_SHOWDROPDOWN, FALSE, 0);
			SelectorStates current;
			const bool ready = state->host.refreshSelectors(state->host.context, current, true);
			if (State(window) != state) return false;
			const bool result = ready && CanSelectChoice(shown, current[index], choice) &&
				state->host.selectChoice(state->host.context, selector, shown, choice);
			if (State(window) != state) return result;
			state->busy = false;
			if (result) DismissPopup(window);
			Refresh(window);
			return result;
		}
		catch (...)
		{
			if (State(window) == state) { state->busy = false; Refresh(window); }
			Trace(L"Interface or View selection could not be applied");
			return false;
		}
	}
}
