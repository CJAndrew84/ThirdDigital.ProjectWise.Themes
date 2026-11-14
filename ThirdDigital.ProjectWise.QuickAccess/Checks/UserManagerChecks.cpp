#include "..\src\UserManager.h"
#include "..\src\UserManagerResources.h"

#include <commctrl.h>
#include <algorithm>
#include <map>

namespace
{
	namespace M = QuickAccess::UserManagement;
	void (*Expect)(bool, const wchar_t*) = nullptr;
	M::Snapshot Data()
	{
		M::Snapshot data;
		data.context = L"test datasource/test actor"; data.datasource = L"Test datasource"; data.project = 1;
		data.catalogueReady = data.ready = data.canCreate = true;
		data.projects = {{1, L"BLT-00003 - Design project"}, {2, L"Shared assets"}};
		data.users = {{10, L"Alice Template", L"Design coordinator", L"alice@example.test"}, {11, L"Bob Target", L"Engineer", L"bob@example.test"}, {12, L"Charlie Reviewer", L"Reviewer", L"charlie@example.test"}};
		data.lists = {{100, L"Design team", true}, {101, L"Review team", true}, {102, L"*BA", false}};
		data.members[100] = {{M::MemberKind::User, 10}};
		data.members[101] = {{M::MemberKind::Group, 7}};
		data.groups[7] = {10};
		data.members[102] = {{M::MemberKind::User, 10}};
		data.members[999] = {{M::MemberKind::User, 10}}; // Not assigned to this project.
		return data;
	}
	struct Fixture
	{
		M::Snapshot data = Data();
		bool valid = true, editable = true, throwRead = false, committedFailure = false, destroyInStep = false;
		unsigned writes = 0, inspections = 0, preparations = 0, creates = 0, destroyed = 0;
		int failWrite = -1;
		LONG loadingProject = 0;
		HWND dialog = nullptr;
		static bool Valid(void* context, bool, std::wstring& error)
		{
			auto& self = *static_cast<Fixture*>(context);
			if (!self.valid) error = L"Datasource or user changed.";
			return self.valid;
		}
		static bool Prepare(void* context, std::wstring&) { ++static_cast<Fixture*>(context)->preparations; return true; }
		static bool Begin(void* context, LONG project, M::Snapshot& data, std::wstring&)
		{
			auto& self = *static_cast<Fixture*>(context); self.loadingProject = project;
			data = self.data; data.project = project; data.ready = false; if (!project) data.catalogueReady = false;
			return true;
		}
		static M::LoadResult Step(void* context, M::Snapshot& data, std::wstring& status)
		{
			auto& self = *static_cast<Fixture*>(context);
			if (self.destroyInStep)
			{
				DestroyWindow(self.dialog);
				Expect(self.destroyed == 0, L"floating manager retains its backend while a callback destroys the window");
				return M::LoadResult::Failed;
			}
			data = self.data; data.project = self.loadingProject; data.catalogueReady = true; data.ready = data.project > 0;
			status = L"Loaded test data."; return M::LoadResult::Ready;
		}
		static bool Inspect(void* context, const M::Change& change, bool& direct, bool& editable, std::wstring& error)
		{
			auto& self = *static_cast<Fixture*>(context); ++self.inspections;
			if (self.throwRead) throw 1;
			if (!self.valid) { error = L"Datasource changed."; return false; }
			direct = M::MembershipOf(self.data, change.user, change.list).direct; editable = self.editable; return true;
		}
		static bool Write(void* context, const M::Change& change, std::wstring& error)
		{
			auto& self = *static_cast<Fixture*>(context);
			if (self.failWrite >= 0 && self.writes == static_cast<unsigned>(self.failWrite)) { error = L"Server denied this change."; return false; }
			++self.writes; auto& members = self.data.members[change.list];
			std::erase_if(members, [&](const auto& member) { return member.kind == M::MemberKind::User && member.id == change.user; });
			if (change.after) members.push_back({M::MemberKind::User, change.user});
			if (self.committedFailure) { error = L"Reported failure after commit."; return false; }
			return true;
		}
		static bool Create(void* context, HWND, M::User& user, std::wstring&)
		{
			auto& self = *static_cast<Fixture*>(context); ++self.creates;
			user = {13, L"New Project User", L"New engineer", L"new.user@example.test"}; self.data.users.push_back(user); return true;
		}
		static void Destroy(void* context) { ++static_cast<Fixture*>(context)->destroyed; }
		M::Host Host() { return {this, Valid, Prepare, Begin, Step, Inspect, Write, Create, Destroy}; }
	};
	void Finish(M::ApplyBatch& batch, M::Snapshot& snapshot, M::Plan& plan, Fixture& fixture, std::wstring& status)
	{
		for (int i = 0; i < 100 && batch.Busy(); ++i) batch.Step(snapshot, plan, fixture.Host(), status);
	}
	void CheckModel()
	{
		auto data = Data(); std::wstring error; unsigned added = 0, unavailable = 0;
		Expect(M::MembershipOf(data, 10, 100).direct && !M::MembershipOf(data, 10, 100).inherited, L"user-manager model separates direct membership");
		Expect(!M::MembershipOf(data, 10, 101).direct && M::MembershipOf(data, 10, 101).inherited, L"user-manager model resolves group inheritance");
		data.members[103] = {{M::MemberKind::UserList, 104}}; data.members[104] = {{M::MemberKind::UserList, 103}, {M::MemberKind::User, 10}};
		Expect(M::MembershipOf(data, 10, 103).inherited, L"nested membership cycles preserve a valid inherited path");
		data.members[100].push_back({M::MemberKind::UserList, 100});
		Expect(!M::MembershipOf(data, 10, 100).inherited, L"a self-cycle does not turn the root direct membership into inherited access");
		M::Plan plan;
		Expect(plan.Copy(data, 10, 11, added, unavailable, error) && added == 2 && unavailable == 1 && plan.Changes().size() == 2,
			L"template copy stages only editable project memberships and reports unavailable lists");
		Expect(!M::MembershipOf(data, 11, 100).Effective() && std::none_of(plan.Changes().begin(), plan.Changes().end(), [](const auto& change) { return change.list == 999; }),
			L"staging performs no writes and never copies unrelated datasource memberships");
		Expect(plan.Set(data, 11, 100, false, error) && plan.Changes().size() == 1, L"undoing a staged checkbox removes its pending change");
		Expect(!plan.Set(data, 11, 102, true, error), L"read-only userlists cannot be staged through the editing model");
		auto other = data; other.context = L"other datasource/actor";
		Expect(!plan.Set(other, 11, 100, true, error), L"membership plans cannot mix datasource/user contexts");
		other = data; other.project = 2;
		Expect(!plan.Set(other, 11, 100, true, error), L"membership plans cannot mix projects");
		plan.Clear(); data.groups[7].insert(11);
		Expect(plan.Copy(data, 10, 11, added, unavailable, error) && added == 1 && plan.Changes().size() == 1,
			L"template copy does not materialize an existing inherited membership as a direct grant");
		data = Data(); plan.Clear(); plan.Copy(data, 10, 11, added, unavailable, error);
		Fixture fixture; M::ApplyBatch batch;
		Expect(batch.Start(data, plan, error), L"a reviewed plan starts preflight against the loaded project");
		batch.Step(data, plan, fixture.Host(), error);
		Expect(fixture.writes == 0, L"checking the first change does not write before full-batch preflight");
		fixture.editable = false; batch.Step(data, plan, fixture.Host(), error);
		Expect(batch.State() == M::ApplyState::Failed && fixture.writes == 0 && plan.Changes().size() == 2,
			L"lost permission during preflight stops the whole batch without writes");
		fixture = {}; batch.Start(data, plan, error); fixture.failWrite = 1; Finish(batch, data, plan, fixture, error);
		Expect(batch.State() == M::ApplyState::Failed && fixture.writes == 1 && batch.Completed() == 1 && plan.Changes().size() == 1,
			L"partial server failure preserves successful changes and retains the unapplied change");
		fixture.failWrite = -1; batch.Start(data, plan, error); Finish(batch, data, plan, fixture, error);
		Expect(batch.State() == M::ApplyState::Complete && plan.Empty() && fixture.writes == 2, L"retry applies only the remaining membership change");
		data = Data(); plan.Clear(); plan.Set(data, 11, 100, true, error); fixture = {}; fixture.committedFailure = true;
		batch.Start(data, plan, error); Finish(batch, data, plan, fixture, error);
		Expect(batch.State() == M::ApplyState::Complete && plan.Empty() && fixture.writes == 1,
			L"a reported write failure is reconciled with an independently observed committed membership");
		data = Data(); plan.Set(data, 11, 100, true, error); fixture = {}; batch.Start(data, plan, error);
		fixture.valid = false; batch.Step(data, plan, fixture.Host(), error);
		Expect(batch.State() == M::ApplyState::Failed && fixture.writes == 0 && !plan.Empty(), L"datasource/user loss stops application and retains pending changes");
		fixture = {}; batch.Start(data, plan, error); fixture.throwRead = true; batch.Step(data, plan, fixture.Host(), error);
		Expect(batch.State() == M::ApplyState::Failed && fixture.writes == 0, L"membership verification exceptions are contained");
		fixture = {}; batch.Start(data, plan, error); batch.Stop(); batch.Step(data, plan, fixture.Host(), error);
		Expect(batch.State() == M::ApplyState::Stopped && fixture.writes == 0 && !plan.Empty(), L"stopping a batch does not write or discard pending edits");
		batch.Start(data, plan, error); data.context = L"changed context"; batch.Step(data, plan, fixture.Host(), error);
		Expect(batch.State() == M::ApplyState::Failed && fixture.writes == 0, L"a changed snapshot cannot redirect a prepared batch");
		data = Data(); plan.Clear(); plan.Set(data, 11, 100, true, error); data.members[100].push_back({M::MemberKind::User, 11}); plan.Rebase(data);
		Expect(plan.Empty(), L"refresh recognizes memberships already satisfied by another operation");
		data.ready = false;
		Expect(!plan.Set(data, 11, 100, false, error), L"incomplete snapshots remain read only");
	}
	template<class Predicate> bool PumpUntil(Predicate predicate)
	{
		const auto deadline = GetTickCount64() + 5000;
		do { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } if (predicate()) return true; Sleep(10); } while (GetTickCount64() < deadline);
		return predicate();
	}
	void Command(HWND dialog, int id) { SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, id))); }
	void Select(HWND view, int row)
	{
		ListView_SetItemState(view, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_SetItemState(view, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	}
	void CheckWindow(HWND owner, const wchar_t* preview, bool (*save)(HWND, const wchar_t*))
	{
		Fixture fixture;
		HWND dialog = M::Open(owner, GetModuleHandleW(nullptr), fixture.Host(), 1); fixture.dialog = dialog;
		Expect(dialog != nullptr && IsWindowEnabled(owner), L"floating project user management opens modelessly without disabling Explorer");
		if (!dialog) return;
		HWND grid = GetDlgItem(dialog, IDC_MANAGER_GRID), members = GetDlgItem(dialog, IDC_MANAGER_MEMBERS), pending = GetDlgItem(dialog, IDC_MANAGER_PENDING);
		Expect(PumpUntil([&] { return ListView_GetItemCount(grid) == 3 && IsWindowEnabled(GetDlgItem(dialog, IDC_MANAGER_NEW)); }),
			L"the floating tree and virtual membership grid load users and project lists");
		Select(grid, 0); Command(dialog, IDC_MANAGER_TEMPLATE); Select(grid, 1); Command(dialog, IDC_MANAGER_COPY);
		Expect(ListView_GetItemCount(pending) == 2 && fixture.writes == 0 && ListView_GetItemCount(members) == 3,
			L"copying a selected template populates the review and direct-membership editor without sending changes");
		ListView_SetCheckState(members, 2, TRUE);
		Expect(!ListView_GetCheckState(members, 2) && ListView_GetItemCount(pending) == 2, L"the native editor rejects checkbox changes to a read-only userlist");
		if (preview) Expect(save(dialog, preview), L"floating project/user membership manager preview written");
		Command(dialog, IDC_MANAGER_APPLY);
		Expect(PumpUntil([&] { return fixture.writes == 2 && ListView_GetItemCount(pending) == 0 && IsWindowEnabled(GetDlgItem(dialog, IDC_MANAGER_NEW)); }),
			L"Apply refreshes the project, preflights all changes and applies only the reviewed memberships");
		Expect(M::MembershipOf(fixture.data, 11, 100).direct && M::MembershipOf(fixture.data, 11, 101).direct && !M::MembershipOf(fixture.data, 11, 102).Effective(),
			L"the target receives the copied project memberships without an unauthorized role-list change");
		Command(dialog, IDC_MANAGER_NEW);
		Expect(fixture.creates == 1 && fixture.writes == 2 && ListView_GetItemCount(pending) == 2,
			L"new account creation stages template memberships for a separate reviewed Apply");
		fixture.valid = false; SendMessageW(dialog, WM_TIMER, 1, 0);
		Expect(ListView_GetItemCount(grid) == 0 && TreeView_GetCount(GetDlgItem(dialog, IDC_MANAGER_TREE)) == 0 && !IsWindowEnabled(GetDlgItem(dialog, IDC_MANAGER_APPLY)),
			L"datasource/user loss clears the visible directory and disables mutations");
		fixture.valid = true; Command(dialog, IDC_MANAGER_REFRESH);
		Expect(PumpUntil([&] { return ListView_GetItemCount(grid) == 4 && ListView_GetItemCount(pending) == 2 && IsWindowEnabled(GetDlgItem(dialog, IDC_MANAGER_APPLY)); }),
			L"returning to the original context restores pending work after an explicit refresh");
		Command(dialog, IDC_MANAGER_APPLY);
		Expect(PumpUntil([&] { return fixture.writes == 4 && ListView_GetItemCount(pending) == 0; }), L"restored pending work applies only to the newly created target user");
		SendMessageW(dialog, WM_CLOSE, 0, 0);
		Expect(!IsWindow(dialog) && fixture.destroyed == 1 && IsWindowEnabled(owner), L"closing the manager releases its backend and leaves Explorer enabled");
		Fixture destroyed; dialog = M::Open(owner, GetModuleHandleW(nullptr), destroyed.Host(), 1); destroyed.dialog = dialog; destroyed.destroyInStep = true;
		Expect(PumpUntil([&] { return !IsWindow(dialog); }) && destroyed.destroyed == 1, L"callback destruction releases the floating manager once after the callback returns");
	}
}

void CheckUserManager(HWND owner, const wchar_t* preview, void (*expect)(bool, const wchar_t*), bool (*save)(HWND, const wchar_t*))
{
	Expect = expect; CheckModel(); CheckWindow(owner, preview, save);
}
