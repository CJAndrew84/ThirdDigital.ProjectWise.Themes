#pragma once

#include "Workflow.h"

namespace QuickAccess
{
	enum class AdminCommand : UINT { SignIn = 1, NewUser = 2, Users = 3 };
	struct NewUser { std::wstring name, description, email; };
	ContextState AdminContext(std::wstring context, bool administrator, bool restrictedAdministrator, bool administrativeSession);
	bool ValidateNewUser(const NewUser& user, std::wstring& error);
	using CreateUserCallback = bool (*)(void*, const ContextState&, const NewUser&, std::wstring& error);
	INT_PTR ShowNewUserDialog(HWND parent, HINSTANCE instance, const ContextState& shown, const wchar_t* datasource, void* context, CreateUserCallback create);
}
