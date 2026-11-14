#pragma once

#include "Workflow.h"

namespace QuickAccess
{
	enum class AdminCommand : UINT { NewUser = 2, Users = 3 };
	enum class AccountType { Federated = 0, Logical = 1 };
	inline constexpr std::size_t kMaxLogicalPasswordLength = 43;
	struct NewUser
	{
		std::wstring name, description, email;
		AccountType type = AccountType::Federated;
		std::wstring password, confirmation;
		NewUser() = default;
		NewUser(const NewUser&) = delete;
		NewUser& operator=(const NewUser&) = delete;
		~NewUser();
		void ClearCredentials() noexcept;
	};
	ContextState AdminContext(std::wstring context, bool administrator, bool restrictedAdministrator);
	enum class AdminAccess { Ready, Cancelled, Failed };
	struct AdminSession { ContextState commands; bool administrative = false; };
	struct AdminSessionHost
	{
		void* context = nullptr;
		bool (*read)(void*, AdminSession&) = nullptr;
		AdminAccess (*login)(void*, const ContextState&, std::wstring& error) = nullptr;
	};
	AdminAccess EnsureAdminSession(const AdminSessionHost& host, const ContextState& shown, UINT command, ContextState& ready, std::wstring& error);
	enum class ProjectAdminCommand : UINT { NewUser = 1, Members = 2, Properties = 3, Access = 4 };
	enum class RoleMemberKind : LONG { User = 1, Group = 2, UserList = 3 };
	struct RoleList { LONG id = 0; std::wstring name; };
	bool IsProjectAdminList(const std::wstring& name) noexcept;
	using ReadRoleParents = bool (*)(void*, RoleMemberKind, LONG member, std::vector<RoleList>& parents);
	RoleList FindProjectAdminRole(LONG user, const std::vector<LONG>& groups, void* context, ReadRoleParents read);
	ContextState ProjectAdminContext(std::wstring context, const std::wstring& role, bool canCreateUsers, bool hasProject, const std::wstring& projectName);
	bool ValidateNewUser(const NewUser& user, std::wstring& error, int* invalidControl = nullptr);
	std::wstring UserCreationError(LONG code, const std::wstring& message, const std::wstring& detail);
	using CreateUserCallback = bool (*)(void*, const ContextState&, const NewUser&, std::wstring& error);
	INT_PTR ShowNewUserDialog(HWND parent, HINSTANCE instance, const ContextState& shown, const wchar_t* datasource, void* context, CreateUserCallback create);
}
