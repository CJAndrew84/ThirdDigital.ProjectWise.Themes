#include "..\src\VersionStacks.h"

#include <oleacc.h>
#include <algorithm>
#include <string>

namespace
{
	namespace VS = QuickAccess::VersionStacks;
	void (*g_expect)(bool, const wchar_t*) = nullptr;
	void Expect(bool value, const wchar_t* text) { g_expect(value, text); }
	VS::Row Document(LONG project, LONG id, LONG original, LONG sequence, const wchar_t* version)
	{
		VS::Row row;
		row.project = project; row.document = id; row.original = original; row.sequence = sequence;
		row.groupable = true; row.historical = original > 0;
		row.identity = std::to_wstring(project) + L"/" + std::to_wstring(id) + L"/" + std::to_wstring(sequence);
		row.cells = {L"Drawing.dgn", version, row.historical ? L"Version" : L"Current"};
		return row;
	}
	VS::Snapshot Fixture()
	{
		VS::Snapshot result;
		result.context = L"datasource 1/user 42/folder 100";
		result.columns = {{L"Name", 290}, {L"Version", 90}, {L"Status", 130}};
		result.columnOrder = {0, 1, 2};
		VS::Row folder; folder.project = 101; folder.cells = {L"Design models", L"", L"Folder"}; folder.identity = L"folder101";
		result.rows = {folder, Document(100, 2, 10, 2, L"Z"), Document(100, 20, 0, 7, L"A"),
			Document(100, 10, 0, 10, L"B"), Document(100, 9, 10, 9, L"C"),
			// The inaccessible/filtered active document 30 is deliberately absent.
			Document(100, 31, 30, 1, L"P1"), Document(100, 33, 30, 3, L"P3")};
		result.rows[2].cells[0] = L"Drawing.dgn"; // an unrelated document with exactly the same name
		result.rows[5].cells[0] = result.rows[6].cells[0] = L"Visible history.dgn";
		for (std::size_t i = 0; i < result.rows.size(); ++i) result.rows[i].sourceIndex = static_cast<int>(i);
		return result;
	}
	void Pump()
	{
		MSG message{};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
	}
	std::wstring Text(HWND list, int row, int column)
	{
		wchar_t value[512]{}; ListView_GetItemText(list, row, column, value, ARRAYSIZE(value)); return value;
	}
	std::wstring Accessible(HWND list, LONG child, bool description)
	{
		IAccessible* accessible = nullptr;
		if (FAILED(AccessibleObjectFromWindow(list, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&accessible)))) return {};
		VARIANT target{}; target.vt = VT_I4; target.lVal = child;
		BSTR text = nullptr;
		if (description) accessible->get_accDescription(target, &text); else accessible->get_accName(target, &text);
		std::wstring result = text ? text : L"";
		SysFreeString(text); accessible->Release(); return result;
	}
	struct FakeList
	{
		VS::Snapshot data = Fixture();
		bool available = true, dirty = true, throwRead = false;
		unsigned selections = 0, activations = 0, menus = 0, classics = 0, destroyed = 0;
		std::vector<std::size_t> selected;
		LONG openedSequence = -1;
		static VS::ReadResult Read(void* context, VS::Snapshot& result, bool force)
		{
			auto& self = *static_cast<FakeList*>(context);
			if (self.throwRead) throw 1;
			if (!self.available) { result = self.data; result.message = L"Data unavailable. Use Classic list."; return VS::ReadResult::Unavailable; }
			if (!force && !self.dirty) return VS::ReadResult::Unchanged;
			result = self.data; self.dirty = false; return VS::ReadResult::Ready;
		}
		bool Matches(const VS::Snapshot& shown) const
		{
			if (!available || shown.context != data.context || shown.rows.size() != data.rows.size()) return false;
			for (std::size_t i = 0; i < data.rows.size(); ++i) if (shown.rows[i].identity != data.rows[i].identity) return false;
			return true;
		}
		static bool Select(void* context, const VS::Snapshot& shown, const std::vector<std::size_t>& rows, std::size_t focus)
		{
			auto& self = *static_cast<FakeList*>(context);
			if (!self.Matches(shown)) return false;
			++self.selections; self.selected = rows;
			for (std::size_t i = 0; i < self.data.rows.size(); ++i)
			{
				self.data.rows[i].selected = std::find(rows.begin(), rows.end(), i) != rows.end();
				self.data.rows[i].focused = i == focus;
			}
			return true;
		}
		static bool Activate(void* context, const VS::Snapshot& shown, std::size_t row)
		{
			auto& self = *static_cast<FakeList*>(context);
			if (!self.Matches(shown) || row >= shown.rows.size() || !shown.rows[row].selected) return false;
			++self.activations; self.openedSequence = shown.rows[row].sequence; return true;
		}
		static bool Menu(void* context, const VS::Snapshot& shown, POINT)
		{
			auto& self = *static_cast<FakeList*>(context);
			if (!self.Matches(shown)) return false;
			++self.menus; return true;
		}
		static void Classic(void* context) { ++static_cast<FakeList*>(context)->classics; }
		static void Destroy(void* context) { ++static_cast<FakeList*>(context)->destroyed; }
		VS::Host Callbacks() { return {this, Read, Select, Activate, Menu, Classic, Destroy}; }
	};
	void Models()
	{
		auto fixture = Fixture();
		auto groups = VS::GroupRows(fixture.rows);
		Expect(groups.size() == 4, L"version families use original document identity, keeping folders and same-name documents separate");
		Expect(groups[1].rows == std::vector<std::size_t>({3, 4, 1}), L"visible versions sort by numeric sequence 10, 9, 2 rather than version-label text");
		Expect(groups[3].rows == std::vector<std::size_t>({6, 5}), L"highest permitted visible historical sequence is parent when the active document is absent");
		Expect(VS::VisibleRows(groups, {}).size() == 4, L"collapsed stacks show one selectable visible parent per family");
		const auto expanded = VS::VisibleRows(groups, {{100, 10}});
		Expect(expanded.size() == 6 && expanded[1].row == 3 && expanded[2].row == 4 && expanded[3].row == 1 && !expanded[2].parent,
			L"expanding one family adds only its older visible versions in descending order");
		fixture.rows.push_back(Document(200, 10, 0, 20, L"same document ID in another project"));
		Expect(VS::GroupRows(fixture.rows).size() == 5, L"document IDs in different folders never merge into one version stack");
		fixture = Fixture(); fixture.rows.erase(fixture.rows.begin() + 3);
		groups = VS::GroupRows(fixture.rows);
		Expect(fixture.rows[groups[1].rows[0]].sequence == 9 && groups[1].rows.size() == 2,
			L"a filtered-out newer version contributes neither a parent nor a count");
		fixture = Fixture(); fixture.rows[1].sequence = fixture.rows[3].sequence;
		Expect(VS::GroupRows(fixture.rows)[1].rows[0] == 3, L"equal numeric sequences deterministically prefer the active visible row");
		fixture = Fixture(); fixture.rows[0].groupable = false;
		Expect(VS::GroupRows(fixture.rows)[0].rows == std::vector<std::size_t>({0}), L"non-document rows survive grouping unchanged");
	}
}

