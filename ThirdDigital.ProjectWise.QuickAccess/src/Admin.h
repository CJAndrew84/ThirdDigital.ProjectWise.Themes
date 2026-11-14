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
	bool ValidateNewUser(const NewUser& user, std::wstring& error, int* invalidControl = nullptr);
	std::wstring UserCreationError(LONG code, const std::wstring& message, const std::wstring& detail);
	using CreateUserCallback = bool (*)(void*, const ContextState&, const NewUser&, std::wstring& error);
	INT_PTR ShowNewUserDialog(HWND parent, HINSTANCE instance, const ContextState& shown, const wchar_t* datasource, void* context, CreateUserCallback create);
}
