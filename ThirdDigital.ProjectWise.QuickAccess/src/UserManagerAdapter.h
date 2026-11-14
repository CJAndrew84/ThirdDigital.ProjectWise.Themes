#pragma once
#include "UserManager.h"

namespace QuickAccess
{
	struct UserManagerServices
	{
		void* context = nullptr;
		bool (*eligible)(void*, bool fresh, std::wstring& error) = nullptr;
		bool (*prepare)(void*, std::wstring& error) = nullptr;
		bool (*canCreate)(void*) = nullptr;
		bool (*create)(void*, HWND parent, UserManagement::User&, std::wstring& error) = nullptr;
		void (*destroy)(void*) = nullptr;
	};
	// Services/context ownership is transferred, including failures to create the window.
	HWND OpenProjectUserManager(HWND owner, HINSTANCE instance, LONG initialProject, UserManagerServices services);
}
