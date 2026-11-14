#include "Notifications.h"
#include "NotificationResources.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <utility>

namespace QuickAccess::Notifications
{
	bool SameDocument(const Document& a, const Document& b) noexcept
	{
		return a.project == b.project && a.id == b.id && a.sequence == b.sequence && !a.guid.empty() && a.guid == b.guid;
	}
	bool SameState(const Document& a, const Document& b) noexcept { return a.workflow == b.workflow && a.state == b.state; }
	bool Basket::Add(const Batch& batch)
	{
		if (batch.context.empty() || (!batch_.changes.empty() && batch_.context != batch.context)) return false;
		Batch combined = batch_.changes.empty() ? Batch{batch.context, batch.datasource, batch.initiator, {}} : batch_;
		if (batch_.changes.empty()) combined.linkApp = batch.linkApp;
		if (!batch.note.empty() && combined.note != batch.note) combined.note += (combined.note.empty() ? L"" : L"\r\n") + batch.note;
		for (const auto& change : batch.changes)
		{
			if (!SameDocument(change.before, change.after)) return false;
			const auto found = std::find_if(combined.changes.begin(), combined.changes.end(), [&](const auto& item) { return SameDocument(item.after, change.after); });
			if (found == combined.changes.end()) combined.changes.push_back(change);
			else *found = change; // one link per exact document/version, with its latest observed transition
			if (combined.changes.size() > kMaxItems) return false;
		}
		batch_ = std::move(combined); return true;
	}
	void Basket::Remove(std::size_t index)
	{
		if (index < batch_.changes.size()) batch_.changes.erase(batch_.changes.begin() + static_cast<std::ptrdiff_t>(index));
	}
	bool IsHttpsLink(const std::wstring& value) noexcept
	{
		return value.size() > 8 && _wcsnicmp(value.c_str(), L"https://", 8) == 0 && value[8] != L'/' && value[8] != L'?' && value[8] != L'#' &&
			std::none_of(value.begin(), value.end(), [](wchar_t c) { return iswspace(c) || iswcntrl(c); });
	}
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
			if (!change.observedAt.empty()) result += L"Observed: " + change.observedAt + L"\r\n";
			const auto link = WithLinkApp(change.after.link, batch.linkApp);
			result += !link.empty() ? link + L"\r\n" : L"[ProjectWise link unavailable for this destination]\r\n";
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
			Batch& batch; Composer composer; bool basket = false;
		};
		void Update(HWND window, Dialog& state)
		{
			const std::wstring summary = std::to_wstring(state.batch.changes.size()) + (state.basket ? L" item(s) in your notification basket. " : L" document(s) changed state. ") + state.batch.datasource;
			SetDlgItemTextW(window, IDC_NOTIFY_SUMMARY, summary.c_str());
			SendDlgItemMessageW(window, IDC_NOTIFY_ITEMS, LB_RESETCONTENT, 0, 0);
			for (const auto& change : state.batch.changes)
			{
				const auto row = change.after.name + L"  |  " + change.after.version + L"  |  " + change.after.stateName +
					(IsHttpsLink(change.after.link) ? L"" : L"  [HTTPS link unavailable]");
				SendDlgItemMessageW(window, IDC_NOTIFY_ITEMS, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
			}
			SetDlgItemTextW(window, IDC_NOTIFY_MESSAGE, Message(state.batch).c_str());
			for (int id : {IDC_NOTIFY_OPEN, IDC_NOTIFY_COPY, IDC_NOTIFY_PARK}) EnableWindow(GetDlgItem(window, id), !state.batch.changes.empty());
		}
		std::wstring ComposedText(HWND window, const Dialog& state)
		{
			const auto note = Value(window, IDC_NOTIFY_NOTE);
			return (note.empty() ? L"" : note + L"\r\n\r\n") + Message(state.batch);
		}
		void DeliveryHint(HWND window, const Dialog& state)
		{
			std::wstring uri, error, shortUri, shortError;
			const auto channel = IsDlgButtonChecked(window, IDC_NOTIFY_EMAIL) == BST_CHECKED ? Channel::Email : Channel::Teams;
			const auto to = Value(window, IDC_NOTIFY_RECIPIENTS);
			const bool longMessage = !ComposeLink(channel, to, L"ProjectWise workflow state change", ComposedText(window, state), uri, error) &&
				ComposeLink(channel, to, L"ProjectWise workflow state change", L"", shortUri, shortError);
			SetDlgItemTextW(window, IDC_NOTIFY_OPEN, channel == Channel::Email ? (longMessage ? L"Copy + open email" : L"Open email") : (longMessage ? L"Copy + open Teams" : L"Open Teams"));
			SetDlgItemTextW(window, IDC_NOTIFY_STATUS, longMessage ? L"Large message: the full text will be copied. Paste it into the draft that opens, then press Send." :
				state.basket ? L"Items stay in the basket after opening a draft. Clear them when finished." : L"Opens a draft. Review recipients and press Send in Teams or your email app.");
		}
		INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			try
			{
				if (message == WM_INITDIALOG)
				{
					auto& state = *reinterpret_cast<Dialog*>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam);
					SendDlgItemMessageW(window, IDC_NOTIFY_MESSAGE, EM_SETLIMITTEXT, 1000000, 0);
					Update(window, state);
					SetDlgItemTextW(window, IDC_NOTIFY_NOTE, state.batch.note.c_str());
					if (state.basket) { SetWindowTextW(window, L"Workflow notification basket"); SetDlgItemTextW(window, IDCANCEL, L"Close"); ShowWindow(GetDlgItem(window, IDC_NOTIFY_PARK), SW_HIDE); }
					SendDlgItemMessageW(window, IDC_NOTIFY_RECIPIENTS, EM_SETLIMITTEXT, 1024, 0);
					SendDlgItemMessageW(window, IDC_NOTIFY_NOTE, EM_SETLIMITTEXT, 4096, 0);
					CheckRadioButton(window, IDC_NOTIFY_TEAMS, IDC_NOTIFY_EMAIL, IDC_NOTIFY_TEAMS);
					for (const wchar_t* label : {L"Choose application", L"ProjectWise Explorer", L"ProjectWise Web", L"ProjectWise Web View"})
						SendDlgItemMessageW(window, IDC_NOTIFY_LINK_APP, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
					SendDlgItemMessageW(window, IDC_NOTIFY_LINK_APP, CB_SETCURSEL, static_cast<WPARAM>(state.batch.linkApp), 0);
					SetTimer(window, 1, 500, nullptr); SetFocus(GetDlgItem(window, IDCANCEL)); return FALSE;
				}
				auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
				if (!state) return FALSE;
				if (message == WM_TIMER && state->composer.valid && !state->composer.valid(state->composer.context, state->batch.context))
				{ EndDialog(window, IDCANCEL); return TRUE; }
				if (message != WM_COMMAND) return FALSE;
				const int command = LOWORD(wParam);
				if (command == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
				if (state->composer.valid && !state->composer.valid(state->composer.context, state->batch.context)) { EndDialog(window, IDCANCEL); return TRUE; }
				if ((command == IDC_NOTIFY_NOTE || command == IDC_NOTIFY_RECIPIENTS) && HIWORD(wParam) == EN_CHANGE)
				{
					if (command == IDC_NOTIFY_NOTE) state->batch.note = Value(window, IDC_NOTIFY_NOTE);
					DeliveryHint(window, *state); return TRUE;
				}
				if (command == IDC_NOTIFY_LINK_APP && HIWORD(wParam) == CBN_SELCHANGE)
				{
					const auto selected = SendDlgItemMessageW(window, IDC_NOTIFY_LINK_APP, CB_GETCURSEL, 0, 0);
					if (selected >= 0 && selected <= 3) state->batch.linkApp = static_cast<LinkApp>(selected);
					SetDlgItemTextW(window, IDC_NOTIFY_MESSAGE, Message(state->batch).c_str()); DeliveryHint(window, *state); return TRUE;
				}
				if (command == IDC_NOTIFY_REMOVE || command == IDC_NOTIFY_CLEAR)
				{
					for (std::size_t i = state->batch.changes.size(); i > 0; --i)
						if (command == IDC_NOTIFY_CLEAR || SendDlgItemMessageW(window, IDC_NOTIFY_ITEMS, LB_GETSEL, i - 1, 0) > 0)
							state->batch.changes.erase(state->batch.changes.begin() + static_cast<std::ptrdiff_t>(i - 1));
					Update(window, *state); DeliveryHint(window, *state); return TRUE;
				}
				if (command == IDC_NOTIFY_PARK && !state->batch.changes.empty())
					{ EndDialog(window, IDC_NOTIFY_PARK); return TRUE; }
				if (command == IDC_NOTIFY_TEAMS || command == IDC_NOTIFY_EMAIL)
				{
					SetDlgItemTextW(window, IDC_NOTIFY_TO_LABEL, command == IDC_NOTIFY_TEAMS ? L"Recipients (Teams sign-in addresses / UPNs; separate with semicolons)" : L"Recipients (email addresses; separate with semicolons)");
					DeliveryHint(window, *state); return TRUE;
				}
				if (command != IDC_NOTIFY_OPEN && command != IDC_NOTIFY_COPY) return FALSE;
				if (state->batch.changes.empty()) return TRUE;
				if (state->composer.valid && !state->composer.valid(state->composer.context, state->batch.context)) { EndDialog(window, IDCANCEL); return TRUE; }
				std::wstring error;
				const auto text = ComposedText(window, *state);
				if (command == IDC_NOTIFY_COPY)
				{
					if (state->composer.copy && state->composer.copy(state->composer.context, text, error)) error = L"Message copied. Paste it into Teams or email if the compose link is too long.";
				}
				else
				{
					if (std::any_of(state->batch.changes.begin(), state->batch.changes.end(), [&](const auto& change) { return WithLinkApp(change.after.link, state->batch.linkApp).empty(); }))
					{ SetDlgItemTextW(window, IDC_NOTIFY_STATUS, L"A link cannot open in the selected destination. Web requires its connected work area/project. Choose another destination or remove that item."); return TRUE; }
					std::wstring uri;
					const Channel channel = IsDlgButtonChecked(window, IDC_NOTIFY_EMAIL) == BST_CHECKED ? Channel::Email : Channel::Teams;
					bool prepared = ComposeLink(channel, Value(window, IDC_NOTIFY_RECIPIENTS), L"ProjectWise workflow state change", text, uri, error);
					if (!prepared)
					{
						std::wstring shortUri, shortError;
						if (ComposeLink(channel, Value(window, IDC_NOTIFY_RECIPIENTS), L"ProjectWise workflow state change", L"", shortUri, shortError) &&
							state->composer.copy && state->composer.copy(state->composer.context, text, error)) { uri = std::move(shortUri); prepared = true; }
					}
					if (prepared && state->composer.open && state->composer.open(state->composer.context, uri, error)) { EndDialog(window, IDOK); return TRUE; }
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
		for (const auto* value : {&recipients, &subject, &message})
			if (!value->empty() && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value->data(), static_cast<int>(value->size()), nullptr, 0, nullptr, nullptr) == 0)
			{ error = L"The recipient or message contains invalid Unicode text. Correct it before opening the app."; return false; }
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
	namespace
	{
		using Fields = std::map<std::wstring, std::wstring>;
		int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
		std::wstring Decode(const std::wstring& value)
		{
			const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (count <= 0) return {};
			std::string encoded(static_cast<std::size_t>(count), '\0'), bytes;
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), encoded.data(), count, nullptr, nullptr);
			for (std::size_t i = 0; i < encoded.size(); ++i)
				if (encoded[i] == '%')
				{
					if (i + 2 >= encoded.size() || Hex(encoded[i + 1]) < 0 || Hex(encoded[i + 2]) < 0) return {};
					bytes += static_cast<char>(Hex(encoded[i + 1]) * 16 + Hex(encoded[i + 2])); i += 2;
				}
				else bytes += encoded[i];
			const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
			if (size <= 0) return {};
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), result.data(), size); return result;
		}
		Fields Query(const std::wstring& link, const wchar_t* prefix)
		{
			Fields result; const std::wstring base(prefix);
			if (link.size() <= base.size() || _wcsnicmp(link.c_str(), base.c_str(), base.size()) != 0) return result;
			std::size_t start = base.size();
			while (start < link.size())
			{
				const auto end = link.find(L'&', start), equal = link.find(L'=', start);
				const auto stop = end == std::wstring::npos ? link.size() : end;
				if (equal == std::wstring::npos || equal >= stop) return {};
				const auto name = Decode(link.substr(start, equal - start)), value = Decode(link.substr(equal + 1, stop - equal - 1));
				if (name.empty() || std::any_of(value.begin(), value.end(), [](wchar_t c) { return iswcntrl(c) != 0; }) || !result.emplace(name, value).second) return {};
				start = stop + 1;
			}
			return result;
		}
		std::wstring Guid(const std::wstring& raw)
		{
			std::wstring result;
			for (wchar_t c : raw)
				if (c == L'-' || c == L'{' || c == L'}') continue;
				else if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')) result += static_cast<wchar_t>(towlower(c));
				else return {};
			return result.size() == 32 ? result : std::wstring{};
		}
		std::wstring Build(Fields fields)
		{
			if (!fields.contains(L"ds") || fields[L"ds"].empty() || !fields.contains(L"doc") || Guid(fields[L"doc"]).empty()) return {};
			fields[L"doc"] = Guid(fields[L"doc"]);
			std::wstring result = L"https://pwlink.bentley.com/link?";
			const auto append = [&](const std::wstring& name, const std::wstring& value) {
				if (result.back() != L'?') result += L'&'; result += Encode(name) + L"=" + Encode(value);
			};
			for (const wchar_t* key : {L"ds", L"doc", L"wa", L"p", L"s", L"app"})
				if (const auto found = fields.find(key); found != fields.end()) { append(found->first, found->second); fields.erase(found); }
			for (const auto& [name, value] : fields) append(name, value);
			return result;
		}
	}
	std::wstring ModernDocumentLink(const std::wstring& nativeLink, const std::wstring& datasource, const std::wstring& documentGuid)
	{
		const auto guid = Guid(documentGuid); if (guid.empty() || datasource.empty()) return {};
		auto fields = Query(nativeLink, L"https://pwlink.bentley.com/link?");
		if (fields.empty())
		{
			const auto legacy = Query(nativeLink, L"https://connect-projectwisewac.bentley.com/pwlink?");
			for (const auto& [oldKey, key] : {std::pair{L"datasource", L"ds"}, {L"workAreaId", L"wa"}, {L"projectId", L"p"}, {L"s", L"s"}})
				if (const auto found = legacy.find(oldKey); found != legacy.end()) fields[key] = found->second;
		}
		if (!fields.contains(L"ds") || fields[L"ds"].empty())
		{
			std::wstring value; for (wchar_t c : datasource) value += c == L':' ? L"~3A" : std::wstring(1, c);
			fields[L"ds"] = std::move(value);
		}
		if (fields[L"ds"].starts_with(L"Bentley.PW--")) fields[L"ds"].erase(0, 12);
		fields[L"doc"] = guid; fields.erase(L"fld"); fields.erase(L"app");
		for (const wchar_t* key : {L"wa", L"p"}) if (fields.contains(key))
		{ const auto normalized = Guid(fields[key]); if (normalized.empty()) fields.erase(key); else fields[key] = normalized; }
		return Build(std::move(fields));
	}
	std::wstring WithLinkApp(const std::wstring& link, LinkApp app)
	{
		auto fields = Query(link, L"https://pwlink.bentley.com/link?");
		if (fields.empty() || fields.contains(L"fld")) return {};
		fields.erase(L"app");
		if (app == LinkApp::Web && (!fields.contains(L"wa") || !fields.contains(L"p"))) return {};
		if (app != LinkApp::Choose) fields[L"app"] = app == LinkApp::Explorer ? L"pwe" : app == LinkApp::Web ? L"web" : L"webview";
		return Build(std::move(fields));
	}
	INT_PTR ShowDialog(HWND owner, HINSTANCE instance, Batch& batch, Composer composer, bool basket)
	{
		if (!IsWindow(owner) || !IsWindowEnabled(owner) || batch.changes.empty()) return IDCANCEL;
		Dialog state{batch, composer, basket}; return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_WORKFLOW_NOTIFY), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
	}
}
