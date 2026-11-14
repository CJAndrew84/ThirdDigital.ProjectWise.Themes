#include "..\src\Notifications.h"
#include "..\src\NotificationResources.h"
#include "..\src\NotificationStore.h"
#include <algorithm>
#include <commctrl.h>

namespace
{
	namespace N = QuickAccess::Notifications;
	void (*g_expect)(bool, const wchar_t*) = nullptr;
	bool (*g_save)(HWND, const wchar_t*) = nullptr;
	void Expect(bool value, const wchar_t* text) { g_expect(value, text); }
	N::Document Document(LONG id = 7, LONG state = 100)
	{
		N::Document value; value.project = 1; value.id = id; value.sequence = 2; value.workflow = 10; value.state = state;
		wchar_t suffix[16]{}; swprintf_s(suffix, L"%012lx", static_cast<unsigned long>(id));
		value.guid = L"00000000-0000-0000-0000-" + std::wstring(suffix); value.name = L"Design model.dgn"; value.version = L"P01.02";
		value.stateName = state == 100 ? L"Work in progress" : state == 200 ? L"Shared" : L"Published";
		value.workflowName = L"Design delivery";
		value.link = L"https://pwlink.bentley.com/link?ds=example.test~3ATest&doc=00000000000000000000" + std::wstring(suffix); return value;
	}
	void Models()
	{
		N::Watcher watcher; const auto before = Document(), after = Document(7, 200);
		const auto token = watcher.Begin(L"datasource/user", L"Test datasource", L"Test user", {before}, 0);
		Expect(token != 0 && watcher.Due(L"datasource/user", 5000).empty(), L"workflow observers do not read or prompt while the originating command is active");
		watcher.Complete(token, 100);
		Expect(watcher.Due(L"datasource/user", 1000).empty(), L"verification is deferred until after native workflow dialogs return");
		Expect(watcher.Due(L"datasource/user", 2000).size() == 1, L"only the affected document is scheduled for server verification");
		watcher.Observe(token, before, &before, 2000);
		Expect(watcher.TakeReady(L"datasource/user", 4000).empty(), L"an unchanged document cannot produce a notification from a button click alone");
		watcher.Observe(token, before, &after, 5000);
		Expect(watcher.TakeReady(L"datasource/user", 12000).empty(), L"a single changed read is insufficient to confirm asynchronous WRE completion");
		watcher.Observe(token, before, &after, 7000);
		Expect(watcher.TakeReady(L"datasource/user", 10000).empty(), L"confirmed changes wait for a quiet batching interval");
		const auto ready = watcher.TakeReady(L"datasource/user", 11000);
		Expect(ready.size() == 1 && ready[0].changes.size() == 1 && ready[0].changes[0].after.state == 200,
			L"a delayed confirmed server-side state change produces one notification batch");
		Expect(watcher.TakeReady(L"datasource/user", 18000).empty() && watcher.Pending() == 0, L"repeated state refreshes do not repeat the notification");
		const auto message = N::Message(ready[0]);
		Expect(message.find(L"Work in progress -> Shared") != std::wstring::npos && message.find(L"https://pwlink.bentley.com/link?ds=") != std::wstring::npos &&
			message.find(L"P01.02") != std::wstring::npos, L"notification text includes observed transition, version label and ProjectWise HTTPS link");
		Expect(!N::IsHttpsLink(L"pw://example/document") && !N::IsHttpsLink(L"http://example/document") && !N::IsHttpsLink(L"https://example/link\r\nextra"),
			L"notification links require HTTPS without legacy protocol or control characters");
		const auto modern = N::ModernDocumentLink(L"https://connect-projectwisewac.bentley.com/pwlink?datasource=Bentley.PW--example.test~3ATest&objectId=00000000-0000-0000-0000-000000000007&objectType=doc&workAreaId=11111111-1111-1111-1111-111111111111&projectId=22222222-2222-2222-2222-222222222222&s=YWJjL2RlZg%3D%3D&app=pwe", L"example.test:Test", before.guid);
		Expect(modern.starts_with(L"https://pwlink.bentley.com/link?ds=example.test~3ATest&doc=") && modern.find(L"&wa=11111111111111111111111111111111") != std::wstring::npos &&
			modern.find(L"&p=22222222222222222222222222222222") != std::wstring::npos && modern.find(L"&s=YWJjL2RlZg%3D%3D") != std::wstring::npos,
			L"new pwlink document URLs preserve native datasource, work-area, project and encoded WSG routing");
		Expect(N::WithLinkApp(modern, N::LinkApp::Explorer).find(L"&app=pwe") != std::wstring::npos &&
			N::WithLinkApp(modern, N::LinkApp::Web).find(L"&app=web") != std::wstring::npos &&
			N::WithLinkApp(modern, N::LinkApp::WebView).find(L"&app=webview") != std::wstring::npos && N::WithLinkApp(modern, N::LinkApp::Choose).find(L"app=") == std::wstring::npos,
			L"link destination is independently selectable from Teams/email delivery");
		const auto bare = N::ModernDocumentLink(L"", L"example.test:Test", before.guid);
		Expect(N::WithLinkApp(bare, N::LinkApp::Web).empty() && !N::WithLinkApp(bare, N::LinkApp::Explorer).empty(),
			L"unassociated documents retain Explorer links without inventing connected Web project/work-area IDs");
		Expect(N::WithLinkApp(L"https://pwlink.bentley.com/link?ds=a&fld=11111111111111111111111111111111", N::LinkApp::Explorer).empty(),
			L"a folder URL cannot accidentally replace the approved document link");
		watcher.Clear(); auto changing = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(changing, 0);
		watcher.Observe(changing, before, &after, 2000); const auto final = Document(7, 300);
		watcher.Observe(changing, before, &final, 4000); watcher.Observe(changing, before, &final, 6000);
		const auto settled = watcher.TakeReady(L"a", 10000);
		Expect(settled.size() == 1 && settled[0].changes[0].after.state == 300, L"intermediate WRE states settle into the latest confirmed state before prompting");
		watcher.Clear(); auto cancelled = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(cancelled, 0);
		watcher.Observe(cancelled, before, &before, 2000);
		Expect(watcher.TakeReady(L"a", 120001).empty() && watcher.Pending() == 0, L"cancelled/failed actions with no committed state change expire without a prompt");
		watcher.Clear(); auto stale = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(stale, 0);
		watcher.Observe(stale, before, &after, 2000); watcher.Observe(stale, before, &after, 4000);
		watcher.Observe(stale, before, nullptr, 5000); watcher.Observe(stale, before, nullptr, 10000); watcher.Observe(stale, before, nullptr, 15000);
		Expect(watcher.TakeReady(L"a", 20000).empty(), L"lost access/read failure removes confirmed cached details from notifications");
		watcher.Clear(); auto version = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(version, 0);
		auto different = after; different.sequence = 3; watcher.Observe(version, before, &different, 2000);
		Expect(watcher.Pending() == 0, L"a new document version cannot be mistaken for a state change of the captured version");
		watcher.Clear(); auto scope = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(scope, 0);
		Expect(watcher.Due(L"b", 2000).empty() && watcher.Pending() == 0, L"changing datasource/user clears pending observations");
		watcher.Clear(); auto undo = watcher.Begin(L"a", L"ds", L"user", {before}, 0); watcher.Complete(undo, 0);
		watcher.Observe(undo, before, &after, 2000);
		undo = watcher.Begin(L"a", L"ds", L"user", {after}, 3000); watcher.Complete(undo, 3000);
		watcher.Observe(undo, before, &before, 5000);
		Expect(watcher.TakeReady(L"a", 20000).empty(), L"a quick undo before prompting coalesces to no net state change");
		watcher.Clear(); auto rebased = watcher.Begin(L"a", L"ds", L"user", {before}, 0);
		watcher.Rebase(rebased, {after}); watcher.Complete(rebased, 100);
		watcher.Observe(rebased, after, &final, 2000); watcher.Observe(rebased, after, &final, 4000);
		const auto exact = watcher.TakeReady(L"a", 8000);
		Expect(exact.size() == 1 && exact[0].changes[0].before.state == 200, L"the SDK pre-state hook rebases a dialog-time snapshot immediately before the actual operation");
		watcher.Clear(); std::vector<N::Document> documents;
		for (LONG i = 1; i <= 64; ++i) documents.push_back(Document(i));
		const auto large = watcher.Begin(L"a", L"ds", L"user", documents, 0); watcher.Complete(large, 0);
		Expect(watcher.Due(L"a", 2000, 100).size() == 8, L"server polling is capped at eight affected documents per timer tick");
		documents.push_back(Document(65));
		Expect(watcher.Begin(L"a", L"ds", L"user", documents, 0) == 0, L"oversized actions are not silently truncated into partial notification batches");
		watcher.Clear();
		Expect(watcher.Begin(L"a", L"ds", L"user", {before, before}, 0) == 0, L"ambiguous duplicate captured versions are rejected");
		std::wstring uri, error;
		Expect(N::ComposeLink(N::Channel::Teams, L"one@example.test; two@example.test", L"Workflow", L"Shared & reviewed \x00e9", uri, error) &&
			uri.starts_with(L"https://teams.microsoft.com/l/chat/0/0?users=") && uri.find(L"%26") != std::wstring::npos && uri.find(L"%C3%A9") != std::wstring::npos,
			L"Teams drafts encode recipients, message punctuation and UTF-8 text without sending");
		Expect(N::ComposeLink(N::Channel::Email, L"one@example.test", L"Review & approve", L"Line 1\r\nLine 2", uri, error) && uri.starts_with(L"mailto:") && uri.find(L"%0D%0A") != std::wstring::npos,
			L"email drafts encode subject/body fields and preserve message line breaks");
		Expect(!N::ComposeLink(N::Channel::Email, L"one@example.test\r\nBcc:other@example.test", L"Review", L"Body", uri, error) && uri.empty(), L"recipient header injection is rejected");
		Expect(!N::ComposeLink(N::Channel::Teams, L"Display Name <one@example.test>", L"Review", L"Body", uri, error), L"unresolved display names are not treated as verified Teams recipients");
		Expect(!N::ComposeLink(N::Channel::Teams, L"one@example.test", L"Review", std::wstring(2000, L'x'), uri, error) && error.find(L"Copy") != std::wstring::npos,
			L"oversized compose links retain an actionable copy/paste fallback rather than truncating the message");
		N::Basket basket;
		N::Batch first{L"a", L"ds", L"user", {{before, after}}}; first.note = L"For coordination";
		Expect(basket.Add(first) && basket.Add(first) && basket.Items().changes.size() == 1, L"parking the same document/version twice adds one basket link");
		N::Batch later{L"a", L"ds", L"user", {{after, final}}};
		Expect(basket.Add(later) && basket.Items().changes.size() == 1 && basket.Items().changes[0].after.state == 300,
			L"successive approvals of one document/version retain its latest observed transition");
		N::Batch other{L"b", L"other ds", L"other user", {{before, after}}};
		Expect(!basket.Add(other) && basket.Items().changes.size() == 1, L"parked baskets cannot mix datasource/user session contexts");
		auto nextVersion = before; nextVersion.sequence = 3;
		auto approvedVersion = nextVersion; approvedVersion.state = 200;
		Expect(basket.Add({L"a", L"ds", L"user", {{nextVersion, approvedVersion}}}) && basket.Items().changes.size() == 2,
			L"different versions remain distinct basket entries even for the same document ID");
		basket.Remove(0);
		Expect(basket.Items().changes.size() == 1 && basket.Items().changes[0].after.sequence == 3, L"individual parked items can be removed without losing the rest");
		const auto owner = L"native-check-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
		N::Batch loaded;
		Expect(N::SaveBasket(owner, basket.Items()) && N::LoadBasket(owner, L"new authenticated session", loaded) && loaded.changes.size() == 1 &&
			loaded.changes[0].after.link == approvedVersion.link && loaded.note == L"For coordination" && loaded.context == L"new authenticated session",
			L"Windows-protected basket storage restores HTTPS links and notes into a new authenticated session");
		Expect(N::LoadBasket(owner + L"-another-user", L"other context", loaded) && loaded.changes.empty(), L"another datasource/account does not load someone else's parked basket");
		Expect(N::EraseBasket(owner) && N::LoadBasket(owner, L"a", loaded) && loaded.changes.empty(), L"clearing a basket removes only its own saved record");
	}
	struct Fixture
	{
		HWND owner = nullptr;
		const wchar_t* preview = nullptr;
		unsigned calls = 0, copies = 0, ticks = 0;
		bool succeeds = false, valid = true, cancel = false, changeContext = false, park = false, remove = false;
		std::wstring uri, copied;
		static bool Open(void* context, const std::wstring& uri, std::wstring& error)
		{ auto& self = *static_cast<Fixture*>(context); ++self.calls; self.uri = uri; error = L"Fixture: no mail handler"; return self.succeeds; }
		static bool Copy(void* context, const std::wstring& text, std::wstring&)
		{ auto& self = *static_cast<Fixture*>(context); ++self.copies; self.copied = text; return true; }
		static bool Valid(void* context, const std::wstring&) { return static_cast<Fixture*>(context)->valid; }
	};
	Fixture* g_fixture = nullptr;
	void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD)
	{
		auto& fixture = *g_fixture;
		HWND dialog = GetLastActivePopup(fixture.owner);
		if (!GetDlgItem(dialog, IDC_NOTIFY_MESSAGE)) return;
		if (fixture.cancel || ++fixture.ticks > 20) { SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return; }
		if (fixture.changeContext) { fixture.valid = false; SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_OPEN, 0); return; }
		if (fixture.park) { SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_PARK, 0); return; }
		if (fixture.remove)
		{
			SendDlgItemMessageW(dialog, IDC_NOTIFY_ITEMS, LB_SETSEL, TRUE, 0);
			SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_REMOVE, 0); SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return;
		}
		Expect(GetFocus() == GetDlgItem(dialog, IDCANCEL), L"the workflow notification prompt defaults focus to Not now");
		SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_OPEN, 0);
		Expect(fixture.calls == 0 && IsWindow(dialog), L"no recipients means no compose launch and the editable notification is retained");
		SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_COPY, 0);
		Expect(fixture.copies == 1 && fixture.copied.find(L"Design model.dgn") != std::wstring::npos, L"Copy message preserves the complete notification text");
		SendDlgItemMessageW(dialog, IDC_NOTIFY_EMAIL, BM_CLICK, 0, 0);
		SetDlgItemTextW(dialog, IDC_NOTIFY_RECIPIENTS, L"colleague@example.test");
		SetDlgItemTextW(dialog, IDC_NOTIFY_NOTE, L"Workflow review: shared for coordination.");
		if (fixture.preview) Expect(g_save(dialog, fixture.preview), L"native workflow notification dialog preview written");
		SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_OPEN, 0);
		Expect(fixture.calls == 1 && IsWindow(dialog) && GetWindowTextLengthW(GetDlgItem(dialog, IDC_NOTIFY_STATUS)) > 0,
			L"a failed email/Teams handler keeps the notification open with an explanatory status");
		fixture.succeeds = true; SendMessageW(dialog, WM_COMMAND, IDC_NOTIFY_OPEN, 0);
	}
}

