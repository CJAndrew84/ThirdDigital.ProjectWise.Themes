#include "UserManager.h"

#include <algorithm>

namespace QuickAccess::UserManagement
{
	namespace
	{
		bool Reachable(const Snapshot& snapshot, LONG user, LONG root)
		{
			std::vector<LONG> pending{root}; std::unordered_set<LONG> visited;
			for (std::size_t i = 0; i < pending.size() && visited.size() < 4096; ++i)
			{
				const LONG list = pending[i]; if (!visited.insert(list).second) continue;
				const auto found = snapshot.members.find(list); if (found == snapshot.members.end()) continue;
				for (const auto& member : found->second)
				{
					if (member.kind == MemberKind::User && list != root && member.id == user) return true;
					if (member.kind == MemberKind::Group)
					{
						const auto group = snapshot.groups.find(member.id);
						if (group != snapshot.groups.end() && group->second.contains(user)) return true;
					}
					if (member.kind == MemberKind::UserList && pending.size() < 32768) pending.push_back(member.id);
				}
			}
			return false;
		}
		const User* FindUser(const Snapshot& snapshot, LONG id)
		{
			const auto found = std::find_if(snapshot.users.begin(), snapshot.users.end(), [id](const auto& user) { return user.id == id; });
			return found == snapshot.users.end() ? nullptr : &*found;
		}
		const UserList* FindList(const Snapshot& snapshot, LONG id)
		{
			const auto found = std::find_if(snapshot.lists.begin(), snapshot.lists.end(), [id](const auto& list) { return list.id == id; });
			return found == snapshot.lists.end() ? nullptr : &*found;
		}
	}
	Membership MembershipOf(const Snapshot& snapshot, LONG user, LONG list)
	{
		Membership result;
		if (!snapshot.ready || user <= 0 || list <= 0) return result;
		if (const auto found = snapshot.members.find(list); found != snapshot.members.end())
			result.direct = std::any_of(found->second.begin(), found->second.end(), [user](const auto& member) { return member.kind == MemberKind::User && member.id == user; });
		result.inherited = Reachable(snapshot, user, list); return result;
	}
	bool Plan::Direct(const Snapshot& snapshot, LONG user, LONG list) const
	{
		const auto found = std::find_if(changes_.begin(), changes_.end(), [&](const auto& change) { return context_ == snapshot.context && change.project == snapshot.project && change.user == user && change.list == list; });
		return found == changes_.end() ? MembershipOf(snapshot, user, list).direct : found->after;
	}
	bool Plan::Set(const Snapshot& snapshot, LONG user, LONG list, bool direct, std::wstring& error)
	{
		error.clear();
		const auto* target = FindUser(snapshot, user); const auto* userList = FindList(snapshot, list);
		if (!snapshot.ready || snapshot.context.empty() || snapshot.project <= 0 || target == nullptr || userList == nullptr || !userList->editable)
		{ error = L"This userlist is unavailable for editing. Refresh the project and check your list-management permissions."; return false; }
		if (!changes_.empty() && (context_ != snapshot.context || changes_[0].project != snapshot.project))
		{ error = L"Apply or discard the pending changes before switching datasource or project."; return false; }
		const auto found = std::find_if(changes_.begin(), changes_.end(), [&](const auto& change) { return change.user == user && change.list == list; });
		const bool before = found == changes_.end() ? MembershipOf(snapshot, user, list).direct : found->before;
		if (before == direct)
		{
			if (found != changes_.end()) changes_.erase(found);
			if (changes_.empty()) context_.clear();
			return true;
		}
		if (found != changes_.end()) { found->after = direct; return true; }
		if (changes_.size() >= 1000) { error = L"Apply or discard the current 1000 pending changes before adding more."; return false; }
		context_ = snapshot.context;
		changes_.push_back({snapshot.project, user, list, before, direct, target->name, userList->name}); return true;
	}
	bool Plan::Copy(const Snapshot& snapshot, LONG source, LONG target, unsigned& added, unsigned& unavailable, std::wstring& error)
	{
		added = unavailable = 0; error.clear();
		if (!snapshot.ready || source == target || FindUser(snapshot, source) == nullptr || FindUser(snapshot, target) == nullptr)
		{ error = L"Select a template user and a different target user in this project."; return false; }
		Plan next = *this;
		for (const auto& list : snapshot.lists)
		{
			if (!MembershipOf(snapshot, source, list.id).Effective() || next.Direct(snapshot, target, list.id) || MembershipOf(snapshot, target, list.id).inherited) continue;
			if (!list.editable) { ++unavailable; continue; }
			if (!next.Set(snapshot, target, list.id, true, error)) { added = 0; return false; }
			++added;
		}
		*this = std::move(next); return true;
	}
	void Plan::Complete(LONG user, LONG list)
	{
		std::erase_if(changes_, [&](const auto& change) { return change.user == user && change.list == list; });
		if (changes_.empty()) context_.clear();
	}
	void Plan::Rebase(const Snapshot& snapshot)
	{
		if (!snapshot.ready || snapshot.context != context_) return;
		for (auto& change : changes_)
		{
			const auto* user = FindUser(snapshot, change.user); const auto* list = FindList(snapshot, change.list);
			if (change.project != snapshot.project || user == nullptr || list == nullptr || user->name != change.userName || list->name != change.listName) continue;
			change.before = MembershipOf(snapshot, change.user, change.list).direct;
		}
		std::erase_if(changes_, [](const auto& change) { return change.before == change.after; });
		if (changes_.empty()) context_.clear();
	}
	bool ApplyBatch::Start(const Snapshot& snapshot, const Plan& plan, std::wstring& error)
	{
		operations_.clear(); context_.clear(); project_ = 0; cursor_ = 0; completed_ = 0; state_ = ApplyState::Idle; error.clear();
		if (!snapshot.ready || snapshot.context.empty() || snapshot.context != plan.Context() || plan.Empty()) { error = L"Load the original project and review its pending changes first."; return false; }
		for (const auto& change : plan.Changes())
		{
			const auto* user = FindUser(snapshot, change.user); const auto* list = FindList(snapshot, change.list);
			if (change.project != snapshot.project || user == nullptr || list == nullptr || !list->editable || user->name != change.userName || list->name != change.listName)
			{ error = L"A user, userlist or project assignment changed. Refresh and review the pending changes."; return false; }
		}
		operations_ = plan.Changes(); context_ = snapshot.context; project_ = snapshot.project; state_ = ApplyState::Checking; return true;
	}
	ApplyState ApplyBatch::Step(Snapshot& snapshot, Plan& plan, const Host& host, std::wstring& status)
	{
		if (!Busy()) return state_;
		try
		{
			std::wstring error;
			if (!snapshot.ready || snapshot.context != context_ || snapshot.project != project_ || host.valid == nullptr ||
				!host.valid(host.context, true, error) || host.inspect == nullptr || host.write == nullptr)
			{ status = error.empty() ? L"The datasource, user or permissions changed. Unapplied changes are retained." : error; return state_ = ApplyState::Failed; }
			const auto& change = operations_[cursor_]; bool direct = false, editable = false;
			if (!host.inspect(host.context, change, direct, editable, error) || !editable)
			{ status = error.empty() ? L"A membership is no longer editable. Unapplied changes are retained." : error; return state_ = ApplyState::Failed; }
			if (state_ == ApplyState::Checking)
			{
				status = L"Checking changes: " + std::to_wstring(cursor_ + 1) + L" / " + std::to_wstring(operations_.size());
				if (++cursor_ == operations_.size()) { cursor_ = 0; state_ = ApplyState::Writing; }
				return state_;
			}
			// Inspect again immediately before each write; there is no assumed transaction spanning SDK calls.
			if (direct != change.after && !host.write(host.context, change, error))
			{
				bool observed = false, permitted = false; std::wstring ignored;
				if (!host.inspect(host.context, change, observed, permitted, ignored) || observed != change.after)
				{ status = std::to_wstring(completed_) + L" changes completed. " + (error.empty() ? L"ProjectWise rejected the next change. Remaining changes are retained." : error); return state_ = ApplyState::Failed; }
			}
			auto& members = snapshot.members[change.list];
			std::erase_if(members, [&](const auto& member) { return member.kind == MemberKind::User && member.id == change.user; });
			if (change.after) members.push_back({MemberKind::User, change.user});
			plan.Complete(change.user, change.list); ++completed_;
			status = L"Completed changes: " + std::to_wstring(completed_) + L" / " + std::to_wstring(operations_.size());
			if (++cursor_ == operations_.size()) state_ = ApplyState::Complete;
			return state_;
		}
		catch (...) { status = L"The operation stopped. Completed changes remain applied; unapplied changes are retained."; return state_ = ApplyState::Failed; }
	}
}
