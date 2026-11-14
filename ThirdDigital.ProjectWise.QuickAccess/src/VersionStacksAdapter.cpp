// ProjectWise SDK adapter. Keep licensed SDK declarations and binaries out of this repository.
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <aaapi.h>
#include <aadmsapi.h>
#include <aawindms.h>
#include <delayimp.h>

#include "VersionStacksAdapter.h"
#include "VersionStacks.h"
#include "QuickAccess.h"
#include "NotificationAdapter.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace QuickAccess
{
	namespace
	{
		namespace VS = VersionStacks;
		constexpr UINT kVisibility = WM_APP + 220;
		constexpr ULONG kIdentityMask = AADOCITEM_PROJECTNO | AADOCITEM_DOCNO | AADOCITEM_SETNO | AADOCITEM_SETTYPE |
			AADOCITEM_EXTRAROW | AADOCITEM_FLAGS | AADOCITEM_ACCESS | AADOCITEM_INDENT | AADOCITEM_DOCGUID | AADOCITEM_VERSIONSEQ;
		constexpr int kMaxRows = 20000;
		struct Adapter;
		HINSTANCE g_instance = nullptr;
		bool g_available = false, g_enabled = true, g_registered = false;
		UINT g_command = 0;
		std::vector<Adapter*> g_views;

		int SdkException(DWORD code) noexcept
		{
			return code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
				code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
		}
		std::wstring Identity(const AADOCLIST_ITEM& item)
		{
			wchar_t guid[40]{}; StringFromGUID2(item.documentGuid, guid, ARRAYSIZE(guid));
			return std::to_wstring(item.lProjectId) + L"/" + std::to_wstring(item.lDocumentId) + L"/" + guid + L"/" +
				std::to_wstring(item.versionSeq) + L"/" + std::to_wstring(item.ulFlags) + L"/" + std::to_wstring(item.ulAccess) + L"/" +
				std::to_wstring(item.lSetId) + L"/" + std::to_wstring(item.lSetType) + L"/" + std::to_wstring(item.iExtraRow) + L"/" + std::to_wstring(item.iIndent);
		}
		struct Buffer
		{
			HAADMSBUFFER value = nullptr;
			~Buffer() { if (value) aaApi_DmsDataBufferFree(value); }
		};
		struct Menu
		{
			HMENU value = CreatePopupMenu();
			~Menu() { if (value) DestroyMenu(value); }
		};
		struct Moniker
		{
			HMONIKER value = nullptr;
			~Moniker() { if (value) aaApi_DestroyMoniker(value); }
		};
		struct Metadata { LONG original = 0, sequence = 0; };
		std::wstring CommandPath(HMENU menu, UINT command, const std::wstring& prefix = {})
		{
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
			{
				wchar_t text[2048]{};
				MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
				item.dwTypeData = text; item.cch = ARRAYSIZE(text);
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) continue;
				const std::wstring path = prefix + L"/" + text;
				if (!item.hSubMenu && item.wID == command) return path;
				if (item.hSubMenu) { const auto found = CommandPath(item.hSubMenu, command, path); if (!found.empty()) return found; }
			}
			return {};
		}
		struct Adapter
		{
			HWND container = nullptr, source = nullptr, view = nullptr;
			ULONGLONG revision = 1, readRevision = 0;
			std::wstring readContext, metadataContext;
			std::vector<std::wstring> metadataIdentities;
			std::map<LONG, Metadata> metadata;
			bool changingSelection = false;
			~Adapter()
			{
				if (IsWindow(source)) RemoveWindowSubclass(source, Observe, reinterpret_cast<UINT_PTR>(this));
				g_views.erase(std::remove(g_views.begin(), g_views.end(), this), g_views.end());
			}
			static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref)
			{
				auto& self = *reinterpret_cast<Adapter*>(ref);
				// Observe before forwarding: the native handler may destroy the extra view and its adapter.
				switch (message)
				{
				case LVM_INSERTITEMA: case LVM_INSERTITEMW: case LVM_DELETEITEM: case LVM_DELETEALLITEMS:
				case LVM_SETITEMA: case LVM_SETITEMW: case LVM_SETITEMTEXTA: case LVM_SETITEMTEXTW:
				case LVM_SETITEMCOUNT: case LVM_SORTITEMS: case LVM_SORTITEMSEX: case LVM_REDRAWITEMS:
				case LVM_INSERTCOLUMNA: case LVM_INSERTCOLUMNW: case LVM_DELETECOLUMN: case LVM_SETCOLUMNA: case LVM_SETCOLUMNW:
				case LVM_SETCOLUMNWIDTH: case LVM_SETCOLUMNORDERARRAY: case LVM_SETIMAGELIST:
				case WM_THEMECHANGED: case WM_SYSCOLORCHANGE:
					++self.revision; break;
				case LVM_SETITEMSTATE: if (!self.changingSelection) ++self.revision; break;
				case WM_NCDESTROY:
					RemoveWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(&self));
					self.source = nullptr; ++self.revision; break;
				}
				return DefSubclassProc(window, message, wParam, lParam);
			}
			bool Attach()
			{
				const HWND current = aaApi_GetDocumentListCtrl(container);
				if (!IsWindow(current) || GetWindowThreadProcessId(current, nullptr) != GetCurrentThreadId()) return false;
				if (current == source) return true;
				if (IsWindow(source)) RemoveWindowSubclass(source, Observe, reinterpret_cast<UINT_PTR>(this));
				source = current; ++revision;
				if (SetWindowSubclass(source, Observe, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) return true;
				source = nullptr; return false;
			}
			std::wstring Context() const
			{
				const HWND frame = aaApi_GetMainFrameWindow();
				if (!g_available || !g_enabled || !IsWindow(container) || !IsWindow(source) || !IsChild(frame, container) || !IsWindowEnabled(frame)) return {};
				const HDSOURCE datasource = aaApi_DocumentListGetDatasource(container);
				const LONG folder = aaApi_DocListGetProject(container), user = aaApi_GetCurrentUserId();
				if (!datasource || datasource != aaApi_GetActiveDatasource() || folder <= 0 || user <= 0) return {};
				return std::to_wstring(reinterpret_cast<UINT_PTR>(datasource)) + L"/" + std::to_wstring(user) + L"/" +
					std::to_wstring(folder) + L"/" + std::to_wstring(reinterpret_cast<UINT_PTR>(source));
			}
			bool Item(int index, AADOCLIST_ITEM& item) const
			{
				item = {}; item.iItem = index; item.ulMask = kIdentityMask;
				return aaApi_DocListGetItem(container, &item) != FALSE;
			}
			bool Validate(const VS::Snapshot& shown) const
			{
				if (shown.context.empty() || Context() != shown.context ||
					ListView_GetItemCount(source) != static_cast<int>(shown.rows.size())) return false;
				for (const auto& row : shown.rows)
				{
					AADOCLIST_ITEM current{};
					if (!Item(row.sourceIndex, current) || Identity(current) != row.identity) return false;
				}
				return Context() == shown.context;
			}
			bool ValidateSelection(const VS::Snapshot& shown) const
			{
				if (!Validate(shown)) return false;
				for (const auto& row : shown.rows)
					if ((ListView_GetItemState(source, row.sourceIndex, LVIS_SELECTED) != 0) != row.selected) return false;
				return true;
			}
			VS::ReadResult Read(VS::Snapshot& fresh, bool force)
			{
				fresh.message = L"Version stacks support ordinary folder document lists. Use Classic list for searches, sets or unavailable data.";
				if (!Attach()) return VS::ReadResult::Unavailable;
				fresh.context = Context();
				if (fresh.context.empty()) return VS::ReadResult::Unavailable;
				const int count = ListView_GetItemCount(source);
				const HWND header = ListView_GetHeader(source);
				const int columns = header ? Header_GetItemCount(header) : 0;
				if (count < 0 || count > kMaxRows || columns <= 0 || columns > 128) return VS::ReadResult::Unavailable;
				if (count * columns > 250000) { fresh.message = L"This list is too large for Version stacks. Use Classic list."; return VS::ReadResult::Unavailable; }
				if (!force && fresh.context == readContext && revision == readRevision) return VS::ReadResult::Unchanged;
				const ULONGLONG observed = revision;
				const LONG folder = aaApi_DocListGetProject(container);
				std::vector<std::wstring> identities;
				std::set<LONG> documents;
				for (int i = 0; i < count; ++i)
				{
					AADOCLIST_ITEM item{};
					if (!Item(i, item) || item.lSetId != 0 || item.iExtraRow != 0 ||
						(item.lDocumentId > 0 && item.lProjectId != folder)) return VS::ReadResult::Unavailable;
					VS::Row row;
					row.project = item.lProjectId; row.document = item.lDocumentId; row.sequence = item.versionSeq; row.sourceIndex = i;
					row.identity = Identity(item); row.groupable = item.lDocumentId > 0;
					row.historical = (item.ulFlags & AADOCIF_VERSION) != 0;
					const UINT state = ListView_GetItemState(source, i, LVIS_SELECTED | LVIS_FOCUSED | LVIS_STATEIMAGEMASK | LVIS_OVERLAYMASK);
					row.selected = (state & LVIS_SELECTED) != 0; row.focused = (state & LVIS_FOCUSED) != 0;
					row.stateImages = state & (LVIS_STATEIMAGEMASK | LVIS_OVERLAYMASK);
					if (row.groupable && !documents.insert(row.document).second) return VS::ReadResult::Unavailable;
					identities.push_back(row.identity); fresh.rows.push_back(std::move(row));
				}
				// One folder-scoped metadata lookup on membership changes/explicit refresh, never a datasource-wide scan or query per row.
				// Only metadata for rows present in the native, permission/filter-constrained list is retained or used.
				if (fresh.context != metadataContext || identities != metadataIdentities || force)
				{
					std::map<LONG, Metadata> values;
					const bool versions = std::any_of(fresh.rows.begin(), fresh.rows.end(), [](const auto& row) { return row.historical; });
					if (!versions)
					{
						for (const auto& row : fresh.rows) if (row.groupable) values.emplace(row.document, Metadata{0, row.sequence});
					}
					else if (!documents.empty())
					{
						Buffer buffer{aaApi_SelectDocumentDataBufferByNameProp2(folder, nullptr, nullptr, nullptr, nullptr, AADMSDOCSELECT_FILTER_SHOW_VERSIONS)};
						if (!buffer.value) return VS::ReadResult::Unavailable;
						const LONG total = aaApi_DmsDataBufferGetCount(buffer.value);
						if (total < 0 || total > 500000) return VS::ReadResult::Unavailable;
						for (LONG i = 0; i < total; ++i)
						{
							const LONG document = aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_ID, i);
							if (!documents.contains(document)) continue;
							const Metadata value{aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_ORIGINALNO, i),
								aaApi_DmsDataBufferGetNumericProperty(buffer.value, DOC_PROP_VERSIONNO, i)};
							if (value.original < 0 || value.sequence < 0 || !values.emplace(document, value).second) return VS::ReadResult::Unavailable;
						}
						if (values.size() != documents.size()) return VS::ReadResult::Unavailable;
					}
					metadata = std::move(values); metadataContext = fresh.context; metadataIdentities = identities;
				}
				std::size_t characters = 0;
				for (auto& row : fresh.rows)
				{
					if (row.groupable)
					{
						const auto found = metadata.find(row.document);
						if (found == metadata.end() || found->second.sequence != row.sequence) return VS::ReadResult::Unavailable;
						row.original = found->second.original;
						// Historical documents must have a documented family identity; do not guess from name or sequence.
						if (row.historical && row.original <= 0) return VS::ReadResult::Unavailable;
					}
					LVITEMW native{}; native.mask = LVIF_IMAGE; native.iItem = row.sourceIndex;
					if (!ListView_GetItem(source, &native)) return VS::ReadResult::Unavailable;
					row.image = native.iImage;
					for (int c = 0; c < columns; ++c)
					{
						std::vector<wchar_t> text(256);
						for (;;)
						{
							LVITEMW textItem{}; textItem.iSubItem = c; textItem.pszText = text.data(); textItem.cchTextMax = static_cast<int>(text.size());
							const int length = static_cast<int>(SendMessageW(source, LVM_GETITEMTEXTW, row.sourceIndex, reinterpret_cast<LPARAM>(&textItem)));
							if (length < static_cast<int>(text.size()) - 1) break;
							if (text.size() >= 32768) return VS::ReadResult::Unavailable;
							text.resize(text.size() * 2);
						}
						row.cells.emplace_back(text.data());
						characters += row.cells.back().size();
						if (characters > 8 * 1024 * 1024) { fresh.message = L"This list is too large for Version stacks. Use Classic list."; return VS::ReadResult::Unavailable; }
					}
				}
				for (int c = 0; c < columns; ++c)
				{
					wchar_t text[1024]{};
					LVCOLUMNW native{}; native.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; native.pszText = text; native.cchTextMax = ARRAYSIZE(text);
					if (!ListView_GetColumn(source, c, &native)) return VS::ReadResult::Unavailable;
					fresh.columns.push_back({text, native.cx, native.fmt});
				}
				fresh.images = ListView_GetImageList(source, LVSIL_SMALL); fresh.stateImages = ListView_GetImageList(source, LVSIL_STATE);
				fresh.columnOrder.resize(static_cast<std::size_t>(columns));
				if (!ListView_GetColumnOrderArray(source, columns, fresh.columnOrder.data())) fresh.columnOrder.clear();
				if (revision != observed || !Validate(fresh)) { fresh.rows.clear(); return VS::ReadResult::Unavailable; }
				readRevision = revision; readContext = fresh.context;
				return VS::ReadResult::Ready;
			}
			bool Select(const VS::Snapshot& shown, const std::vector<std::size_t>& selected, std::size_t focus)
			{
				if (!Validate(shown)) return false;
				std::set<std::size_t> selection(selected.begin(), selected.end());
				for (const auto index : selection) if (index >= shown.rows.size()) return false;
				changingSelection = true;
				struct Reset { bool& flag; ~Reset() { flag = false; } } reset{changingSelection};
				for (std::size_t n = 0; n < shown.rows.size(); ++n)
				{
					const auto& row = shown.rows[n];
					const UINT desired = (selection.contains(n) ? LVIS_SELECTED : 0) | (focus == n ? LVIS_FOCUSED : 0);
					if (ListView_GetItemState(source, row.sourceIndex, LVIS_SELECTED | LVIS_FOCUSED) == desired) continue;
					AADOCLIST_ITEM current{};
					if (Context() != shown.context || !Item(row.sourceIndex, current) || Identity(current) != row.identity) { changingSelection = false; return false; }
					ListView_SetItemState(source, row.sourceIndex, desired, LVIS_SELECTED | LVIS_FOCUSED);
				}
				changingSelection = false;
				return ValidateSelection(shown);
			}
			bool BuildMenu(const VS::Snapshot& shown, Menu& owner, HMENU& commands, bool& documents)
			{
				if (!ValidateSelection(shown) || !owner.value) return false;
				bool folders = false; documents = false;
				for (const auto& row : shown.rows) if (row.selected) { documents |= row.document > 0; folders |= row.document <= 0; }
				if (folders && documents) return false;
				commands = aaApi_InsertMenuByType(documents ? AAMENU_DOCUMENT : AAMENU_PROJECT, owner.value, 0);
				return commands && aaApi_InitMenuByAaControl(container, commands,
					AAMG_MODIFY | AAMG_WORKFLOW | AAMG_CUSTOMS | AAMG_PROPERTIES | AAMG_VIEW) != 0;
			}
			bool Dispatch(const VS::Snapshot& shown, UINT command, std::wstring_view expectedPath = {})
			{
				Menu menu; HMENU commands = nullptr; bool documents = false;
				ButtonState state{};
				if (!BuildMenu(shown, menu, commands, documents) || !ReadMenuState(commands, command, state) || !state.enabled) return false;
				if (!expectedPath.empty() && CommandPath(commands, command) != expectedPath) return false;
				if (!FocusVersionStacks(container)) SetFocus(container);
				if (GetFocus() != container && !IsChild(container, GetFocus()) && VersionStacksListFromFocus(GetFocus()) != container) return false;
				if (!ValidateSelection(shown)) return false;
				const auto notification = BeginWorkflowNotification(command);
				struct Notification { std::uint64_t token; ~Notification() { EndWorkflowNotification(token); } } notify{notification};
				// Native list/view commands can appear inside the Document popup. Only document commands take a document array.
				if (documents && aaApi_GetCommandType(static_cast<LONG>(command)) == AAMENU_DOCUMENT)
				{
					std::vector<AADOC_ITEM> items;
					for (const auto& row : shown.rows) if (row.selected) items.push_back({row.project, row.document});
					if (items.empty()) return false;
					const LONG_PTR outcome = aaApi_ExecuteDocumentCommand(command, static_cast<LONG>(items.size()), items.data());
					++revision;
					return outcome == IDOK || outcome == IDCANCEL;
				}
				// Explorer's ordinary command routing handles selected folders and current-folder operations.
				SendMessageW(aaApi_GetMainFrameWindow(), WM_COMMAND, MAKEWPARAM(command, 0), 0); ++revision;
				return true;
			}
			bool OpenFolder(const VS::Snapshot& shown, std::size_t row)
			{
				if (row >= shown.rows.size() || shown.rows[row].document > 0 || !shown.rows[row].selected || !ValidateSelection(shown)) return false;
				if (std::count_if(shown.rows.begin(), shown.rows.end(), [](const auto& item) { return item.selected; }) != 1) return false;
				LONG folder = shown.rows[row].project;
				Moniker moniker;
				if (folder <= 0 || !aaApi_GetProjectMonikers(1, &folder, &moniker.value) || !moniker.value || !ValidateSelection(shown)) return false;
				// Select through the native tree, keeping its list/address/history buddies synchronized.
				const bool selected = aaApi_ExpandDscTreeItemByMoniker(aaApi_GetMainDscTree(), moniker.value) != FALSE;
				++revision; return selected;
			}
		};

		VS::ReadResult ReadImpl(void* context, VS::Snapshot& fresh, bool force)
		{
			try
			{
				const auto result = static_cast<Adapter*>(context)->Read(fresh, force);
				if (result == VS::ReadResult::Unavailable) { fresh.rows.clear(); fresh.columns.clear(); static_cast<Adapter*>(context)->readRevision = 0; }
				return result;
			}
			catch (...) { fresh = {}; static_cast<Adapter*>(context)->readRevision = 0; return VS::ReadResult::Unavailable; }
		}
		VS::ReadResult Read(void* context, VS::Snapshot& fresh, bool force)
		{
			__try { return ReadImpl(context, fresh, force); }
			__except (SdkException(GetExceptionCode())) { fresh.rows.clear(); fresh.columns.clear(); return VS::ReadResult::Unavailable; }
		}
		bool Select(void* context, const VS::Snapshot& shown, const std::vector<std::size_t>& selected, std::size_t focus)
		{
			__try { return static_cast<Adapter*>(context)->Select(shown, selected, focus); }
			__except (SdkException(GetExceptionCode())) { return false; }
		}
		bool Activate(void* context, const VS::Snapshot& shown, std::size_t row)
		{
			__try
			{
				if (row >= shown.rows.size() || !shown.rows[row].selected) return false;
				return shown.rows[row].document > 0 ? static_cast<Adapter*>(context)->Dispatch(shown, IDMD_OPEN) :
					static_cast<Adapter*>(context)->OpenFolder(shown, row);
			}
			__except (SdkException(GetExceptionCode())) { return false; }
		}
		bool PopupImpl(void* context, const VS::Snapshot& shown, POINT screen)
		{
			auto& self = *static_cast<Adapter*>(context);
			Menu owner; HMENU commands = nullptr; bool documents = false;
			if (!self.BuildMenu(shown, owner, commands, documents)) return false;
			// The SDK window must own menu draw/measure/update messages for native and extension menu items.
			const UINT command = TrackPopupMenuEx(commands, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, self.container, nullptr);
			const auto path = CommandPath(commands, command);
			return command == 0 || (!path.empty() && self.Dispatch(shown, command, path));
		}
		bool Popup(void* context, const VS::Snapshot& shown, POINT screen)
		{
			__try { return PopupImpl(context, shown, screen); }
			__except (SdkException(GetExceptionCode())) { return false; }
		}
		void VisibilityImpl() noexcept
		{
			try
			{
				// Copy handles because hiding the selected extra view may synchronously destroy it.
				std::vector<std::pair<HWND, HWND>> views;
				for (const auto* view : g_views) views.push_back({view->container, view->view});
				for (const auto& [container, view] : views)
					if (IsWindow(container) && IsWindow(view)) aaApi_ShowDocumentListControlView(container, view, g_available && g_enabled);
			}
			catch (...) { OutputDebugStringW(L"ProjectWise Version stacks: could not change extra-view availability.\n"); }
		}
		void Visibility() noexcept
		{
			__try { VisibilityImpl(); }
			__except (SdkException(GetExceptionCode())) { OutputDebugStringW(L"ProjectWise Version stacks: additional view unavailable; native list retained.\n"); }
		}
		void Classic(void*) { g_enabled = false; Visibility(); }
		void Destroy(void* context) { delete static_cast<Adapter*>(context); }
		LRESULT CALLBACK ViewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
		{
			if (message == kVisibility) { Visibility(); return 0; }
			return DefSubclassProc(window, message, wParam, lParam);
		}
		BOOL CALLBACK CreateView(HWND container, ULONG, ULONG, LONG event, LPARAM, LPVOID parameter)
		{
			if (event != AATYPE_CREATE_VIEW || !parameter) return FALSE;
			HWND created = nullptr;
			try
			{
				auto* info = static_cast<LPAATYPE_DOCLISTCTRL_CREATE_INFO>(parameter);
				auto adapter = std::make_unique<Adapter>(); adapter->container = container;
				const auto self = adapter.release();
				const HWND view = VS::Create(info->parentHwnd, g_instance, {self, Read, Select, Activate, Popup, Classic, Destroy});
				if (!view) return FALSE; // Create has already released the adapter
				created = view;
				self->view = view;
				g_views.push_back(self);
				if (!SetWindowSubclass(view, ViewProc, 2, 0)) { DestroyWindow(view); return FALSE; }
				info->createdHwnd = view; info->viewName = L"Version stacks"; info->viewImage = nullptr; info->crMask = RGB(255, 0, 255);
				PostMessageW(view, kVisibility, 0, 0);
				return TRUE;
			}
			catch (...) { if (IsWindow(created)) DestroyWindow(created); return FALSE; }
		}
		LONG CALLBACK Toggle(LPAACMDPARAM command)
		{
			if (!command || static_cast<UINT>(command->lCommandId) != g_command || !g_available) return -1;
			g_enabled = !g_enabled; Visibility(); return 0;
		}
		LONG CALLBACK ToggleState(LPAACMDSTATE) { return !g_available || !g_registered ? AAMS_GRAYED : g_enabled ? AAMS_CHECKED : 0; }
		bool RegisterImpl(HINSTANCE instance, bool available)
		{
			g_instance = instance; g_available = available;
			if (g_registered) return true;
			const ULONG anchor = aaApi_GetCommandHandle(AAMENU_VIEW, ID_VIEW_LARGEICONS);
			const ULONG parent = anchor ? aaApi_GetNextMenuItem(anchor, 0, AAMIPF_PARENT) : 0;
			if (!parent) { OutputDebugStringW(L"ProjectWise Version stacks: could not locate Explorer's View menu.\n"); return false; }
			g_command = aaApi_ReserveCommandIdByType(AAMENU_CUSTOM);
			if (!g_command) { OutputDebugStringW(L"ProjectWise Version stacks: could not reserve visibility command.\n"); return false; }
			AADMSMENUITEM item{};
			item.ulMask = AADMSMIF_FLAGS | AADMSMIF_NAME | AADMSMIF_CMDTYPE | AADMSMIF_CMDID | AADMSMIF_CALLBACK | AADMSMIF_STATEPROC;
			item.uiFlags = AAMIF_COMMAND; item.uiCmdType = AAMENU_CUSTOM; item.uiCommandId = g_command; item.ushParamType = AACMDPT_GENERIC;
			item.lptstrName = const_cast<LPWSTR>(L"Version stacks"); item.fpExecute = Toggle; item.fpGetState = ToggleState;
			if (!aaApi_AddCustomMenuItem(parent, AADMSMI_LAST, &item)) { OutputDebugStringW(L"ProjectWise Version stacks: could not add visibility command to View menu.\n"); return false; }
			g_registered = aaApi_AddDocumentListControlView(L"AtkinsRealis.ProjectWise.VersionStacks", CreateView) != FALSE;
			if (!g_registered) OutputDebugStringW(L"ProjectWise Version stacks: SDK rejected additional document-list view registration.\n");
			return g_registered;
		}
		bool TryRegister(HINSTANCE instance, bool available) noexcept
		{
			try { return RegisterImpl(instance, available); }
			catch (...) { return false; }
		}
	}
	bool RegisterVersionStacks(HINSTANCE instance, bool available) noexcept
	{
		__try { return TryRegister(instance, available); }
		__except (SdkException(GetExceptionCode())) { return false; }
	}
	void SetVersionStacksAvailable(bool available) noexcept
	{
		g_available = available;
		Visibility();
	}
	HWND VersionStacksListFromFocus(HWND focus) noexcept
	{
		if (!g_available || !g_enabled || !IsWindow(focus)) return nullptr;
		for (const auto* view : g_views)
			if (IsWindowVisible(view->view) && (focus == view->view || IsChild(view->view, focus))) return view->container;
		return nullptr;
	}
	bool FocusVersionStacks(HWND documentList) noexcept
	{
		if (!g_available || !g_enabled) return false;
		for (const auto* candidate : g_views) if (candidate->container == documentList && IsWindowVisible(candidate->view))
		{
			const HWND view = candidate->view;
			SetFocus(view);
			return GetFocus() == view || IsChild(view, GetFocus());
		}
		return false;
	}
}
