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

	void NewUser::ClearCredentials() noexcept
	{
		for (auto* value : {&password, &confirmation})
		{
			if (!value->empty()) SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
			value->clear();
		}
	}
	NewUser::~NewUser() { ClearCredentials(); }

	bool ValidateNewUser(const NewUser& user, std::wstring& error, int* invalidControl)
	{
		if (invalidControl != nullptr) *invalidControl = 0;
		const auto invalid = [&](int control, const wchar_t* text)
		{
			error = text; if (invalidControl != nullptr) *invalidControl = control; return false;
		};
		if (user.type != AccountType::Federated && user.type != AccountType::Logical)
			return invalid(IDC_USER_TYPE, L"Select Federated identity or Logical as the account type.");
		if (user.name.empty() || user.name.size() > 255 ||
			std::all_of(user.name.begin(), user.name.end(), [](wchar_t c) { return iswspace(c) != 0; }) ||
			std::any_of(user.name.begin(), user.name.end(), [](wchar_t c) { return iswcntrl(c) != 0; }))
			return invalid(IDC_USER_NAME, L"Enter a user name of up to 255 characters. ProjectWise validates account names and uniqueness.");
		if (user.description.size() > 1024 || user.description.find(L'\0') != std::wstring::npos)
			return invalid(IDC_USER_DESCRIPTION, L"Enter a description of up to 1024 characters.");
		if (user.email.empty() && user.type == AccountType::Federated)
			return invalid(IDC_USER_EMAIL, L"Enter the primary email address of the user's Bentley IMS identity.");
		if (!user.email.empty())
		{
			const auto at = user.email.find(L'@');
			if (user.email.size() > 320 || at == 0 || at == std::wstring::npos || at + 1 == user.email.size() ||
				user.email.find(L'@', at + 1) != std::wstring::npos ||
				std::any_of(user.email.begin(), user.email.end(), [](wchar_t c) { return iswspace(c) != 0 || iswcntrl(c) != 0 || c == L';' || c == L','; }))
				return invalid(IDC_USER_EMAIL, L"Enter one email address, without spaces or a display name.");
		}
		if (user.type == AccountType::Logical)
		{
			if (user.password.empty() || user.password.size() > kMaxLogicalPasswordLength || user.password.find(L'\0') != std::wstring::npos)
				return invalid(IDC_USER_PASSWORD, L"Enter a ProjectWise password of 1 to 43 characters. Datasource password policy also applies.");
			if (user.password != user.confirmation)
				return invalid(IDC_USER_PASSWORD, L"The passwords do not match. Re-enter both password fields.");
		}
		else if (!user.password.empty() || !user.confirmation.empty())
			return invalid(IDC_USER_TYPE, L"Federated accounts use the identity provider's credentials. Clear the ProjectWise password fields.");
		error.clear(); return true;
	}
	std::wstring UserCreationError(LONG code, const std::wstring& message, const std::wstring& detail)
	{
		std::wstring result = L"ProjectWise did not create the account" + (code ? L" (error " + std::to_wstring(code) + L")." : std::wstring(L"."));
		if (!message.empty()) result += L"\r\n" + message;
		if (!detail.empty() && detail != message) result += L"\r\n" + detail;
		if (message.empty() && detail.empty()) result += code == 58004 ?
			L"\r\nThe SDK reported invalid data. This code alone does not identify a permissions problem. Check this datasource's account/identity requirements." :
			L"\r\nThe SDK returned no additional detail. Check the administrative session and datasource account policy.";
		return result;
	}

	namespace
	{
		struct DialogState
		{
			const ContextState& shown; const wchar_t* datasource; void* context; CreateUserCallback create;
			bool logicalLayout = true; // Resource positions include both password rows.
			bool creating = false;
		};
		std::wstring Value(HWND dialog, int id)
		{
			const HWND edit = GetDlgItem(dialog, id);
			std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
			text.resize(static_cast<std::size_t>(GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
			return text;
		}
		void ReadPassword(HWND dialog, int id, std::wstring& text)
		{
			const HWND edit = GetDlgItem(dialog, id);
			text.resize(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1);
			const int count = GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
			text.resize(static_cast<std::size_t>(count));
		}
		void ClearPasswords(HWND dialog)
		{
			SetDlgItemTextW(dialog, IDC_USER_PASSWORD, L"");
			SetDlgItemTextW(dialog, IDC_USER_CONFIRMATION, L"");
		}
		AccountType SelectedType(HWND dialog)
		{
			const auto index = SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_GETCURSEL, 0, 0);
			return index == CB_ERR ? static_cast<AccountType>(-1) :
				static_cast<AccountType>(SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
		}
		void RefreshType(HWND dialog, DialogState& state)
		{
			const bool logical = SelectedType(dialog) == AccountType::Logical;
			if (!logical) ClearPasswords(dialog);
			for (const int id : {IDC_USER_PASSWORD_LABEL, IDC_USER_PASSWORD, IDC_USER_CONFIRMATION_LABEL, IDC_USER_CONFIRMATION})
			{
				EnableWindow(GetDlgItem(dialog, id), logical);
				ShowWindow(GetDlgItem(dialog, id), logical ? SW_SHOWNA : SW_HIDE);
			}
			SetDlgItemTextW(dialog, IDC_USER_EMAIL_LABEL, logical ? L"Contact &email" : L"Identity &email");
			SetDlgItemTextW(dialog, IDC_USER_HINT, logical ?
				L"Sets a ProjectWise password (up to 43 characters). Datasource password policy applies. Contact email is optional. Re-enter passwords after an error." :
				L"Uses this email as the Bentley IMS identity. Sign-in uses the user's existing IMS/provider credentials; no ProjectWise password is set.");
			if (logical != state.logicalLayout)
			{
				RECT shift{0, 0, 0, 46}; MapDialogRect(dialog, &shift);
				const int delta = logical ? shift.bottom : -shift.bottom;
				for (const int id : {IDC_USER_HINT, IDC_USER_STATUS_LABEL, IDC_USER_STATUS, IDOK, IDCANCEL})
				{
					HWND control = GetDlgItem(dialog, id); RECT bounds{}; GetWindowRect(control, &bounds);
					MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
					SetWindowPos(control, nullptr, bounds.left, bounds.top + delta, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				}
				RECT bounds{}; GetWindowRect(dialog, &bounds);
				SetWindowPos(dialog, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top + delta,
					SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
				state.logicalLayout = logical;
				InvalidateRect(dialog, nullptr, TRUE);
			}
			NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dialog, IDC_USER_EMAIL), OBJID_CLIENT, CHILDID_SELF);
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
					// Accept one extra character so overlong pasted passwords are rejected, rather than silently used truncated.
					for (const int id : {IDC_USER_PASSWORD, IDC_USER_CONFIRMATION})
						SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, kMaxLogicalPasswordLength + 1, 0);
					const auto federated = SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Federated identity (Bentley IMS)"));
					SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_SETITEMDATA, static_cast<WPARAM>(federated), static_cast<LPARAM>(AccountType::Federated));
					const auto logical = SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Logical (ProjectWise password)"));
					SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_SETITEMDATA, static_cast<WPARAM>(logical), static_cast<LPARAM>(AccountType::Logical));
					SendDlgItemMessageW(dialog, IDC_USER_TYPE, CB_SETCURSEL, static_cast<WPARAM>(federated), 0);
					RefreshType(dialog, *state);
					return TRUE;
				}
				auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
				if (message == WM_DESTROY) { ClearPasswords(dialog); return FALSE; }
				if (state != nullptr && state->creating && (message == WM_COMMAND || message == WM_CLOSE)) return TRUE;
				if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL))
				{ ClearPasswords(dialog); EndDialog(dialog, IDCANCEL); return TRUE; }
				if (message == WM_COMMAND && LOWORD(wParam) == IDC_USER_TYPE && HIWORD(wParam) == CBN_SELCHANGE && state != nullptr)
				{ RefreshType(dialog, *state); SetDlgItemTextW(dialog, IDC_USER_STATUS, L""); return TRUE; }
				if (message != WM_COMMAND || LOWORD(wParam) != IDOK) return FALSE;
				if (state == nullptr || state->create == nullptr) return TRUE;
				NewUser user;
				user.name = Value(dialog, IDC_USER_NAME); user.description = Value(dialog, IDC_USER_DESCRIPTION); user.email = Value(dialog, IDC_USER_EMAIL);
				user.type = SelectedType(dialog);
				ReadPassword(dialog, IDC_USER_PASSWORD, user.password); ReadPassword(dialog, IDC_USER_CONFIRMATION, user.confirmation);
				struct Clear { HWND dialog; ~Clear() { if (IsWindow(dialog)) ClearPasswords(dialog); } } clear{dialog};
				std::wstring error;
				int invalidControl = 0;
				if (ValidateNewUser(user, error, &invalidControl))
				{
					state->creating = true;
					struct Creating
					{
						DialogState& state; HWND dialog;
						~Creating()
						{
							state.creating = false;
							if (IsWindow(dialog)) { EnableWindow(GetDlgItem(dialog, IDOK), TRUE); EnableWindow(GetDlgItem(dialog, IDCANCEL), TRUE); }
						}
					} creating{*state, dialog};
					EnableWindow(GetDlgItem(dialog, IDOK), FALSE); EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
					if (state->create(state->context, state->shown, user, error))
					{ ClearPasswords(dialog); EndDialog(dialog, IDOK); return TRUE; }
				}
				SetDlgItemTextW(dialog, IDC_USER_STATUS, error.empty() ? L"ProjectWise did not create the account. No detailed error was returned." : error.c_str());
				NotifyWinEvent(EVENT_SYSTEM_ALERT, dialog, OBJID_CLIENT, CHILDID_SELF);
				SetFocus(GetDlgItem(dialog, invalidControl ? invalidControl : (user.type == AccountType::Logical ? IDC_USER_PASSWORD : IDC_USER_NAME)));
				return TRUE;
			}
			catch (...) { ClearPasswords(dialog); SetDlgItemTextW(dialog, IDC_USER_STATUS, L"The account could not be created."); return TRUE; }
		}
	}

	INT_PTR ShowNewUserDialog(HWND parent, HINSTANCE instance, const ContextState& shown, const wchar_t* datasource, void* context, CreateUserCallback create)
	{
		DialogState state{shown, datasource, context, create};
		return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_NEW_USER), parent, UserDialog, reinterpret_cast<LPARAM>(&state));
	}
}