void CheckNotifications(HWND owner, const wchar_t* preview, void (*expect)(bool, const wchar_t*), bool (*save)(HWND, const wchar_t*))
{
	g_expect = expect; g_save = save; Models();
	N::Batch batch{L"test context", L"Test datasource", L"Test user", {{Document(), Document(7, 200)}}};
	Fixture fixture; fixture.owner = owner; fixture.preview = preview; g_fixture = &fixture;
	UINT_PTR timer = SetTimer(nullptr, 0, 50, Drive);
	if (timer)
	{
		const auto result = N::ShowDialog(owner, GetModuleHandleW(nullptr), batch, {&fixture, Fixture::Open, Fixture::Copy, Fixture::Valid}); KillTimer(nullptr, timer);
		Expect(result == IDOK && fixture.calls == 2 && fixture.uri.starts_with(L"mailto:") && IsWindowEnabled(owner), L"native channel selection launches a draft through the host and restores Explorer ownership");
	}
	fixture = {}; fixture.owner = owner; fixture.cancel = true;
	timer = SetTimer(nullptr, 0, 50, Drive);
	if (timer)
	{
		const auto result = N::ShowDialog(owner, GetModuleHandleW(nullptr), batch, {&fixture, Fixture::Open, Fixture::Copy, Fixture::Valid}); KillTimer(nullptr, timer);
		Expect(result == IDCANCEL && fixture.calls == 0 && fixture.copies == 0, L"Not now performs no external app launch or clipboard write");
	}
	fixture = {}; fixture.owner = owner; fixture.changeContext = true;
	timer = SetTimer(nullptr, 0, 50, Drive);
	if (timer)
	{
		const auto result = N::ShowDialog(owner, GetModuleHandleW(nullptr), batch, {&fixture, Fixture::Open, Fixture::Copy, Fixture::Valid}); KillTimer(nullptr, timer);
		Expect(result == IDCANCEL && fixture.calls == 0, L"changing datasource/user while reviewing a notification prevents compose dispatch");
	}
	fixture = {}; fixture.owner = owner; fixture.park = true;
	timer = SetTimer(nullptr, 0, 50, Drive);
	if (timer)
	{
		const auto result = N::ShowDialog(owner, GetModuleHandleW(nullptr), batch, {&fixture, Fixture::Open, Fixture::Copy, Fixture::Valid}); KillTimer(nullptr, timer);
		Expect(result == IDC_NOTIFY_PARK && fixture.calls == 0 && !batch.changes.empty(), L"Add to basket parks the reviewed links without launching Teams or email");
	}
	fixture = {}; fixture.owner = owner; fixture.remove = true;
	timer = SetTimer(nullptr, 0, 50, Drive);
	if (timer)
	{
		const auto result = N::ShowDialog(owner, GetModuleHandleW(nullptr), batch, {&fixture, Fixture::Open, Fixture::Copy, Fixture::Valid}, true); KillTimer(nullptr, timer);
		Expect(result == IDCANCEL && batch.changes.empty() && fixture.calls == 0, L"basket review removes selected items and closes without sending");
	}
	g_fixture = nullptr;
}
