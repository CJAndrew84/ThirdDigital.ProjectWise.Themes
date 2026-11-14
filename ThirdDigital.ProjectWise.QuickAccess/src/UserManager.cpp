#include "UserManager.h"
#include "UserManagerResources.h"

#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <memory>

namespace QuickAccess::UserManagement
{
	namespace
	{
		struct Node { LONG project = 0, list = 0; };
		struct Window
		{
			HWND dialog = nullptr; Host host; Snapshot snapshot; Plan plan; ApplyBatch batch;
			LONG initial = 0, selected = 0, donor = 0;
			bool owned = false, destroyed = false, loading = false, applyAfterLoad = false, invalid = false, updating = false;
			unsigned active = 0;
			std::vector<Node> nodes; std::vector<std::size_t> rows;
			std::wstring scratch; HFONT font = nullptr; HIMAGELIST spacing = nullptr;
			~Window() { if (spacing) ImageList_Destroy(spacing); if (font) DeleteObject(font); if (host.destroy) host.destroy(host.context); }
		};
		struct Lease
		{
			Window* window;
			explicit Lease(Window* value) : window(value) { if (window) ++window->active; }
			~Lease() { if (window && --window->active == 0 && window->owned && window->destroyed) delete window; }
		};
		thread_local std::vector<HWND> g_windows;
		thread_local HHOOK g_keyboard = nullptr;
		thread_local bool g_routing = false;
		Window* State(HWND dialog) { return reinterpret_cast<Window*>(GetWindowLongPtrW(dialog, DWLP_USER)); }
		bool Alive(const Window& window) { return !window.destroyed && State(window.dialog) == &window; }
		LRESULT CALLBACK Keyboard(int code, WPARAM wParam, LPARAM lParam)
		{
			if (code >= 0 && wParam == PM_REMOVE && !g_routing)
			{
				auto* message = reinterpret_cast<MSG*>(lParam);
				if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN || message->message == WM_CHAR)
					for (HWND dialog : g_windows) if (message->hwnd == dialog || IsChild(dialog, message->hwnd))
					{
						g_routing = true; const bool consumed = IsDialogMessageW(dialog, message) != FALSE; g_routing = false;
						if (consumed) message->message = WM_NULL;
						break;
					}
			}
			return CallNextHookEx(g_keyboard, code, wParam, lParam);
		}
		HWND Control(const Window& window, int id) { return GetDlgItem(window.dialog, id); }
		void Status(Window& window, const std::wstring& text) { SetDlgItemTextW(window.dialog, IDC_MANAGER_STATUS, text.c_str()); }
		bool Busy(const Window& window) { return window.loading || window.batch.Busy(); }
		const User* UserById(const Window& window, LONG id)
		{
			const auto found = std::find_if(window.snapshot.users.begin(), window.snapshot.users.end(), [id](const auto& user) { return user.id == id; });
			return found == window.snapshot.users.end() ? nullptr : &*found;
		}
		void Enable(Window& window)
		{
			const bool editable = !Busy(window) && !window.invalid && window.snapshot.ready;
			for (int id : {IDC_MANAGER_TREE, IDC_MANAGER_SEARCH, IDC_MANAGER_GRID}) EnableWindow(Control(window, id), !Busy(window) && !window.invalid);
			EnableWindow(Control(window, IDC_MANAGER_MEMBERS), editable && window.selected > 0);
			EnableWindow(Control(window, IDC_MANAGER_TEMPLATE), editable && window.selected > 0);
			EnableWindow(Control(window, IDC_MANAGER_COPY), editable && window.donor > 0 && window.selected > 0 && window.donor != window.selected);
			EnableWindow(Control(window, IDC_MANAGER_NEW), editable && window.snapshot.canCreate && window.host.create != nullptr);
			EnableWindow(Control(window, IDC_MANAGER_APPLY), editable && !window.plan.Empty());
			EnableWindow(Control(window, IDC_MANAGER_DISCARD), !Busy(window) && !window.invalid && !window.plan.Empty());
			EnableWindow(Control(window, IDC_MANAGER_REFRESH), !Busy(window));
			SetDlgItemTextW(window.dialog, IDC_MANAGER_CLOSE, Busy(window) ? L"Stop" : L"Close");
		}
		void Column(HWND view, int index, const wchar_t* text, int width)
		{
			LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<LPWSTR>(text); column.cx = width;
			ListView_InsertColumn(view, index, &column);
		}
		void Cell(HWND view, int row, int column, const std::wstring& text)
		{
			if (column == 0) { LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = const_cast<LPWSTR>(text.c_str()); ListView_InsertItem(view, &item); }
			else ListView_SetItemText(view, row, column, const_cast<LPWSTR>(text.c_str()));
		}
		void UpdatePending(Window& window)
		{
			HWND view = Control(window, IDC_MANAGER_PENDING); ListView_DeleteAllItems(view); int row = 0;
			for (const auto& change : window.plan.Changes())
			{ Cell(view, row, 0, change.userName); Cell(view, row, 1, change.listName); Cell(view, row++, 2, change.after ? L"Add" : L"Remove"); }
			SetDlgItemTextW(window.dialog, IDC_MANAGER_PENDING_LABEL, (L"Pending changes: " + std::to_wstring(window.plan.Changes().size())).c_str());
		}
		void UpdateMembers(Window& window)
		{
			window.updating = true; HWND view = Control(window, IDC_MANAGER_MEMBERS); ListView_DeleteAllItems(view);
			if (window.snapshot.ready && window.selected > 0)
				for (std::size_t i = 0; i < window.snapshot.lists.size(); ++i)
				{
					const auto& list = window.snapshot.lists[i]; const auto member = MembershipOf(window.snapshot, window.selected, list.id);
					Cell(view, static_cast<int>(i), 0, list.name);
					Cell(view, static_cast<int>(i), 1, member.inherited ? (member.direct ? L"Direct + inherited" : L"Inherited") : member.direct ? L"Direct" : L"None");
					Cell(view, static_cast<int>(i), 2, list.editable ? L"Editable" : L"Read only");
					ListView_SetCheckState(view, static_cast<int>(i), window.plan.Direct(window.snapshot, window.selected, list.id));
				}
			if (const auto* user = UserById(window, window.selected))
				SetDlgItemTextW(window.dialog, IDC_MANAGER_MEMBER_LABEL, (L"Direct memberships: " + user->name).c_str());
			else SetDlgItemTextW(window.dialog, IDC_MANAGER_MEMBER_LABEL, L"Direct memberships for selected user");
			window.updating = false; Enable(window);
		}
		bool Contains(const std::wstring& value, const std::wstring& query)
		{
			if (query.empty()) return true;
			if (query.size() > value.size()) return false;
			for (std::size_t i = 0; i <= value.size() - query.size(); ++i)
				if (CompareStringOrdinal(value.data() + i, static_cast<int>(query.size()), query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL) return true;
			return false;
		}
		void UpdateRows(Window& window)
		{
			wchar_t query[256]{}; GetDlgItemTextW(window.dialog, IDC_MANAGER_SEARCH, query, ARRAYSIZE(query));
			const LONG previous = window.selected;
			window.updating = true;
			ListView_SetItemState(Control(window, IDC_MANAGER_GRID), -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			window.selected = 0;
			window.rows.clear();
			if (window.snapshot.catalogueReady && !window.invalid)
				for (std::size_t i = 0; i < window.snapshot.users.size(); ++i)
					if (Contains(window.snapshot.users[i].name, query) || Contains(window.snapshot.users[i].email, query)) window.rows.push_back(i);
			std::sort(window.rows.begin(), window.rows.end(), [&](std::size_t a, std::size_t b) {
				const auto& left = window.snapshot.users[a]; const auto& right = window.snapshot.users[b];
				const int order = CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
				return order == CSTR_EQUAL ? left.id < right.id : order == CSTR_LESS_THAN;
			});
			ListView_SetItemCountEx(Control(window, IDC_MANAGER_GRID), static_cast<int>(window.rows.size()), LVSICF_NOINVALIDATEALL);
			for (std::size_t i = 0; i < window.rows.size(); ++i) if (window.snapshot.users[window.rows[i]].id == previous)
			{ window.selected = previous; ListView_SetItemState(Control(window, IDC_MANAGER_GRID), static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); break; }
			window.updating = false; UpdateMembers(window);
			InvalidateRect(Control(window, IDC_MANAGER_GRID), nullptr, TRUE);
		}
		void SelectUser(Window& window, LONG id)
		{
			for (std::size_t i = 0; i < window.rows.size(); ++i) if (window.snapshot.users[window.rows[i]].id == id)
			{ ListView_SetItemState(Control(window, IDC_MANAGER_GRID), static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(Control(window, IDC_MANAGER_GRID), static_cast<int>(i), FALSE); return; }
		}
		void Begin(Window& window, LONG project, bool apply = false)
		{
			std::wstring error;
			if (project == 0)
			{
				if (!window.plan.Empty()) window.initial = window.plan.Changes()[0].project;
				else if (window.snapshot.project > 0) window.initial = window.snapshot.project;
			}
			window.loading = true; window.updating = true;
			window.rows.clear(); ListView_SetItemCount(Control(window, IDC_MANAGER_GRID), 0); ListView_DeleteAllItems(Control(window, IDC_MANAGER_MEMBERS));
			if (project == 0) { TreeView_DeleteAllItems(Control(window, IDC_MANAGER_TREE)); window.nodes.clear(); }
			window.updating = false; Enable(window);
			const bool started = window.host.valid != nullptr && window.host.valid(window.host.context, false, error) &&
				window.host.begin != nullptr && window.host.begin(window.host.context, project, window.snapshot, error);
			if (!Alive(window)) return;
			if (!started)
			{ window.loading = false; Status(window, error.empty() ? L"ProjectWise could not load this project." : error); window.snapshot.ready = false; Enable(window); return; }
			window.invalid = false; window.loading = true; window.applyAfterLoad = apply; Status(window, L"Loading project information..."); Enable(window);
		}
		HTREEITEM InsertNode(Window& window, HTREEITEM parent, const std::wstring& name, Node node)
		{
			window.nodes.push_back(node);
			TVINSERTSTRUCTW item{}; item.hParent = parent; item.hInsertAfter = TVI_LAST;
			item.item.mask = TVIF_TEXT | TVIF_PARAM; item.item.pszText = const_cast<LPWSTR>(name.c_str()); item.item.lParam = static_cast<LPARAM>(window.nodes.size());
			return TreeView_InsertItem(Control(window, IDC_MANAGER_TREE), &item);
		}
		void Catalogue(Window& window)
		{
			window.updating = true; window.nodes.clear(); TreeView_DeleteAllItems(Control(window, IDC_MANAGER_TREE));
			HTREEITEM selected = nullptr;
			for (const auto& project : window.snapshot.projects)
			{
				const auto item = InsertNode(window, TVI_ROOT, project.name, {project.id, 0});
				if (selected == nullptr || project.id == window.initial) selected = item;
			}
			window.updating = false; UpdateRows(window);
			if (const auto* donor = UserById(window, window.donor))
				SetDlgItemTextW(window.dialog, IDC_MANAGER_TEMPLATE_LABEL, (L"Template user: " + donor->name + L". Only project userlist memberships are copied.").c_str());
			else { window.donor = 0; SetDlgItemTextW(window.dialog, IDC_MANAGER_TEMPLATE_LABEL, L"No template user selected"); }
			SetDlgItemTextW(window.dialog, IDC_MANAGER_SCOPE, (L"Datasource: " + window.snapshot.datasource + L". Userlists come from project security assignments.").c_str());
			if (selected != nullptr) TreeView_SelectItem(Control(window, IDC_MANAGER_TREE), selected);
			else Status(window, L"No readable work areas are available.");
		}
		void ProjectReady(Window& window)
		{
			window.updating = true;
			HWND grid = Control(window, IDC_MANAGER_GRID);
			while (Header_GetItemCount(ListView_GetHeader(grid)) > 2) ListView_DeleteColumn(grid, 2);
			const int dpi = static_cast<int>(GetDpiForWindow(window.dialog));
			for (std::size_t i = 0; i < window.snapshot.lists.size(); ++i) Column(grid, static_cast<int>(i + 2), window.snapshot.lists[i].name.c_str(), MulDiv(165, dpi, 96));
			HWND tree = Control(window, IDC_MANAGER_TREE); HTREEITEM root = TreeView_GetRoot(tree);
			while (root != nullptr)
			{
				TVITEMW item{}; item.hItem = root; item.mask = TVIF_PARAM; TreeView_GetItem(tree, &item);
				if (item.lParam > 0 && static_cast<std::size_t>(item.lParam) <= window.nodes.size() && window.nodes[static_cast<std::size_t>(item.lParam) - 1].project == window.snapshot.project)
				{
					while (HTREEITEM child = TreeView_GetChild(tree, root)) TreeView_DeleteItem(tree, child);
					for (const auto& list : window.snapshot.lists) InsertNode(window, root, list.name, {window.snapshot.project, list.id});
					TreeView_Expand(tree, root, TVE_EXPAND); break;
				}
				root = TreeView_GetNextSibling(tree, root);
			}
			window.updating = false;
			window.plan.Rebase(window.snapshot); UpdateRows(window); SelectUser(window, window.selected); UpdateMembers(window); UpdatePending(window);
			Status(window, L"Select a user to edit direct memberships. Inherited membership is shown separately. Changes are staged until Apply.");
			if (window.applyAfterLoad)
			{
				window.applyAfterLoad = false; std::wstring error;
				if (window.plan.Empty()) Status(window, L"Memberships already match the requested changes.");
				else if (!window.batch.Start(window.snapshot, window.plan, error)) Status(window, error);
			}
			Enable(window);
		}
		void Layout(Window& window)
		{
			RECT area{}; GetClientRect(window.dialog, &area); const int dpi = static_cast<int>(GetDpiForWindow(window.dialog));
			const auto s = [dpi](int n) { return MulDiv(n, dpi, 96); };
			const auto place = [&](int id, int x, int y, int w, int h) { SetWindowPos(Control(window, id), nullptr, x, y, std::max(1, w), std::max(1, h), SWP_NOZORDER | SWP_NOACTIVATE); };
			const int margin = s(12), gap = s(8), width = std::max(s(200), area.right - 2 * margin);
			constexpr std::pair<int, int> buttons[] = {{IDC_MANAGER_REFRESH, 88}, {IDC_MANAGER_TEMPLATE, 132}, {IDC_MANAGER_COPY, 150}, {IDC_MANAGER_NEW, 108}, {IDC_MANAGER_APPLY, 120}, {IDC_MANAGER_DISCARD, 140}, {IDC_MANAGER_CLOSE, 80}};
			int rows = 1, x = 0; for (const auto& button : buttons) { if (x && x + s(button.second) > width) { ++rows; x = 0; } x += s(button.second) + gap; }
			const int statusTop = area.bottom - margin - s(42), buttonTop = statusTop - gap - rows * s(34), templateTop = buttonTop - s(28);
			place(IDC_MANAGER_SCOPE, margin, s(8), width, s(24));
			const int left = std::clamp(width / 4, s(130), s(260)), right = margin + left + gap, remaining = width - left - gap;
			place(IDC_MANAGER_PROJECT_LABEL, margin, s(38), left, s(20));
			place(IDC_MANAGER_TREE, margin, s(62), left, templateTop - s(66));
			place(IDC_MANAGER_SEARCH_LABEL, right, s(38), std::min(s(220), remaining / 2), s(20));
			place(IDC_MANAGER_SEARCH, right + remaining / 2, s(34), remaining / 2, s(26));
			const int lower = s(62) + std::max(s(80), (templateTop - s(62)) * 55 / 100), half = (remaining - gap) / 2;
			place(IDC_MANAGER_GRID, right, s(66), remaining, lower - s(70));
			place(IDC_MANAGER_MEMBER_LABEL, right, lower, half, s(20)); place(IDC_MANAGER_PENDING_LABEL, right + half + gap, lower, half, s(20));
			place(IDC_MANAGER_MEMBERS, right, lower + s(24), half, templateTop - lower - s(28));
			place(IDC_MANAGER_PENDING, right + half + gap, lower + s(24), half, templateTop - lower - s(28));
			place(IDC_MANAGER_TEMPLATE_LABEL, margin, templateTop, width, s(22));
			x = 0; int y = buttonTop;
			for (const auto& button : buttons) { if (x && x + s(button.second) > width) { x = 0; y += s(34); } place(button.first, margin + x, y, s(button.second), s(30)); x += s(button.second) + gap; }
			place(IDC_MANAGER_STATUS, margin, statusTop, width, s(42));
		}
		void ClearView(Window& window)
		{
			window.batch.Stop(); window.loading = false; window.applyAfterLoad = false; window.invalid = true;
			window.snapshot = {}; window.rows.clear(); window.nodes.clear(); window.selected = window.donor = 0;
			window.updating = true; TreeView_DeleteAllItems(Control(window, IDC_MANAGER_TREE)); ListView_SetItemCount(Control(window, IDC_MANAGER_GRID), 0);
			ListView_DeleteAllItems(Control(window, IDC_MANAGER_MEMBERS)); ListView_DeleteAllItems(Control(window, IDC_MANAGER_PENDING)); window.updating = false;
			while (Header_GetItemCount(ListView_GetHeader(Control(window, IDC_MANAGER_GRID))) > 2) ListView_DeleteColumn(Control(window, IDC_MANAGER_GRID), 2);
			SetDlgItemTextW(window.dialog, IDC_MANAGER_SCOPE, L"Original datasource or Project Admin membership is unavailable.");
			SetDlgItemTextW(window.dialog, IDC_MANAGER_MEMBER_LABEL, L"Direct memberships for selected user");
			SetDlgItemTextW(window.dialog, IDC_MANAGER_TEMPLATE_LABEL, L"No template user selected"); Enable(window);
		}
		void Tick(Window& window)
		{
			std::wstring status;
			const bool valid = window.host.valid != nullptr && window.host.valid(window.host.context, false, status);
			if (!Alive(window)) return;
			if (!valid)
			{ if (!window.invalid) ClearView(window); Status(window, status.empty() ? L"Activate the original datasource and user, then Refresh." : status); return; }
			if (window.invalid) return;
			if (window.loading)
			{
				const auto result = window.host.step ? window.host.step(window.host.context, window.snapshot, status) : LoadResult::Failed;
				if (!Alive(window)) return;
				Status(window, status);
				if (result != LoadResult::Loading)
				{
					window.loading = false;
					if (result == LoadResult::Ready) { if (window.snapshot.project == 0) Catalogue(window); else ProjectReady(window); }
					else { window.snapshot.ready = false; window.applyAfterLoad = false; }
					Enable(window);
				}
			}
			else if (window.batch.Busy())
			{
				window.batch.Step(window.snapshot, window.plan, window.host, status);
				if (!Alive(window)) return;
				UpdatePending(window); UpdateMembers(window); InvalidateRect(Control(window, IDC_MANAGER_GRID), nullptr, FALSE); Status(window, status); Enable(window);
			}
		}
		INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
		{
			auto* window = State(dialog);
			if (message == WM_INITDIALOG) window = reinterpret_cast<Window*>(lParam);
			Lease lease(window);
			try
			{
				if (message == WM_INITDIALOG)
				{
					window = reinterpret_cast<Window*>(lParam); window->dialog = dialog; SetWindowLongPtrW(dialog, DWLP_USER, lParam);
					const int dpi = static_cast<int>(GetDpiForWindow(dialog));
					window->font = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(window->font), TRUE);
					for (int id : {IDC_MANAGER_GRID, IDC_MANAGER_MEMBERS, IDC_MANAGER_PENDING}) ListView_SetExtendedListViewStyle(Control(*window, id), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | (id == IDC_MANAGER_MEMBERS ? LVS_EX_CHECKBOXES : 0));
					window->spacing = ImageList_Create(1, MulDiv(24, dpi, 96), ILC_COLOR32, 1, 0);
					for (int id : {IDC_MANAGER_GRID, IDC_MANAGER_MEMBERS, IDC_MANAGER_PENDING}) ListView_SetImageList(Control(*window, id), window->spacing, LVSIL_SMALL);
					TreeView_SetItemHeight(Control(*window, IDC_MANAGER_TREE), MulDiv(24, dpi, 96));
					Column(Control(*window, IDC_MANAGER_GRID), 0, L"User", MulDiv(180, dpi, 96)); Column(Control(*window, IDC_MANAGER_GRID), 1, L"Email", MulDiv(220, dpi, 96));
					Column(Control(*window, IDC_MANAGER_MEMBERS), 0, L"Userlist (direct member)", MulDiv(210, dpi, 96)); Column(Control(*window, IDC_MANAGER_MEMBERS), 1, L"Current membership", MulDiv(150, dpi, 96)); Column(Control(*window, IDC_MANAGER_MEMBERS), 2, L"Editing", MulDiv(100, dpi, 96));
					Column(Control(*window, IDC_MANAGER_PENDING), 0, L"User", MulDiv(160, dpi, 96)); Column(Control(*window, IDC_MANAGER_PENDING), 1, L"Userlist", MulDiv(200, dpi, 96)); Column(Control(*window, IDC_MANAGER_PENDING), 2, L"Change", MulDiv(85, dpi, 96));
					MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
					SetWindowPos(dialog, nullptr, monitor.rcWork.left + 16, monitor.rcWork.top + 16,
						std::min(MulDiv(1200, dpi, 96), monitor.rcWork.right - monitor.rcWork.left - 32), std::min(MulDiv(760, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top - 32), SWP_NOZORDER | SWP_NOACTIVATE);
					Layout(*window); SetTimer(dialog, 1, 100, nullptr); Begin(*window, 0); return TRUE;
				}
				if (window == nullptr) return FALSE;
				if (message == WM_SIZE) { Layout(*window); return TRUE; }
				if (message == WM_APP + 1) { UpdateMembers(*window); return TRUE; }
				if (message == WM_APP + 2)
				{ SetForegroundWindow(dialog); SetFocus(Control(*window, IsWindowEnabled(Control(*window, IDC_MANAGER_SEARCH)) ? IDC_MANAGER_SEARCH : IDC_MANAGER_CLOSE)); return TRUE; }
				if (message == WM_TIMER) { Tick(*window); return TRUE; }
				if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wParam) == IDC_MANAGER_CLOSE || LOWORD(wParam) == IDCANCEL)))
				{
					if (Busy(*window)) { window->batch.Stop(); window->loading = false; window->applyAfterLoad = false; Status(*window, L"Stopped. Unapplied changes are retained."); Enable(*window); return TRUE; }
					if (!window->plan.Empty() && MessageBoxW(dialog, L"Discard the pending membership changes and close this window?", L"Project user management", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return TRUE;
					DestroyWindow(dialog); return TRUE;
				}
				if (message == WM_NOTIFY)
				{
					const auto* notification = reinterpret_cast<NMHDR*>(lParam);
					if (notification->idFrom == IDC_MANAGER_GRID && notification->code == LVN_GETDISPINFOW)
					{
						auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam); window->scratch.clear();
						if (info->item.iItem >= 0 && static_cast<std::size_t>(info->item.iItem) < window->rows.size() && window->rows[static_cast<std::size_t>(info->item.iItem)] < window->snapshot.users.size())
						{
							const auto& user = window->snapshot.users[window->rows[static_cast<std::size_t>(info->item.iItem)]];
							if (info->item.iSubItem == 0) window->scratch = user.name;
							else if (info->item.iSubItem == 1) window->scratch = user.email;
							else if (window->snapshot.ready && static_cast<std::size_t>(info->item.iSubItem - 2) < window->snapshot.lists.size())
							{
								const auto& list = window->snapshot.lists[static_cast<std::size_t>(info->item.iSubItem - 2)]; const auto member = MembershipOf(window->snapshot, user.id, list.id);
								const bool planned = window->plan.Direct(window->snapshot, user.id, list.id);
								window->scratch = planned != member.direct ? (planned ? L"Add pending" : member.inherited ? L"Remove direct; inherited remains" : L"Remove pending") : member.direct ? L"Direct" : member.inherited ? L"Inherited" : L"—";
							}
						}
						if (info->item.mask & LVIF_TEXT) info->item.pszText = window->scratch.data(); return TRUE;
					}
					if (window->updating) return FALSE;
					if (notification->idFrom == IDC_MANAGER_GRID && notification->code == LVN_ITEMCHANGED)
					{
						const auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
						if ((item->uNewState & LVIS_SELECTED) && item->iItem >= 0 && static_cast<std::size_t>(item->iItem) < window->rows.size())
						{ window->selected = window->snapshot.users[window->rows[static_cast<std::size_t>(item->iItem)]].id; UpdateMembers(*window); }
					}
					if (notification->idFrom == IDC_MANAGER_MEMBERS && notification->code == LVN_ITEMCHANGING)
					{
						const auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
						if ((item->uChanged & LVIF_STATE) && ((item->uNewState ^ item->uOldState) & LVIS_STATEIMAGEMASK))
						{
							const bool permitted = !Busy(*window) && !window->invalid && item->iItem >= 0 && static_cast<std::size_t>(item->iItem) < window->snapshot.lists.size() && window->snapshot.lists[static_cast<std::size_t>(item->iItem)].editable;
							if (!permitted) { SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE); Status(*window, L"This membership is read only for the current user."); return TRUE; }
						}
					}
					if (notification->idFrom == IDC_MANAGER_MEMBERS && notification->code == LVN_ITEMCHANGED)
					{
						const auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
						if ((item->uChanged & LVIF_STATE) && ((item->uNewState ^ item->uOldState) & LVIS_STATEIMAGEMASK) && item->iItem >= 0 && static_cast<std::size_t>(item->iItem) < window->snapshot.lists.size())
						{
							std::wstring error; const auto& list = window->snapshot.lists[static_cast<std::size_t>(item->iItem)];
							if (!window->plan.Set(window->snapshot, window->selected, list.id, ListView_GetCheckState(Control(*window, IDC_MANAGER_MEMBERS), item->iItem) != FALSE, error))
							{ Status(*window, error); PostMessageW(dialog, WM_APP + 1, 0, 0); }
							UpdatePending(*window); InvalidateRect(Control(*window, IDC_MANAGER_GRID), nullptr, FALSE); Enable(*window);
						}
					}
					if (notification->idFrom == IDC_MANAGER_TREE && notification->code == TVN_SELCHANGINGW && !window->plan.Empty())
					{
						const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam); const auto index = static_cast<std::size_t>(change->itemNew.lParam);
						if (index > 0 && index <= window->nodes.size() && window->nodes[index - 1].project != window->plan.Changes()[0].project)
						{ SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE); Status(*window, L"Apply or discard pending changes before selecting another project."); return TRUE; }
					}
					if (notification->idFrom == IDC_MANAGER_TREE && notification->code == TVN_SELCHANGEDW)
					{
						const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam); const auto index = static_cast<std::size_t>(change->itemNew.lParam);
						if (index > 0 && index <= window->nodes.size())
						{
							const auto node = window->nodes[index - 1];
							if (node.project != window->snapshot.project || !window->snapshot.ready) Begin(*window, node.project);
							else if (node.list) for (std::size_t i = 0; i < window->snapshot.lists.size(); ++i) if (window->snapshot.lists[i].id == node.list)
							{ ListView_EnsureVisible(Control(*window, IDC_MANAGER_MEMBERS), static_cast<int>(i), FALSE); ListView_SetSelectedColumn(Control(*window, IDC_MANAGER_GRID), static_cast<int>(i + 2)); break; }
						}
					}
					return FALSE;
				}
				if (message == WM_COMMAND)
				{
					const int id = LOWORD(wParam);
					if (id == IDC_MANAGER_SEARCH && HIWORD(wParam) == EN_CHANGE) { UpdateRows(*window); return TRUE; }
					if (Busy(*window)) return TRUE;
					std::wstring error;
					if (id == IDC_MANAGER_REFRESH) { Begin(*window, 0); return TRUE; }
					const bool valid = !window->invalid && window->host.valid != nullptr && window->host.valid(window->host.context, false, error);
					if (!Alive(*window)) return TRUE;
					if (!valid) { Status(*window, error); return TRUE; }
					if (id == IDC_MANAGER_TEMPLATE && window->selected > 0)
					{ window->donor = window->selected; if (const auto* user = UserById(*window, window->donor)) SetDlgItemTextW(dialog, IDC_MANAGER_TEMPLATE_LABEL, (L"Template user: " + user->name + L". Copy adds project userlist memberships; credentials are not copied.").c_str()); Enable(*window); return TRUE; }
					if (id == IDC_MANAGER_COPY)
					{
						unsigned added = 0, skipped = 0;
						if (!window->plan.Copy(window->snapshot, window->donor, window->selected, added, skipped, error)) Status(*window, error);
						else Status(*window, std::to_wstring(added) + L" template memberships selected; " + std::to_wstring(skipped) + L" unavailable. Review pending changes before Apply.");
						UpdateMembers(*window); UpdatePending(*window); InvalidateRect(Control(*window, IDC_MANAGER_GRID), nullptr, FALSE); Enable(*window); return TRUE;
					}
					if (id == IDC_MANAGER_DISCARD) { window->plan.Clear(); UpdatePending(*window); UpdateMembers(*window); InvalidateRect(Control(*window, IDC_MANAGER_GRID), nullptr, FALSE); Enable(*window); return TRUE; }
					if (id == IDC_MANAGER_APPLY && !window->plan.Empty())
					{
						const bool prepared = !window->host.prepare || window->host.prepare(window->host.context, error);
						if (!Alive(*window)) return TRUE;
						if (!prepared) Status(*window, error); else Begin(*window, window->snapshot.project, true); return TRUE;
					}
					if (id == IDC_MANAGER_NEW && window->snapshot.ready && window->snapshot.canCreate && window->host.create)
					{
						User user;
						const bool created = window->host.create(window->host.context, dialog, user, error);
						if (!Alive(*window)) return TRUE;
						if (!created) { Status(*window, error); return TRUE; }
						if (user.id <= 0) { Status(*window, L"The account could not be identified. Refresh users before trying again."); return TRUE; }
						window->snapshot.users.push_back(user); SetDlgItemTextW(dialog, IDC_MANAGER_SEARCH, L""); UpdateRows(*window); window->selected = user.id; SelectUser(*window, user.id);
						unsigned added = 0, skipped = 0;
						if (window->donor > 0 && !window->plan.Copy(window->snapshot, window->donor, user.id, added, skipped, error)) Status(*window, L"Account created. " + error);
						else Status(*window, L"Account created. " + std::to_wstring(added) + L" template memberships selected; " + std::to_wstring(skipped) + L" unavailable. Review and Apply memberships separately.");
						UpdateMembers(*window); UpdatePending(*window); Enable(*window); return TRUE;
					}
				}
				if (message == WM_NCDESTROY)
				{
					KillTimer(dialog, 1); SetWindowLongPtrW(dialog, DWLP_USER, 0); std::erase(g_windows, dialog);
					if (g_windows.empty() && g_keyboard != nullptr) { UnhookWindowsHookEx(g_keyboard); g_keyboard = nullptr; }
					window->destroyed = true; return FALSE;
				}
			}
			catch (...) { if (window && Alive(*window)) { window->loading = false; window->batch.Stop(); window->snapshot.ready = false; Status(*window, L"The operation stopped. Refresh to continue; pending changes are retained."); Enable(*window); } return TRUE; }
			return FALSE;
		}
	}
	HWND Open(HWND owner, HINSTANCE instance, Host host, LONG initialProject)
	{
		std::unique_ptr<Window> window;
		try { window = std::make_unique<Window>(); }
		catch (...) { if (host.destroy) host.destroy(host.context); return nullptr; }
		window->host = host; window->initial = initialProject;
		INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES};
		if (!InitCommonControlsEx(&controls)) return nullptr;
		HWND dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_USER_MANAGER), owner, Procedure, reinterpret_cast<LPARAM>(window.get()));
		if (dialog == nullptr || !IsWindow(dialog) || State(dialog) != window.get()) return nullptr;
		try
		{
			g_windows.push_back(dialog);
			if (g_keyboard == nullptr) g_keyboard = SetWindowsHookExW(WH_GETMESSAGE, Keyboard, nullptr, GetCurrentThreadId());
			if (g_keyboard == nullptr) { DestroyWindow(dialog); return nullptr; }
		}
		catch (...) { DestroyWindow(dialog); return nullptr; }
		window->owned = true;
		window.release(); ShowWindow(dialog, SW_SHOWNORMAL); SetForegroundWindow(dialog); PostMessageW(dialog, WM_APP + 2, 0, 0); return dialog;
	}
}
