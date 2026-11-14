#include "VersionStacks.h"

#include <oleacc.h>
#include <algorithm>
#include <map>
#include <memory>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace QuickAccess::VersionStacks
{
	std::vector<Group> GroupRows(const std::vector<Row>& rows)
	{
		std::vector<Group> result;
		std::map<Family, std::size_t> families;
		for (std::size_t i = 0; i < rows.size(); ++i)
		{
			const auto& row = rows[i];
			const Family family{row.project, row.original > 0 ? row.original : row.document};
			if (!row.groupable || row.project <= 0 || row.document <= 0)
			{
				result.push_back({{}, {i}}); // folders and other independent rows retain their position
				continue;
			}
			const auto [found, inserted] = families.emplace(family, result.size());
			if (inserted) result.push_back({family, {}});
			result[found->second].rows.push_back(i);
		}
		for (auto& group : result)
			std::stable_sort(group.rows.begin(), group.rows.end(), [&rows](std::size_t a, std::size_t b) {
				if (rows[a].sequence != rows[b].sequence) return rows[a].sequence > rows[b].sequence;
				return !rows[a].historical && rows[b].historical;
			});
		return result;
	}

	std::vector<Entry> VisibleRows(const std::vector<Group>& groups, const std::set<Family>& expanded)
	{
		std::vector<Entry> result;
		for (std::size_t i = 0; i < groups.size(); ++i)
			for (std::size_t n = 0; n < groups[i].rows.size(); ++n)
			{
				if (n > 0 && !expanded.contains(groups[i].family)) break;
				result.push_back({groups[i].rows[n], i, n == 0});
			}
		return result;
	}

	namespace
	{
		constexpr wchar_t kClass[] = L"AtkinsRealis.ProjectWise.VersionStacks";
		constexpr UINT kSync = WM_APP + 210;
		constexpr UINT kRetry = WM_APP + 211;
		struct State
		{
			HWND window = nullptr, list = nullptr, status = nullptr;
			Host host;
			Snapshot snapshot;
			std::vector<Group> groups;
			std::vector<Entry> visible;
			std::set<Family> expanded;
			HIMAGELIST images = nullptr, stateImages = nullptr;
			IAccPropServices* accessibility = nullptr;
			HFONT font = nullptr;
			bool com = false, building = false, reading = false, ready = false;
			~State()
			{
				if (images) ImageList_Destroy(images);
				if (stateImages) ImageList_Destroy(stateImages);
				if (font) DeleteObject(font);
				if (accessibility) accessibility->Release();
				if (com) CoUninitialize();
				if (host.destroy) host.destroy(host.context);
			}
		};
		using StatePtr = std::shared_ptr<State>;
		StatePtr Get(HWND window)
		{
			const auto data = reinterpret_cast<StatePtr*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			return data ? *data : StatePtr{};
		}
		int Scale(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }
		void Annotate(State& state, HWND control, DWORD child, MSAAPROPID property, const std::wstring& value)
		{
			if (state.accessibility)
				state.accessibility->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), child, property, value.c_str());
		}
		void ClearAnnotations(State& state)
		{
			if (!state.accessibility) return;
			const MSAAPROPID properties[]{PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION};
			for (std::size_t i = 0; i < state.visible.size(); ++i)
				state.accessibility->ClearHwndProps(state.list, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(i + 1), properties, ARRAYSIZE(properties));
		}
		std::size_t FocusedRow(const State& state)
		{
			const int focus = ListView_GetNextItem(state.list, -1, LVNI_FOCUSED);
			return focus >= 0 && static_cast<std::size_t>(focus) < state.visible.size() ? state.visible[focus].row : state.snapshot.rows.size();
		}
		void ReadSelection(State& state)
		{
			for (auto& row : state.snapshot.rows) row.selected = row.focused = false;
			for (std::size_t i = 0; i < state.visible.size(); ++i)
			{
				const UINT flags = ListView_GetItemState(state.list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED);
				auto& row = state.snapshot.rows[state.visible[i].row];
				row.selected = (flags & LVIS_SELECTED) != 0;
				row.focused = (flags & LVIS_FOCUSED) != 0;
			}
		}
		bool Sync(State& state)
		{
			if (!state.ready || state.building || !IsWindowEnabled(GetAncestor(state.window, GA_ROOT))) return false;
			ReadSelection(state);
			std::vector<std::size_t> selected;
			for (std::size_t i = 0; i < state.snapshot.rows.size(); ++i)
				if (state.snapshot.rows[i].selected) selected.push_back(i);
			const bool accepted = state.host.select && state.host.select(state.host.context, state.snapshot, selected, FocusedRow(state));
			if (!accepted) { state.ready = false; EnableWindow(state.list, FALSE); PostMessageW(state.window, kRetry, 0, 0); }
			return accepted;
		}
		void Render(State& state)
		{
			state.building = true;
			SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
			ClearAnnotations(state);
			ListView_DeleteAllItems(state.list);
			state.visible = VisibleRows(state.groups, state.expanded);
			for (std::size_t i = 0; i < state.visible.size(); ++i)
			{
				const auto& entry = state.visible[i];
				const auto& group = state.groups[entry.group];
				const auto& row = state.snapshot.rows[entry.row];
				const bool expandable = entry.parent && group.rows.size() > 1;
				const bool expanded = state.expanded.contains(group.family);
				std::wstring marker = expandable ? (expanded ? L"\x25be " : L"\x25b8 ") + std::to_wstring(group.rows.size()) : entry.parent ? L"" : L"\x21b3";
				LVITEMW item{};
				item.mask = LVIF_TEXT | LVIF_STATE;
				item.iItem = static_cast<int>(i);
				item.pszText = marker.data();
				item.stateMask = LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK | LVIS_OVERLAYMASK;
				item.state = row.stateImages | (row.selected ? LVIS_SELECTED : 0) | (row.focused ? LVIS_FOCUSED : 0);
				ListView_InsertItem(state.list, &item);
				for (std::size_t c = 0; c < row.cells.size(); ++c)
				{
					LVITEMW cell{};
					cell.mask = LVIF_TEXT | (c == 0 ? LVIF_IMAGE : 0);
					cell.iItem = static_cast<int>(i); cell.iSubItem = static_cast<int>(c + 1);
					cell.pszText = const_cast<LPWSTR>(row.cells[c].c_str()); cell.iImage = row.image;
					SendMessageW(state.list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&cell));
				}
				std::wstring sequence = row.groupable ? std::to_wstring(row.sequence) : L"";
				ListView_SetItemText(state.list, static_cast<int>(i), static_cast<int>(state.snapshot.columns.size() + 1), sequence.data());
				const std::wstring name = (row.cells.empty() ? L"Document" : row.cells[0]) +
					(row.groupable ? L", sequence " + std::to_wstring(row.sequence) : L"");
				const std::wstring description = expandable ? std::to_wstring(group.rows.size()) + L" visible versions, " +
					(expanded ? L"expanded. Left arrow collapses." : L"collapsed. Right arrow expands.") :
					entry.parent ? L"" : L"Older visible version. Left arrow returns to the parent.";
				Annotate(state, state.list, static_cast<DWORD>(i + 1), PROPID_ACC_NAME, name);
				Annotate(state, state.list, static_cast<DWORD>(i + 1), PROPID_ACC_DESCRIPTION, description);
			}
			SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
			InvalidateRect(state.list, nullptr, TRUE);
			const int focused = ListView_GetNextItem(state.list, -1, LVNI_FOCUSED);
			if (focused >= 0) ListView_EnsureVisible(state.list, focused, FALSE);
			state.building = false;
			NotifyWinEvent(EVENT_OBJECT_REORDER, state.list, OBJID_CLIENT, CHILDID_SELF);
		}
		void CollapseGroup(State& state, const Group& group)
		{
			if (group.rows.size() < 2) return;
			for (std::size_t n = 1; n < group.rows.size(); ++n)
			{
				auto& row = state.snapshot.rows[group.rows[n]];
				auto& parent = state.snapshot.rows[group.rows[0]];
				parent.selected = parent.selected || row.selected;
				parent.focused = parent.focused || row.focused;
				row.selected = row.focused = false;
			}
			state.expanded.erase(group.family);
		}
		void Expand(State& state, int visibleIndex, bool expand)
		{
			if (!state.ready || visibleIndex < 0 || static_cast<std::size_t>(visibleIndex) >= state.visible.size()) return;
			ReadSelection(state);
			const auto entry = state.visible[visibleIndex];
			const auto& group = state.groups[entry.group];
			if (group.rows.size() < 2) return;
			if (expand) state.expanded.insert(group.family);
			else CollapseGroup(state, group);
			Render(state);
			Sync(state); // collapsing must never leave a selected version hidden from the user
		}
		void All(State& state, bool expand)
		{
			if (!state.ready) return;
			ReadSelection(state);
			for (const auto& group : state.groups)
				if (expand && group.rows.size() > 1) state.expanded.insert(group.family);
				else if (!expand) CollapseGroup(state, group);
			Render(state); Sync(state);
		}
		void Layout(State& state)
		{
			RECT bounds{}; GetClientRect(state.window, &bounds);
			const int gap = Scale(state.window, 6), width = Scale(state.window, 112), height = Scale(state.window, 30);
			int x = gap, y = gap;
			for (int id : {kExpand, kCollapse, kClassic})
			{
				if (x > gap && x + width > bounds.right) { x = gap; y += height + gap; }
				MoveWindow(GetDlgItem(state.window, id), x, y, width, height, TRUE); x += width + gap;
			}
			y += height + gap;
			const int messageHeight = Scale(state.window, 40);
			MoveWindow(state.status, gap, y, std::max(0L, bounds.right - 2 * gap), messageHeight, TRUE);
			y += messageHeight;
			MoveWindow(state.list, 0, y, bounds.right, std::max(0L, bounds.bottom - y), TRUE);
		}
		void Load(State& state, bool force)
		{
			if (state.reading || !IsWindow(state.window) || !IsWindowEnabled(GetAncestor(state.window, GA_ROOT))) return;
			state.reading = true;
			Snapshot fresh;
			ReadResult result = ReadResult::Unavailable;
			try { if (state.host.read) result = state.host.read(state.host.context, fresh, force); }
			catch (...) { fresh.message = L"Version stacks could not be refreshed. Use Classic list."; }
			state.reading = false;
			if (!IsWindow(state.window) || result == ReadResult::Unchanged) return;
			state.ready = result == ReadResult::Ready;
			if (!state.ready) { fresh.rows.clear(); fresh.columns.clear(); }
			state.building = true;
			ClearAnnotations(state);
			state.visible.clear();
			ListView_DeleteAllItems(state.list);
			while (ListView_DeleteColumn(state.list, 0)) {}
			if (state.snapshot.context != fresh.context) state.expanded.clear();
			state.snapshot = std::move(fresh);
			state.groups = GroupRows(state.snapshot.rows);
			if (state.ready)
			{
				LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
				col.pszText = const_cast<LPWSTR>(L"Versions"); col.cx = Scale(state.window, 86);
				ListView_InsertColumn(state.list, 0, &col);
				for (std::size_t n = 0; n < state.snapshot.columns.size(); ++n)
				{
					const auto& c = state.snapshot.columns[n];
					col.pszText = const_cast<LPWSTR>(c.label.c_str()); col.cx = c.width; col.fmt = c.format & LVCFMT_JUSTIFYMASK;
					ListView_InsertColumn(state.list, static_cast<int>(n + 1), &col);
				}
				col.pszText = const_cast<LPWSTR>(L"Sequence"); col.cx = Scale(state.window, 88); col.fmt = LVCFMT_RIGHT;
				ListView_InsertColumn(state.list, static_cast<int>(state.snapshot.columns.size() + 1), &col);
				if (state.snapshot.columnOrder.size() == state.snapshot.columns.size())
				{
					std::vector<int> order{0};
					for (int index : state.snapshot.columnOrder) order.push_back(index + 1);
					order.push_back(static_cast<int>(state.snapshot.columns.size() + 1));
					ListView_SetColumnOrderArray(state.list, static_cast<int>(order.size()), order.data());
				}
				HIMAGELIST images = state.snapshot.images ? ImageList_Duplicate(state.snapshot.images) : nullptr;
				HIMAGELIST states = state.snapshot.stateImages ? ImageList_Duplicate(state.snapshot.stateImages) : nullptr;
				ListView_SetImageList(state.list, images, LVSIL_SMALL);
				ListView_SetImageList(state.list, states, LVSIL_STATE);
				if (state.images) ImageList_Destroy(state.images);
				if (state.stateImages) ImageList_Destroy(state.stateImages);
				state.images = images; state.stateImages = states;
				// External native selection is never concealed by a collapsed stack.
				for (const auto& group : state.groups)
					for (std::size_t n = 1; n < group.rows.size(); ++n)
						if (state.snapshot.rows[group.rows[n]].selected || state.snapshot.rows[group.rows[n]].focused)
							state.expanded.insert(group.family);
			}
			state.building = false;
			Render(state);
			SetWindowTextW(state.status, state.ready ? L"Parent: highest visible sequence. Right / Left arrow expands / collapses a stack." :
				state.snapshot.message.empty() ? L"Version stacks are unavailable here. Use Classic list." : state.snapshot.message.c_str());
			EnableWindow(state.list, state.ready);
			EnableWindow(GetDlgItem(state.window, kExpand), state.ready);
			EnableWindow(GetDlgItem(state.window, kCollapse), state.ready);
		}
		LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref)
		{
			const auto state = Get(reinterpret_cast<HWND>(ref));
			if (!state) return DefSubclassProc(control, message, wParam, lParam);
			try
			{
				if (message == WM_KEYDOWN && wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)
				{
					const bool reverse = GetKeyState(VK_SHIFT) < 0;
					const HWND nextNative = GetNextDlgTabItem(GetAncestor(state->window, GA_ROOT), control, reverse);
					if (nextNative && nextNative != control && IsWindowEnabled(nextNative) && IsWindowVisible(nextNative))
					{
						SetFocus(nextNative); return 0;
					}
					const HWND controls[]{GetDlgItem(state->window, kExpand), GetDlgItem(state->window, kCollapse), GetDlgItem(state->window, kClassic), state->list};
					for (int i = 0; i < 4; ++i) if (controls[i] == control)
						for (int n = 1; n <= 4; ++n)
						{
							const int index = i + (reverse ? -n : n);
							if (index < 0 || index >= 4) { SetFocus(GetParent(state->window)); return 0; }
							const HWND next = controls[index];
							if (IsWindowEnabled(next)) { SetFocus(next); return 0; }
						}
				}
				if (control == state->list && message == WM_KEYDOWN && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)
				{
					const int focused = ListView_GetNextItem(control, -1, LVNI_FOCUSED);
					if (wParam == VK_LEFT || wParam == VK_RIGHT) { Expand(*state, focused, wParam == VK_RIGHT); return 0; }
					if (wParam == VK_RETURN)
					{
						if ((lParam & (1L << 30)) == 0 && Sync(*state) && state->host.activate)
							state->host.activate(state->host.context, state->snapshot, FocusedRow(*state));
						return 0;
					}
				}
				if (control != state->list && message == WM_KEYDOWN && wParam == VK_RETURN)
				{
					if ((lParam & (1L << 30)) == 0) SendMessageW(control, BM_CLICK, 0, 0);
					return 0;
				}
				if (control == state->list && (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK))
				{
					LVHITTESTINFO hit{}; hit.pt = {static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
					if (ListView_SubItemHitTest(control, &hit) >= 0 && hit.iSubItem == 0 && static_cast<std::size_t>(hit.iItem) < state->visible.size())
					{
						const auto& entry = state->visible[hit.iItem];
						const auto& group = state->groups[entry.group];
						if (entry.parent && group.rows.size() > 1)
						{
							SetFocus(control);
							if (message == WM_LBUTTONDOWN) Expand(*state, hit.iItem, !state->expanded.contains(group.family));
							return 0;
						}
					}
				}
				if (control == state->list && message == WM_CONTEXTMENU)
				{
					POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
					if (point.x == -1 && point.y == -1) { point = {Scale(control, 100), Scale(control, 40)}; ClientToScreen(control, &point); }
					if (Sync(*state) && state->host.menu) state->host.menu(state->host.context, state->snapshot, point);
					return 0;
				}
			}
			catch (...) { state->ready = false; PostMessageW(state->window, kRetry, 0, 0); return 0; }
			return DefSubclassProc(control, message, wParam, lParam);
		}
		LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			if (message == WM_NCCREATE)
			{
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
				return TRUE;
			}
			const auto state = Get(window);
			if (!state) return DefWindowProcW(window, message, wParam, lParam);
			try
			{
				switch (message)
				{
				case WM_CREATE:
				{
					state->window = window;
					const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
					NONCLIENTMETRICSW metrics{sizeof(metrics)};
					SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
					state->font = CreateFontIndirectW(&metrics.lfMessageFont);
					for (const auto& button : {std::pair{kExpand, L"Expand all"}, {kCollapse, L"Collapse all"}, {kClassic, L"Classic list"}})
					{
						HWND child = CreateWindowExW(0, L"BUTTON", button.second, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
							0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.first)), instance, nullptr);
						if (!child || !SetWindowSubclass(child, ControlProc, 1, reinterpret_cast<DWORD_PTR>(window))) return -1;
						SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
					}
					state->status = CreateWindowExW(0, L"STATIC", L"Loading version stacks...", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
						0, 0, 0, 0, window, nullptr, instance, nullptr);
					state->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Version stacks", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
						LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | LVS_NOSORTHEADER, 0, 0, 0, 0, window,
						reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)), instance, nullptr);
					if (!state->status || !state->list || !SetWindowSubclass(state->list, ControlProc, 1, reinterpret_cast<DWORD_PTR>(window))) return -1;
					ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_SUBITEMIMAGES | LVS_EX_LABELTIP);
					SendMessageW(state->list, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
					SendMessageW(state->status, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
					const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
					state->com = SUCCEEDED(com);
					CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&state->accessibility));
					Annotate(*state, state->list, CHILDID_SELF, PROPID_ACC_NAME, L"Document version stacks");
					Annotate(*state, state->list, CHILDID_SELF, PROPID_ACC_DESCRIPTION, L"The parent is the highest sequence visible in this list. Right arrow expands; left arrow collapses. Only selected rows are used by document commands.");
					if (!SetTimer(window, 1, 500, nullptr)) return -1;
					Layout(*state); return 0;
				}
				case WM_SHOWWINDOW: if (wParam) Load(*state, true); break;
				case WM_TIMER: if (IsWindowVisible(window)) Load(*state, false); return 0;
				case WM_SIZE: Layout(*state); return 0;
				case WM_SETFOCUS: SetFocus(state->ready ? state->list : GetDlgItem(window, kClassic)); return 0;
				case WM_COMMAND:
					if (!IsWindowEnabled(GetAncestor(window, GA_ROOT))) return 0;
					if (HIWORD(wParam) == BN_CLICKED)
					{
						if (LOWORD(wParam) == kExpand || LOWORD(wParam) == kCollapse) All(*state, LOWORD(wParam) == kExpand);
						else if (LOWORD(wParam) == kClassic && state->host.classic) state->host.classic(state->host.context);
					}
					return 0;
				case WM_NOTIFY:
				{
					const auto header = reinterpret_cast<NMHDR*>(lParam);
					if (header->hwndFrom == state->list && header->code == LVN_ITEMCHANGED && !state->building)
						PostMessageW(window, kSync, 0, 0);
					if (header->hwndFrom == state->list && header->code == LVN_ITEMACTIVATE && Sync(*state) && state->host.activate)
						state->host.activate(state->host.context, state->snapshot, FocusedRow(*state));
					return 0;
				}
				case kSync: if (state->ready && !state->building) Sync(*state); return 0;
				case kRetry: Load(*state, true); return 0;
				case WM_SYSCOLORCHANGE: case WM_THEMECHANGED: Load(*state, true); InvalidateRect(window, nullptr, TRUE); return 0;
				case WM_NCDESTROY:
				{
					ClearAnnotations(*state);
					const auto data = reinterpret_cast<StatePtr*>(GetWindowLongPtrW(window, GWLP_USERDATA));
					SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete data;
					return DefWindowProcW(window, message, wParam, lParam);
				}
				}
			}
			catch (...) { if (message == WM_CREATE) return -1; state->ready = false; EnableWindow(state->list, FALSE); SetWindowTextW(state->status, L"Version stacks failed. Use Classic list."); }
			return DefWindowProcW(window, message, wParam, lParam);
		}
	}

	HWND Create(HWND parent, HINSTANCE instance, const Host& host) noexcept
	{
		bool owned = false;
		try
		{
			auto state = std::make_shared<State>(); state->host = host; owned = true;
			if (!IsWindow(parent) || !host.read || !host.select) return nullptr;
			INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
			WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
			wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = kClass;
			if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
			auto data = std::make_unique<StatePtr>(state);
			HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Version stacks", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
				0, 0, 0, 0, parent, nullptr, instance, data.get());
			// WM_NCDESTROY releases the pointer after WM_NCCREATE, including a failed WM_CREATE.
			if (window) data.release();
			else if (state->window) data.release();
			return window;
		}
		catch (...) { if (!owned && host.destroy) host.destroy(host.context); return nullptr; }
	}
	bool Refresh(HWND window, bool force) noexcept
	{
		try { const auto state = Get(window); if (!state) return false; Load(*state, force); return state->ready; }
		catch (...) { return false; }
	}
}
