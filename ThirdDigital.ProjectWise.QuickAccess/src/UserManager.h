#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace QuickAccess::UserManagement
{
	struct User { LONG id = 0; std::wstring name, description, email; };
	struct Project { LONG id = 0; std::wstring name; };
	struct UserList { LONG id = 0; std::wstring name; bool editable = false; };
	enum class MemberKind : LONG { User = 1, Group = 2, UserList = 3 };
	struct Member { MemberKind kind; LONG id; };
	struct Snapshot
	{
		std::wstring context, datasource;
		LONG project = 0;
		bool catalogueReady = false, ready = false, canCreate = false;
		std::vector<Project> projects;
		std::vector<User> users;
		std::vector<UserList> lists;
		std::unordered_map<LONG, std::vector<Member>> members;
		std::unordered_map<LONG, std::unordered_set<LONG>> groups;
	};
	struct Membership { bool direct = false, inherited = false; bool Effective() const { return direct || inherited; } };
	Membership MembershipOf(const Snapshot& snapshot, LONG user, LONG list);
	struct Change
	{
		LONG project = 0, user = 0, list = 0;
		bool before = false, after = false;
		std::wstring userName, listName;
	};
	class Plan
	{
		std::vector<Change> changes_;
		std::wstring context_;
	public:
		const std::vector<Change>& Changes() const { return changes_; }
		bool Empty() const { return changes_.empty(); }
		const std::wstring& Context() const { return context_; }
		void Clear() { changes_.clear(); context_.clear(); }
		bool Direct(const Snapshot& snapshot, LONG user, LONG list) const;
		bool Set(const Snapshot& snapshot, LONG user, LONG list, bool direct, std::wstring& error);
		bool Copy(const Snapshot& snapshot, LONG source, LONG target, unsigned& added, unsigned& unavailable, std::wstring& error);
		void Complete(LONG user, LONG list);
		void Rebase(const Snapshot&);
	};
	enum class LoadResult { Loading, Ready, Failed };
	struct Host
	{
		void* context = nullptr;
		bool (*valid)(void*, bool writing, std::wstring& error) = nullptr;
		bool (*prepare)(void*, std::wstring& error) = nullptr;
		bool (*begin)(void*, LONG project, Snapshot&, std::wstring& error) = nullptr;
		LoadResult (*step)(void*, Snapshot&, std::wstring& status) = nullptr;
		bool (*inspect)(void*, const Change&, bool& direct, bool& editable, std::wstring& error) = nullptr;
		bool (*write)(void*, const Change&, std::wstring& error) = nullptr;
		bool (*create)(void*, HWND parent, User&, std::wstring& error) = nullptr;
		void (*destroy)(void*) = nullptr;
	};
	enum class ApplyState { Idle, Checking, Writing, Complete, Failed, Stopped };
	class ApplyBatch
	{
		std::vector<Change> operations_;
		std::wstring context_;
		LONG project_ = 0;
		std::size_t cursor_ = 0;
		unsigned completed_ = 0;
		ApplyState state_ = ApplyState::Idle;
	public:
		bool Start(const Snapshot&, const Plan&, std::wstring& error);
		ApplyState Step(Snapshot&, Plan&, const Host&, std::wstring& status);
		void Stop() { if (Busy()) state_ = ApplyState::Stopped; }
		bool Busy() const { return state_ == ApplyState::Checking || state_ == ApplyState::Writing; }
		unsigned Completed() const { return completed_; }
		ApplyState State() const { return state_; }
	};
	// Takes ownership of host.context through host.destroy, including creation failures.
	HWND Open(HWND owner, HINSTANCE instance, Host host, LONG initialProject = 0);
}
