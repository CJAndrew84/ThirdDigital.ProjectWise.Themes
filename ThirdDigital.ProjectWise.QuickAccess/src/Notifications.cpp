#include "Notifications.h"
#include "NotificationResources.h"
#include <algorithm>
#include <cwctype>
#include <utility>

namespace QuickAccess::Notifications
{
	bool SameDocument(const Document& a, const Document& b) noexcept
	{
		return a.project == b.project && a.id == b.id && a.sequence == b.sequence && !a.guid.empty() && a.guid == b.guid;
	}
	bool SameState(const Document& a, const Document& b) noexcept { return a.workflow == b.workflow && a.state == b.state; }
	std::size_t Watcher::Pending() const noexcept
	{
		std::size_t count = 0; for (const auto& batch : pending_) count += batch.documents.size(); return count;
	}
	std::uint64_t Watcher::Begin(const std::wstring& context, const std::wstring& datasource, const std::wstring& initiator,
		const std::vector<Document>& before, Time now)
	{
		if (context.empty() || before.empty() || before.size() > kMaxDocuments) return 0;
		for (std::size_t i = 0; i < before.size(); ++i)
		{
			if (before[i].project <= 0 || before[i].id <= 0 || before[i].guid.empty() || before[i].workflow <= 0) return 0;
			for (std::size_t j = 0; j < i; ++j) if (SameDocument(before[i], before[j])) return 0;
		}
		std::erase_if(pending_, [&](const auto& b) { return b.info.context != context; });
		std::size_t overlapping = 0;
		for (const auto& batch : pending_) for (const auto& candidate : batch.documents)
			if (std::any_of(before.begin(), before.end(), [&](const auto& d) { return SameDocument(candidate.before, d); })) ++overlapping;
		if (Pending() - overlapping + before.size() > kMaxPending) return 0;
		PendingBatch batch;
		batch.token = ++serial_; batch.info = {context, datasource, initiator, {}};
		batch.expires = now + 120000;
		for (auto document : before)
		{
			for (auto& previous : pending_)
			{
				const auto found = std::find_if(previous.documents.begin(), previous.documents.end(), [&](const auto& c) { return SameDocument(c.before, document); });
				if (found != previous.documents.end()) document = found->before; // coalesce quick follow-up changes/undo before any prompt
				std::erase_if(previous.documents, [&](const auto& c) { return SameDocument(c.before, document); });
			}
			Candidate candidate; candidate.before = std::move(document); batch.documents.push_back(std::move(candidate));
		}
		std::erase_if(pending_, [](const auto& b) { return b.documents.empty(); });
		const auto token = batch.token; pending_.push_back(std::move(batch)); return token;
	}
	void Watcher::Complete(std::uint64_t token, Time now)
	{
		for (auto& batch : pending_) if (batch.token == token)
		{
			batch.complete = true; batch.expires = now + 120000;
			for (auto& candidate : batch.documents) candidate.next = now + 1500;
		}
	}
	void Watcher::Rebase(std::uint64_t token, const std::vector<Document>& before)
	{
		for (auto& batch : pending_) if (batch.token == token && !batch.complete)
			for (auto& candidate : batch.documents) if (!candidate.rebased)
				for (const auto& document : before) if (SameDocument(candidate.before, document))
				{ candidate.before = document; candidate.rebased = true; break; }
	}
	std::vector<Probe> Watcher::Due(const std::wstring& context, Time now, std::size_t budget)
	{
		std::erase_if(pending_, [&](const auto& batch) { return batch.info.context != context; });
		std::vector<std::pair<PendingBatch*, Candidate*>> due;
		for (auto& batch : pending_) if (batch.complete && now <= batch.expires)
			for (auto& candidate : batch.documents) if (candidate.next <= now) due.push_back({&batch, &candidate});
		std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.second->next < b.second->next; });
		std::vector<Probe> result;
		for (auto [batch, candidate] : due)
		{
			if (result.size() >= std::min<std::size_t>(budget, 8)) break;
			result.push_back({batch->token, candidate->before}); candidate->next = now + 15000;
		}
		return result;
	}
	void Watcher::Observe(std::uint64_t token, const Document& before, const Document* after, Time now)
	{
		for (auto& batch : pending_) if (batch.token == token)
		{
			const auto found = std::find_if(batch.documents.begin(), batch.documents.end(), [&](const auto& c) { return SameDocument(c.before, before); });
			if (found == batch.documents.end()) return;
			auto& candidate = *found;
			if (!after)
			{
				// Never offer cached document details after access/read failure. A later successful read must settle again.
				candidate.changed = candidate.confirmed = false; candidate.stableSince = 0;
				if (++candidate.failures >= 3) batch.documents.erase(found); else candidate.next = now + 5000;
				return;
			}
			if (!SameDocument(before, *after)) { batch.documents.erase(found); return; }
			candidate.failures = 0;
			if (SameState(before, *after))
			{
				candidate.changed = candidate.confirmed = false; candidate.stableSince = 0;
				candidate.delay = std::min(15000U, candidate.delay * 2); candidate.next = now + candidate.delay;
				return;
			}
			if (!candidate.changed || !SameState(candidate.last, *after))
			{
				candidate.changed = true; candidate.confirmed = false; candidate.stableSince = now; batch.lastChange = now;
			}
			else if (now >= candidate.stableSince + 2000) candidate.confirmed = true;
			candidate.last = *after; candidate.next = now + 2000;
			return;
		}
	}
	std::vector<Batch> Watcher::TakeReady(const std::wstring& context, Time now)
	{
		std::vector<Batch> result;
		for (auto& batch : pending_)
		{
			if (batch.info.context != context || !batch.complete || now < batch.lastChange + 6000) continue;
			// A changing WRE intermediate state cannot be reported as the stable outcome.
			if (std::any_of(batch.documents.begin(), batch.documents.end(), [](const auto& c) { return c.changed && !c.confirmed; })) continue;
			Batch ready = batch.info;
			for (const auto& candidate : batch.documents) if (candidate.confirmed) ready.changes.push_back({candidate.before, candidate.last});
			if (!ready.changes.empty())
			{
				result.push_back(std::move(ready));
				std::erase_if(batch.documents, [](const auto& c) { return c.confirmed; });
			}
		}
		std::erase_if(pending_, [&](const auto& b) { return b.info.context != context || b.documents.empty() || (b.complete && now > b.expires); });
		return result;
	}
	std::wstring Message(const Batch& batch)
	{
		std::wstring result = L"ProjectWise workflow state change\r\nDatasource: " + batch.datasource;
		if (!batch.initiator.empty()) result += L"\r\nFollowing a workflow action initiated by: " + batch.initiator;
		result += L"\r\n\r\n";
		for (const auto& change : batch.changes)
		{
			const auto state = [](const Document& document) { return document.stateName.empty() ? std::to_wstring(document.state) : document.stateName; };
			result += change.after.name + (change.after.version.empty() ? L"" : L" (version " + change.after.version + L")") + L"\r\n";
			result += state(change.before) + L" -> " + state(change.after) + L"\r\n";
			if (change.before.workflow != change.after.workflow)
				result += L"Workflow: " + change.before.workflowName + L" -> " + change.after.workflowName + L"\r\n";
			if (!change.after.link.empty()) result += change.after.link + L"\r\n";
			result += L"\r\n";
		}
		return result + L"This describes the state change observed in ProjectWise; later workflow processing may change the state again.";
	}
	namespace
	{
		std::wstring Encode(const std::wstring& input)
		{
			if (input.empty()) return {};
			const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
			if (count <= 0) return {};
			std::string bytes(static_cast<std::size_t>(count), '\0');
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), bytes.data(), count, nullptr, nullptr);
			constexpr wchar_t hex[] = L"0123456789ABCDEF";
			std::wstring result;
			for (unsigned char c : bytes)
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') result += c;
				else { result += L'%'; result += hex[c >> 4]; result += hex[c & 15]; }
			return result;
		}
		std::wstring Value(HWND dialog, int id)
		{
			std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(GetDlgItem(dialog, id))) + 1, L'\0');
			value.resize(static_cast<std::size_t>(GetDlgItemTextW(dialog, id, value.data(), static_cast<int>(value.size())))); return value;
		}
		struct Dialog
		{
			const Batch& batch; Composer composer;
		};
		INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			try
			{
				if (message == WM_INITDIALOG)
				{
					auto& state = *reinterpret_cast<Dialog*>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam);
					const std::wstring summary = std::to_wstring(state.batch.changes.size()) + L" document(s) changed workflow state in " + state.batch.datasource + L". Prepare a notification?";
					SetDlgItemTextW(window, IDC_NOTIFY_SUMMARY, summary.c_str());
					SetDlgItemTextW(window, IDC_NOTIFY_MESSAGE, Message(state.batch).c_str());
					SendDlgItemMessageW(window, IDC_NOTIFY_RECIPIENTS, EM_SETLIMITTEXT, 1024, 0);
					SendDlgItemMessageW(window, IDC_NOTIFY_MESSAGE, EM_SETLIMITTEXT, 32000, 0);
					CheckRadioButton(window, IDC_NOTIFY_TEAMS, IDC_NOTIFY_EMAIL, IDC_NOTIFY_TEAMS);
					SetTimer(window, 1, 500, nullptr); SetFocus(GetDlgItem(window, IDCANCEL)); return FALSE;
				}
				auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
				if (!state) return FALSE;
				if (message == WM_TIMER && state->composer.valid && !state->composer.valid(state->composer.context, state->batch.context))
				{ EndDialog(window, IDCANCEL); return TRUE; }
				if (message != WM_COMMAND) return FALSE;
				const int command = LOWORD(wParam);
				if (command == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
				if (command == IDC_NOTIFY_TEAMS || command == IDC_NOTIFY_EMAIL)
				{
					SetDlgItemTextW(window, IDC_NOTIFY_TO_LABEL, command == IDC_NOTIFY_TEAMS ? L"Recipients (Teams sign-in addresses / UPNs; separate with semicolons)" : L"Recipients (email addresses; separate with semicolons)");
					SetDlgItemTextW(window, IDC_NOTIFY_OPEN, command == IDC_NOTIFY_TEAMS ? L"Open Teams" : L"Open email"); return TRUE;
				}
				if (command != IDC_NOTIFY_OPEN && command != IDC_NOTIFY_COPY) return FALSE;
				if (state->composer.valid && !state->composer.valid(state->composer.context, state->batch.context)) { EndDialog(window, IDCANCEL); return TRUE; }
				std::wstring error;
				const auto text = Value(window, IDC_NOTIFY_MESSAGE);
				if (command == IDC_NOTIFY_COPY)
				{
					if (state->composer.copy && state->composer.copy(state->composer.context, text, error)) error = L"Message copied. Paste it into Teams or email if the compose link is too long.";
				}
				else
				{
					std::wstring uri;
					const Channel channel = IsDlgButtonChecked(window, IDC_NOTIFY_EMAIL) == BST_CHECKED ? Channel::Email : Channel::Teams;
					if (ComposeLink(channel, Value(window, IDC_NOTIFY_RECIPIENTS), L"ProjectWise workflow state change", text, uri, error) && state->composer.open &&
						state->composer.open(state->composer.context, uri, error)) { EndDialog(window, IDOK); return TRUE; }
				}
				SetDlgItemTextW(window, IDC_NOTIFY_STATUS, error.empty() ? L"The message could not be prepared. Try again or copy it." : error.c_str());
				NotifyWinEvent(EVENT_SYSTEM_ALERT, window, OBJID_CLIENT, CHILDID_SELF); return TRUE;
			}
			catch (...) { SetDlgItemTextW(window, IDC_NOTIFY_STATUS, L"The notification could not be prepared. You can close this dialog and continue working."); return TRUE; }
		}
	}
	bool ComposeLink(Channel channel, const std::wstring& recipients, const std::wstring& subject, const std::wstring& message, std::wstring& uri, std::wstring& error)
	{
		uri.clear(); error.clear();
		if (recipients.empty() || recipients.size() > 1024 || subject.size() > 200 || message.size() > 32000)
		{ error = L"Enter recipients and keep the message within the supported size."; return false; }
		std::vector<std::wstring> addresses;
		std::size_t start = 0;
		for (std::size_t i = 0; i <= recipients.size(); ++i) if (i == recipients.size() || recipients[i] == L';' || recipients[i] == L',')
		{
			std::wstring address = recipients.substr(start, i - start); start = i + 1;
			while (!address.empty() && address.front() == L' ') address.erase(address.begin());
			while (!address.empty() && address.back() == L' ') address.pop_back();
			const auto at = address.find(L'@');
			if (address.empty() || address.size() > 320 || at == 0 || at == std::wstring::npos || at + 1 == address.size() || address.find(L'@', at + 1) != std::wstring::npos ||
				std::any_of(address.begin(), address.end(), [](wchar_t c) { return iswspace(c) || iswcntrl(c) || wcschr(L"<>\"?&#=:/\\", c); }))
			{ error = L"Use email addresses / Teams sign-in addresses, separated by semicolons. Display names and control characters are not supported."; return false; }
			if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(std::move(address));
			if (addresses.size() > 20) { error = L"Use no more than 20 recipients in a compose link."; return false; }
		}
		std::wstring joined;
		for (const auto& address : addresses)
		{
			if (!joined.empty()) joined += L',';
			auto encoded = Encode(address);
			if (channel == Channel::Email)
				if (const auto at = encoded.find(L"%40"); at != std::wstring::npos) encoded.replace(at, 3, L"@");
			joined += encoded;
		}
		uri = channel == Channel::Teams ? L"https://teams.microsoft.com/l/chat/0/0?users=" + joined + L"&message=" + Encode(message) :
			L"mailto:" + joined + L"?subject=" + Encode(subject) + L"&body=" + Encode(message);
		if (uri.size() > 1800) { uri.clear(); error = L"The message is too long for a compose link. Copy the message, shorten or clear this text, open the app and paste it there."; return false; }
		return true;
	}
	INT_PTR ShowDialog(HWND owner, HINSTANCE instance, const Batch& batch, Composer composer)
	{
		if (!IsWindow(owner) || !IsWindowEnabled(owner) || batch.changes.empty()) return IDCANCEL;
		Dialog state{batch, composer}; return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_WORKFLOW_NOTIFY), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
	}
}
