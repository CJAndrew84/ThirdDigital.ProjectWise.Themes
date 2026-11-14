#include "Admin.h"
#include "AdminResources.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace QuickAccess
{
	ContextState AdminContext(std::wstring context, bool administrator, bool restrictedAdministrator, bool administrativeSession)
	{
		if (context.empty() || (!administrator && !restrictedAdministrator)) return {};
		return {true, false, std::move(context), {
			{static_cast<UINT>(AdminCommand::NewUser), L"New User", administrativeSession, ContextActionKind::NewUser},
			{static_cast<UINT>(AdminCommand::Users), L"Users and Groups", true, ContextActionKind::Users},
			{static_cast<UINT>(AdminCommand::SignIn), L"Administrative Sign-in", !administrativeSession, ContextActionKind::SignIn}}};
	}

	bool ValidateNewUser(const NewUser& user, std::wstring& error)
	{
		if (user.name.empty() || user.name.size() > 255 || user.description.size() > 1024 || user.email.size() > 320 ||
			std::all_of(user.name.begin(), user.name.end(), [](wchar_t c) { return iswspace(c) != 0; }) ||
			std::any_of(user.name.begin(), user.name.end(), [](wchar_t c) { return iswcntrl(c) != 0; }))
		{ error = L"Enter a user name. ProjectWise validates account names and uniqueness."; return false; }
		error.clear(); return true;
	}

	namespace
	{
		struct DialogState { const ContextState& shown; const wchar_t* datasource; void* context; CreateUserCallback create; };
		std::wstring Value(HWND dialog, int id)
		{
			const HWND edit = GetDlgItem(dialog, id);
			std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
			text.resize(static_cast<std::size_t>(GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
			return text;
		}
		INT_PTR CALLBACK UserDialog(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
		{
			try
			{
				if (message == WM_INITDIALOG)
				{
					auto* state = reinterpret_cast<DialogState*>(lParam);
					SetWindowLongPtrW(dialog, DWLP_USER, lParam);
					SetDlgItemTextW(dialog, IDC_USER_DATASOURCE, state->datasource);
					for (const auto [id, limit] : {std::pair{IDC_USER_NAME, 255}, {IDC_USER_DESCRIPTION, 1024}, {IDC_USER_EMAIL, 320}})
						SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
					return TRUE;
				}
				if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
				if (message != WM_COMMAND || LOWORD(wParam) != IDOK) return FALSE;
				const auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
				if (state == nullptr || state->create == nullptr) return TRUE;
				NewUser user{Value(dialog, IDC_USER_NAME), Value(dialog, IDC_USER_DESCRIPTION), Value(dialog, IDC_USER_EMAIL)};
				std::wstring error;
				if (ValidateNewUser(user, error) && state->create(state->context, state->shown, user, error)) { EndDialog(dialog, IDOK); return TRUE; }
				SetDlgItemTextW(dialog, IDC_USER_STATUS, error.empty() ? L"ProjectWise did not create the account. Check your session and Users permissions." : error.c_str());
				NotifyWinEvent(EVENT_SYSTEM_ALERT, dialog, OBJID_CLIENT, CHILDID_SELF);
				SetFocus(GetDlgItem(dialog, IDC_USER_NAME));
				return TRUE;
			}
			catch (...) { SetDlgItemTextW(dialog, IDC_USER_STATUS, L"The account could not be created."); return TRUE; }
		}
	}

	INT_PTR ShowNewUserDialog(HWND parent, HINSTANCE instance, const ContextState& shown, const wchar_t* datasource, void* context, CreateUserCallback create)
	{
		DialogState state{shown, datasource, context, create};
		return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_NEW_USER), parent, UserDialog, reinterpret_cast<LPARAM>(&state));
	}
}