void CheckVersionStacks(const wchar_t* preview, void (*expect)(bool, const wchar_t*), bool (*save)(HWND, const wchar_t*))
{
	g_expect = expect; Models();
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	HWND frame = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"Version stacks native checks", WS_OVERLAPPEDWINDOW,
		0, 0, 940, 540, nullptr, nullptr, instance, nullptr);
	FakeList fake;
	HWND view = VS::Create(frame, instance, fake.Callbacks());
	Expect(view != nullptr, L"SDK-free version stacks use a real native report list view");
	if (!view) { DestroyWindow(frame); return; }
	MoveWindow(view, 0, 0, 920, 480, TRUE); ShowWindow(frame, SW_SHOWNOACTIVATE); ShowWindow(view, SW_SHOWNOACTIVATE);
	Expect(VS::Refresh(view, true), L"visible document-list snapshot initializes version stacks");
	const HWND list = GetDlgItem(view, VS::kList);
	Expect(ListView_GetItemCount(list) == 4 && Text(list, 1, 4) == L"10" && Text(list, 3, 4) == L"3",
		L"native parent rows show the highest visible numeric sequence, including restricted visible history");
	Expect(Text(list, 1, 1) == L"Drawing.dgn" && Text(list, 2, 1) == L"Drawing.dgn", L"same-name independent documents remain separate native rows");
	Expect(Header_GetItemCount(ListView_GetHeader(list)) == 5, L"native columns are retained beside stack controls and numeric sequence");
	Expect(Accessible(list, CHILDID_SELF, false) == L"Document version stacks", L"the native version list has an explicit accessible name");
	Expect(Accessible(list, 2, false).find(L"sequence 10") != std::wstring::npos && Accessible(list, 2, true).find(L"collapsed") != std::wstring::npos,
		L"screen-reader row annotations identify sequence and collapsed visible-version count");
	ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); SetFocus(list); Pump();
	Expect(fake.selected == std::vector<std::size_t>({3}), L"selecting a stack parent selects only that document version, never its history");
	SendMessageW(list, WM_KEYDOWN, VK_RIGHT, 0); Pump();
	Expect(ListView_GetItemCount(list) == 6 && Text(list, 2, 4) == L"9" && Text(list, 3, 4) == L"2", L"Right arrow expands older visible versions in descending order");
	Expect(Accessible(list, 2, true).find(L"expanded") != std::wstring::npos, L"expanded stack descriptions update for assistive technology");
	ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_SetItemState(list, 2, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); Pump();
	SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
	Expect(fake.activations == 1 && fake.openedSequence == 9 && fake.selected == std::vector<std::size_t>({4}),
		L"Enter on an older row activates exactly that selected version");
	SendMessageW(list, WM_KEYDOWN, VK_RETURN, 1L << 30);
	Expect(fake.activations == 1, L"held Enter cannot repeatedly activate a version");
	SendMessageW(list, WM_KEYDOWN, VK_LEFT, 0); Pump();
	Expect(ListView_GetItemCount(list) == 4 && fake.selected == std::vector<std::size_t>({3}) && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 1,
		L"collapsing selected history moves selection and focus to the visible parent");
	SendMessageW(GetDlgItem(view, VS::kExpand), BM_CLICK, 0, 0); Pump();
	Expect(ListView_GetItemCount(list) == 7, L"Expand all exposes only the seven rows in the native visible snapshot");
	ListView_SetItemState(list, 4, LVIS_SELECTED, LVIS_SELECTED); Pump();
	Expect(fake.selected == std::vector<std::size_t>({2, 3}), L"native multiselection preserves independent version identities across families");
	if (preview) Expect(save(view, preview), L"native expanded version-stack preview written");
	SendMessageW(GetDlgItem(view, VS::kCollapse), BM_CLICK, 0, 0); Pump();
	Expect(ListView_GetItemCount(list) == 4 && fake.selected == std::vector<std::size_t>({2, 3}), L"Collapse all preserves visible selections from different families");
	SendMessageW(list, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(list), MAKELPARAM(-1, -1));
	Expect(fake.menus == 1, L"keyboard context menu delegates the validated visible selection to the host");
	const unsigned before = fake.activations;
	EnableWindow(frame, FALSE); SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
	SendMessageW(GetDlgItem(view, VS::kClassic), BM_CLICK, 0, 0);
	Expect(fake.activations == before && fake.classics == 0, L"modal Explorer ownership blocks version actions and Classic switching"); EnableWindow(frame, TRUE);
	fake.data.context = L"different datasource or user";
	SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0); Pump();
	Expect(fake.activations == before, L"changing datasource or user rejects activation from the stale stacked view");
	fake.available = false;
	Expect(!VS::Refresh(view, true) && ListView_GetItemCount(list) == 0 && !IsWindowEnabled(list),
		L"failed reads remove all stale rows and disable the custom view without affecting Classic");
	SendMessageW(GetDlgItem(view, VS::kClassic), WM_KEYDOWN, VK_RETURN, 0);
	Expect(fake.classics == 1, L"Classic list remains keyboard-accessible after a read failure");
	fake.available = true; fake.data = Fixture();
	fake.data.rows[4].selected = fake.data.rows[4].focused = true;
	Expect(VS::Refresh(view, true) && ListView_GetItemCount(list) == 6 && Text(list, ListView_GetNextItem(list, -1, LVNI_FOCUSED), 4) == L"9",
		L"a selected older version in Explorer is revealed automatically rather than hidden in a collapsed stack");
	fake.throwRead = true;
	Expect(!VS::Refresh(view, true) && ListView_GetItemCount(list) == 0, L"a host read exception leaves an empty recoverable custom view");
	fake.throwRead = false; VS::Refresh(view, true);
	SendMessageW(GetDlgItem(view, VS::kExpand), WM_KEYDOWN, VK_TAB, 0);
	Expect(GetFocus() != nullptr, L"native stack buttons retain keyboard focus traversal");
	DestroyWindow(view);
	Expect(fake.destroyed == 1, L"closing a stacked view releases its host exactly once");
	FakeList rejected;
	Expect(VS::Create(nullptr, instance, rejected.Callbacks()) == nullptr && rejected.destroyed == 1,
		L"invalid stacked-view parents release host ownership safely");
	DestroyWindow(frame); Pump();
}
