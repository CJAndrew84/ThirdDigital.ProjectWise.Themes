#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <aaapi.h>
#include <aadmsapi.h>
#include <aawindms.h>
#include <delayimp.h>
#include "NotificationAdapter.h"
#include "Notifications.h"
#include "NotificationStore.h"
#include "NotificationResources.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace QuickAccess
{
	namespace
	{
		namespace N = Notifications;
		constexpr wchar_t kClass[] = L"AtkinsRealis.ProjectWise.WorkflowNotifications";
		HINSTANCE g_instance = nullptr;
		HWND g_window = nullptr;
		DWORD g_thread = 0;
		UINT g_command = 0, g_basketCommand = 0, g_parkCommand = 0;
		bool g_available = false, g_enabled = true, g_started = false, g_dialog = false, g_polling = false;
		bool (*g_workflowCommand)(UINT) = nullptr;
		N::Watcher g_watcher;
		N::Basket g_basket;
		bool g_autoPark = false, g_storageBlocked = false, g_saveWarning = false;
		std::wstring g_context;
		std::wstring g_owner;
		struct Unsaved { N::Batch batch; bool blocked = false; };
		std::map<std::wstring, Unsaved> g_unsaved;
		std::vector<N::Batch> g_ready;
		std::map<LONG, std::wstring> g_states;
		std::uint64_t g_activeToken = 0;
		struct HookCall { LONG hook; AAPARAM parameter; std::uint64_t token; };
		std::vector<HookCall> g_calls;
		struct Buffer { HAADMSBUFFER value; ~Buffer() { if (value) aaApi_DmsDataBufferFree(value); } };
		struct Errors
		{
			LONG_PTR bookmark = aaApi_AddErrorStackBookmark();
			~Errors() { if (bookmark >= 0) aaApi_RemoveErrorStackBookmark(bookmark); else aaApi_RemoveAllErrors(); }
		};
		int SdkException(DWORD code) noexcept
		{
			return code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
				code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
		}
		std::wstring Datasource()
		{
			wchar_t value[1024]{}; return aaApi_GetActiveDatasourceName(value, ARRAYSIZE(value)) ? value : L"";
		}
		std::wstring SessionContext()
		{
			if (!g_started || GetCurrentThreadId() != g_thread) return {};
			const HDSOURCE source = aaApi_GetActiveDatasource(); const LONG user = aaApi_GetCurrentUserId();
			if (!source || user <= 0) return {};
			const auto name = Datasource(); if (name.empty()) return {};
			return std::to_wstring(reinterpret_cast<UINT_PTR>(source)) + L"/" + std::to_wstring(user) + L"/" + name;
		}
		std::wstring Context() { return g_available && g_enabled ? SessionContext() : std::wstring{}; }
		std::wstring StateName(LONG id)
		{
			if (id <= 0) return L"No workflow state";
			if (const auto found = g_states.find(id); found != g_states.end()) return found->second;
			Buffer buffer{aaApi_SelectStateDataBuffer(id)};
			std::wstring name = L"State " + std::to_wstring(id);
			if (buffer.value && aaApi_DmsDataBufferGetCount(buffer.value) == 1)
			{
				const LPCWSTR value = aaApi_DmsDataBufferGetStringProperty(buffer.value, STATE_PROP_NAME, 0);
				if (value && *value) name = value;
			}
			if (g_states.size() >= 256) g_states.clear();
			g_states[id] = name; return name;
		}
		LPWSTR NativeWebLink(AADOC_ITEM* document)
		{
			__try { return aaApi_GetDocumentWebLinksByIds(1, document); }
			__except (SdkException(GetExceptionCode())) { return nullptr; }
		}
		std::wstring Link(AADOC_ITEM document, const std::wstring& guid)
		{
			std::wstring result;
			if (LPWSTR value = NativeWebLink(&document)) { result = value; aaApi_Free(value); }
			while (!result.empty() && iswspace(result.back())) result.pop_back();
			while (!result.empty() && iswspace(result.front())) result.erase(result.begin());
			return N::ModernDocumentLink(result, Datasource(), guid);
		}
		bool Read(const AADOC_ITEM& id, N::Document& document, bool includeLink)
		{
			if (id.lProjectId <= 0 || id.lDocumentId <= 0) return false;
			Buffer buffer{aaApi_SelectDocumentDataBuffer(id.lProjectId, id.lDocumentId)};
			if (!buffer.value || aaApi_DmsDataBufferGetCount(buffer.value) != 1) return false;
			document = {}; document.project = id.lProjectId; document.id = id.lDocumentId;
			document.sequence = aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_VERSIONNO, 0);
			document.workflow = aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_WORKFLOWID, 0);
			document.state = aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_STATEID, 0);
			const LPCGUID guid = aaApi_DmsDataBufferGetGuidProperty(buffer.value, DOC_PROP_DOCGUID, 0);
			if (!guid || IsEqualGUID(*guid, GUID_NULL)) return false;
			wchar_t text[40]{}; StringFromGUID2(*guid, text, ARRAYSIZE(text)); document.guid = text;
			const LPCWSTR name = aaApi_DmsDataBufferGetStringProperty(buffer.value, DOC_PROP_NAME, 0);
			document.name = name ? name : L"";
			const LPCWSTR version = aaApi_DmsDataBufferGetStringProperty(buffer.value, DOC_PROP_VERSION, 0);
			document.version = version ? version : L"";
			document.workflowName = document.workflow > 0 ? L"Workflow " + std::to_wstring(document.workflow) : L"No workflow";
			document.stateName = StateName(document.state);
			if (includeLink) document.link = Link(id, document.guid);
			return document.sequence >= 0 && document.workflow >= 0 && document.state >= 0;
		}
		void Reset(const std::wstring& context)
		{
			if (g_context == context) return;
			if (!g_owner.empty())
			{
				if (g_storageBlocked || !N::SaveBasket(g_owner, g_basket.Items())) g_unsaved[g_owner] = {g_basket.Items(), g_storageBlocked};
				else g_unsaved.erase(g_owner);
			}
			g_context = context; g_watcher.Clear(); g_ready.clear(); g_states.clear(); g_activeToken = 0;
			g_basket.Clear(); g_owner.clear(); g_storageBlocked = false; g_saveWarning = false;
			if (!context.empty())
			{
				g_owner = Datasource() + L"/user/" + std::to_wstring(aaApi_GetCurrentUserId());
				if (const auto cached = g_unsaved.find(g_owner); cached != g_unsaved.end())
				{
					auto batch = cached->second.batch; batch.context = context; g_basket.Replace(std::move(batch)); g_storageBlocked = cached->second.blocked; return;
				}
				N::Batch saved;
				if (N::LoadBasket(g_owner, context, saved)) g_basket.Replace(std::move(saved));
				else g_storageBlocked = true; // Preserve an unreadable existing record instead of overwriting it.
			}
		}
		bool Persist()
		{
			if (g_owner.empty()) return false;
			if (g_storageBlocked || !N::SaveBasket(g_owner, g_basket.Items())) { g_unsaved[g_owner] = {g_basket.Items(), g_storageBlocked}; return false; }
			g_unsaved.erase(g_owner); return true;
		}
		bool Park(const N::Batch& batch)
		{
			if (!g_basket.Add(batch))
			{
				MessageBoxW(aaApi_GetMainFrameWindow(), L"The notification basket is full (250 document/version entries). Review and clear items, then try adding this batch again.", L"Notification basket", MB_OK | MB_ICONINFORMATION);
				return false;
			}
			if (!Persist() && !g_saveWarning)
			{
				g_saveWarning = true; g_autoPark = false;
				MessageBoxW(aaApi_GetMainFrameWindow(), L"The basket is available in this session, but Windows could not save it. Keep Explorer open and use Copy message before exiting. Any unreadable saved basket has been preserved.", L"Notification basket", MB_OK | MB_ICONINFORMATION);
			}
			return true;
		}
		bool Capture(const std::vector<AADOC_ITEM>& ids, std::vector<N::Document>& before)
		{
			if (ids.empty() || ids.size() > N::Watcher::kMaxDocuments) return false;
			Errors errors;
			for (const auto& id : ids)
			{
				N::Document document;
				if (!Read(id, document, true) || document.workflow <= 0) return false;
				if (std::none_of(before.begin(), before.end(), [&](const auto& value) { return N::SameDocument(value, document); })) before.push_back(std::move(document));
			}
			return true;
		}
		std::uint64_t BeginIds(const std::vector<AADOC_ITEM>& ids)
		{
			if (g_activeToken || g_dialog || g_polling) return 0;
			const auto context = Context(); if (context.empty()) return 0;
			Reset(context);
			std::vector<N::Document> before; if (!Capture(ids, before) || Context() != context) return 0;
			g_activeToken = g_watcher.Begin(context, Datasource(), L"ProjectWise user " + std::to_wstring(aaApi_GetCurrentUserId()), before, GetTickCount64());
			return g_activeToken;
		}
		std::uint64_t BeginSelection(UINT command)
		{
			if (g_activeToken || !g_workflowCommand || !g_workflowCommand(command) || Context().empty()) return 0;
			HWND list = aaApi_GetActiveDocumentList(); if (!IsWindow(list)) list = aaApi_GetMainDocumentList();
			if (!IsWindow(list) || aaApi_DocumentListGetDatasource(list) != aaApi_GetActiveDatasource()) return 0;
			const HWND control = aaApi_GetDocumentListCtrl(list);
			if (!IsWindow(control) || ListView_GetSelectedCount(control) > N::Watcher::kMaxDocuments) return 0;
			std::vector<AADOC_ITEM> ids;
			for (int i = ListView_GetNextItem(control, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(control, i, LVNI_SELECTED))
			{
				LONG project = 0, document = 0, set = 0;
				if (!aaApi_DocListGetData(list, i, &project, &document, &set) || project <= 0 || document <= 0 || set != 0) return 0;
				ids.push_back({project, document});
			}
			return BeginIds(ids);
		}
		void Complete(std::uint64_t token)
		{
			if (!token) return;
			g_watcher.Complete(token, GetTickCount64());
			if (g_activeToken == token) g_activeToken = 0;
		}
		LONG HookImpl(LONG hook, LONG type, AAPARAM parameter)
		{
			// Hooks are observers. Never replace an action, change parameters/result, show UI, or propagate observer errors.
			if (GetCurrentThreadId() != g_thread || g_polling || g_dialog) return AAHOOK_SUCCESS;
			if (type == AAPREHOOK)
			{
				std::uint64_t token = 0;
				if (!Context().empty())
				{
					if (hook == AAHOOK_EXEC_MENU_COMMAND)
					{
						const auto* command = reinterpret_cast<const AACMDPARAM*>(parameter);
						if (command) token = BeginSelection(static_cast<UINT>(command->lCommandId));
					}
					else if (hook == AAHOOK_CHANGE_DOC_STATE)
					{
						const auto* documents = reinterpret_cast<const AADOCUMENTS_PARAM*>(parameter);
						if (documents && documents->lpDocuments && documents->lCount > 0 && documents->lCount <= static_cast<LONG>(N::Watcher::kMaxDocuments))
						{
							std::vector<AADOC_ITEM> ids(documents->lpDocuments, documents->lpDocuments + documents->lCount);
							if (g_activeToken)
							{
								std::vector<N::Document> before; if (Capture(ids, before)) g_watcher.Rebase(g_activeToken, before);
							}
							else token = BeginIds(ids);
						}
					}
				}
				if (g_calls.size() < 64) g_calls.push_back({hook, parameter, token});
				else Complete(token);
			}
			else if (type == AAPOSTHOOK || type == AAPOSTHOOK_FAIL)
			{
				for (std::size_t i = g_calls.size(); i > 0; --i) if (g_calls[i - 1].hook == hook && g_calls[i - 1].parameter == parameter)
				{
					const auto token = g_calls[i - 1].token; g_calls.erase(g_calls.begin() + static_cast<std::ptrdiff_t>(i - 1)); Complete(token); break;
				}
			}
			return AAHOOK_SUCCESS;
		}
		LONG SafeHook(LONG hook, LONG type, AAPARAM parameter) noexcept
		{
			try { return HookImpl(hook, type, parameter); }
			catch (...) { return AAHOOK_SUCCESS; }
		}
		LONG CALLBACK Hook(LONG hook, LONG type, AAPARAM first, AAPARAM, AARESULT*)
		{
			__try { return SafeHook(hook, type, first); }
			__except (SdkException(GetExceptionCode())) { return AAHOOK_SUCCESS; }
		}
		bool Valid(void*, const std::wstring& context) { return SessionContext() == context; }
		bool Open(void*, const std::wstring& uri, std::wstring& error)
		{
			if (!(uri.starts_with(L"https://teams.microsoft.com/l/chat/0/0?users=") || uri.starts_with(L"mailto:"))) return false;
			const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(aaApi_GetMainFrameWindow(), L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
			if (result > 32) return true;
			error = L"Windows could not open the app. Check your Teams/email association, or copy the message."; return false;
		}
		bool Copy(void*, const std::wstring& text, std::wstring& error)
		{
			const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
			if (!memory) return false;
			void* destination = GlobalLock(memory);
			if (!destination) { GlobalFree(memory); return false; }
			std::memcpy(destination, text.c_str(), bytes); GlobalUnlock(memory);
			bool copied = false;
			if (OpenClipboard(aaApi_GetMainFrameWindow()))
			{ if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true; CloseClipboard(); }
			if (!copied) { GlobalFree(memory); error = L"The clipboard is unavailable. Try Copy message again."; }
			return copied;
		}
		void Tick()
		{
			if (g_polling || g_dialog) return;
			const auto context = SessionContext(); Reset(context);
			if (context.empty() || !g_available || !g_enabled) { g_watcher.Clear(); g_ready.clear(); return; }
			if (g_activeToken) return;
			const HWND frame = aaApi_GetMainFrameWindow(); if (!IsWindow(frame) || !IsWindowEnabled(frame)) return;
			g_polling = true;
			struct Polling { ~Polling() { g_polling = false; } } polling;
			{
				Errors errors;
				for (const auto& probe : g_watcher.Due(context, GetTickCount64()))
				{
					N::Document after;
					const bool found = Read({probe.before.project, probe.before.id}, after, false);
					if (Context() != context) { Reset(Context()); return; }
					after.link = probe.before.link;
					g_watcher.Observe(probe.token, probe.before, found ? &after : nullptr, GetTickCount64());
				}
			}
			for (auto& batch : g_watcher.TakeReady(context, GetTickCount64()))
			{
				SYSTEMTIME utc{}; GetSystemTime(&utc); wchar_t time[40]{};
				swprintf_s(time, L"%04u-%02u-%02u %02u:%02u:%02u UTC", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
				for (auto& change : batch.changes) change.observedAt = time;
				if (g_autoPark) { if (!Park(batch) && g_ready.size() < 4) g_ready.push_back(std::move(batch)); }
				else if (g_ready.size() < 4) g_ready.push_back(std::move(batch));
				else Park(batch); // Busy users retain overflow in the basket instead of silently dropping confirmed changes.
			}
			const HWND foreground = GetForegroundWindow();
			if (g_ready.empty() || (foreground != frame && GetAncestor(foreground, GA_ROOTOWNER) != frame)) return;
			N::Batch batch = std::move(g_ready.front()); g_ready.erase(g_ready.begin());
			{
				Errors errors;
				// Recheck access/identity/state immediately before displaying any queued document details.
				std::erase_if(batch.changes, [&](const auto& change) {
					N::Document current; return !Read({change.after.project, change.after.id}, current, false) ||
						!N::SameDocument(current, change.after) || !N::SameState(current, change.after);
				});
			}
			if (batch.changes.empty() || Context() != context) return;
			g_dialog = true;
			struct Dialog { ~Dialog() { g_dialog = false; } } dialog;
			if (N::ShowDialog(frame, g_instance, batch, {nullptr, Open, Copy, Valid}) == IDC_NOTIFY_PARK && Context() == context)
				if (!Park(batch) && g_ready.size() < 4) g_ready.push_back(std::move(batch));
		}
		void TickSafe() noexcept { try { Tick(); } catch (...) { g_polling = false; g_dialog = false; } }
		LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			if (message == WM_TIMER)
			{
				__try { TickSafe(); }
				__except (SdkException(GetExceptionCode())) { g_watcher.Clear(); g_ready.clear(); g_polling = false; g_dialog = false; }
				return 0;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}
		LONG CALLBACK Toggle(LPAACMDPARAM command)
		{
			if (!command || static_cast<UINT>(command->lCommandId) != g_command || !g_available || !g_started) return -1;
			g_enabled = !g_enabled;
			if (!g_enabled) { g_watcher.Clear(); g_ready.clear(); g_activeToken = 0; }
			return 0;
		}
		LONG CALLBACK State(LPAACMDSTATE) { return !g_available || !g_started ? AAMS_GRAYED : g_enabled ? AAMS_CHECKED : 0; }
		LONG BasketImpl(LPAACMDPARAM command)
		{
			if (!command || !g_started || g_dialog || g_polling) return -1;
			const auto context = SessionContext(); if (context.empty()) return -1; Reset(context);
			if (static_cast<UINT>(command->lCommandId) == g_parkCommand)
			{ if (!g_available || !g_enabled) return -1; g_autoPark = !g_autoPark; return 0; }
			if (static_cast<UINT>(command->lCommandId) != g_basketCommand) return -1;
			N::Batch batch = g_basket.Items(); if (batch.changes.empty()) return 0;
			const HWND frame = aaApi_GetMainFrameWindow(); if (!IsWindowEnabled(frame)) return -1;
			g_dialog = true;
			struct Dialog { ~Dialog() { g_dialog = false; } } dialog;
			// This is the user's saved activity/draft, not a refreshed document-list view. Keep the recorded observed state.
			N::ShowDialog(frame, g_instance, batch, {nullptr, Open, Copy, Valid}, true);
			if (SessionContext() == context)
			{
				g_basket.Replace(std::move(batch));
				if (!Persist()) MessageBoxW(frame, L"The basket changes could not be saved. Copy the message before closing Explorer.", L"Notification basket", MB_OK | MB_ICONINFORMATION);
			}
			return 0;
		}
		LONG BasketSafe(LPAACMDPARAM command) noexcept { try { return BasketImpl(command); } catch (...) { g_dialog = false; return -1; } }
		LONG CALLBACK BasketCommand(LPAACMDPARAM command)
		{
			__try { return BasketSafe(command); }
			__except (SdkException(GetExceptionCode())) { g_dialog = false; return -1; }
		}
		LONG CALLBACK BasketState(LPAACMDSTATE)
		{
			return !g_started || g_basket.Items().changes.empty() ? AAMS_GRAYED : 0;
		}
		LONG CALLBACK ParkState(LPAACMDSTATE) { return !g_available || !g_started || !g_enabled ? AAMS_GRAYED : g_autoPark ? AAMS_CHECKED : 0; }
		bool AddMenu(const wchar_t* text, UINT& id, bool basket)
		{
			id = aaApi_ReserveCommandIdByType(AAMENU_TOOLS); if (!id) return false;
			AADMSMENUITEM item{};
			item.ulMask = AADMSMIF_FLAGS | AADMSMIF_NAME | AADMSMIF_CMDTYPE | AADMSMIF_CMDID | AADMSMIF_CALLBACK | AADMSMIF_STATEPROC;
			item.uiFlags = AAMIF_COMMAND; item.uiCmdType = AAMENU_TOOLS; item.uiCommandId = id; item.ushParamType = AACMDPT_GENERIC;
			item.lptstrName = const_cast<LPWSTR>(text); item.fpExecute = BasketCommand; item.fpGetState = basket ? BasketState : ParkState;
			return aaApi_AddCustomMenuItem(0, AADMSMI_LAST, &item) != 0;
		}
		bool Start(HINSTANCE instance, bool (*workflowCommand)(UINT))
		{
			if (g_started) return true;
			g_instance = instance; g_thread = GetCurrentThreadId(); g_workflowCommand = workflowCommand;
			WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc; wc.lpszClassName = kClass;
			if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
			g_window = CreateWindowExW(0, kClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
			if (!g_window || !SetTimer(g_window, 1, 2000, nullptr)) { if (g_window) DestroyWindow(g_window); g_window = nullptr; return false; }
			std::vector<std::pair<LONG, LONG>> registered;
			for (LONG hook : {AAHOOK_EXEC_MENU_COMMAND, AAHOOK_CHANGE_DOC_STATE}) for (LONG type : {AAPREHOOK, AAPOSTHOOK, AAPOSTHOOK_FAIL})
			{
				if (!aaApi_AddHook(hook, type, Hook))
				{
					for (const auto& [id, kind] : registered) aaApi_RemoveHook(id, kind, Hook);
					DestroyWindow(g_window); g_window = nullptr; return false;
				}
				registered.push_back({hook, type});
			}
			g_command = aaApi_ReserveCommandIdByType(AAMENU_TOOLS);
			AADMSMENUITEM item{};
			item.ulMask = AADMSMIF_FLAGS | AADMSMIF_NAME | AADMSMIF_CMDTYPE | AADMSMIF_CMDID | AADMSMIF_CALLBACK | AADMSMIF_STATEPROC;
			item.uiFlags = AAMIF_COMMAND; item.uiCmdType = AAMENU_TOOLS; item.uiCommandId = g_command; item.ushParamType = AACMDPT_GENERIC;
			item.lptstrName = const_cast<LPWSTR>(L"Prompt after workflow state changes"); item.fpExecute = Toggle; item.fpGetState = State;
			if (!g_command || !aaApi_AddCustomMenuItem(0, AADMSMI_LAST, &item))
			{
				for (const auto& [id, kind] : registered) aaApi_RemoveHook(id, kind, Hook);
				DestroyWindow(g_window); g_window = nullptr; return false;
			}
			if (!AddMenu(L"Notification basket...", g_basketCommand, true) || !AddMenu(L"Park workflow notifications automatically", g_parkCommand, false))
			{
				for (const auto& [id, kind] : registered) aaApi_RemoveHook(id, kind, Hook);
				DestroyWindow(g_window); g_window = nullptr; return false;
			}
			g_started = true; g_available = true; return true;
		}
		bool TryStart(HINSTANCE instance, bool (*workflowCommand)(UINT)) noexcept { try { return Start(instance, workflowCommand); } catch (...) { return false; } }
		std::uint64_t TryBegin(UINT command) noexcept { try { return BeginSelection(command); } catch (...) { return 0; } }
	}
	bool StartWorkflowNotifications(HINSTANCE instance, bool (*workflowCommand)(UINT)) noexcept
	{
		__try { return TryStart(instance, workflowCommand); }
		__except (SdkException(GetExceptionCode())) { return false; }
	}
	void SetWorkflowNotificationsAvailable(bool available) noexcept
	{
		g_available = available;
		if (!available) { g_watcher.Clear(); g_ready.clear(); g_activeToken = 0; }
	}
	std::uint64_t BeginWorkflowNotification(UINT command) noexcept
	{
		__try { return TryBegin(command); }
		__except (SdkException(GetExceptionCode())) { return 0; }
	}
	void EndWorkflowNotification(std::uint64_t token) noexcept { Complete(token); }
}
