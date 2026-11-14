#include <windows.h>
#include <aaapi.h>
#include <aadmsapi.h>
#include <delayimp.h>
#include "UserManagerAdapter.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace QuickAccess
{
	namespace
	{
		namespace M = UserManagement;
		struct Buffer
		{
			HAADMSBUFFER value = nullptr;
			~Buffer() { Reset(); }
			void Reset(HAADMSBUFFER next = nullptr) { if (value) aaApi_DmsDataBufferFree(value); value = next; }
			LONG Count() const { return value ? aaApi_DmsDataBufferGetCount(value) : 0; }
			LONG Number(LONG property, LONG row) const { return aaApi_DmsDataBufferGetNumericProperty(value, property, row); }
			std::wstring Text(LONG property, LONG row) const { const auto* text = aaApi_DmsDataBufferGetStringProperty(value, property, row); return text ? text : L""; }
		};
		struct Errors
		{
			LONG_PTR mark = aaApi_AddErrorStackBookmark();
			~Errors() { if (mark >= 0) aaApi_RemoveErrorStackBookmark(mark); }
		};
		int SdkException(DWORD code)
		{
			return code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) || code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
		}
		bool EmptyOrSuccess(const Buffer& buffer, ULONG_PTR before)
		{
			// AAERR_NOT_FOUND is the public SDK's no-record result. Other new errors make a snapshot incomplete.
			return buffer.value != nullptr || aaApi_GetErrorCount() <= before || aaApi_GetLastErrorId() == 50000;
		}
		struct Backend
		{
			HDSOURCE source = nullptr; LONG actor = 0, project = 0; HWND owner = nullptr;
			std::wstring identity, datasource; UserManagerServices services;
			enum class Phase { Catalogue, ProjectAccess, Users, Groups, Folders, Lists, Done } phase = Phase::Done;
			Buffer buffer, users, groups;
			LONG cursor = 0;
			struct Folder { LONG id, parent, type; };
			std::vector<Folder> folders;
			std::unordered_map<LONG, Folder> hierarchy;
			std::unordered_set<LONG> assigned, queued;
			std::vector<LONG> pending;
			std::size_t memberRecords = 0;
			~Backend() { if (services.destroy) services.destroy(services.context); }
			bool Same() const { return IsWindow(owner) && source != nullptr && source == aaApi_GetActiveDatasource() && actor == aaApi_GetCurrentUserId(); }
			bool Valid(bool writing, std::wstring& error)
			{
				if (!Same()) { error = L"Activate the original datasource and user, then Refresh. Pending changes have not been sent."; return false; }
				return services.eligible && services.eligible(services.context, writing, error) && Same();
			}
			bool Prepare(std::wstring& error) { return Valid(true, error) && (!services.prepare || services.prepare(services.context, error)) && Same(); }
			bool Begin(LONG selected, M::Snapshot& snapshot, std::wstring& error)
			{
				if (!Valid(false, error)) return false;
				buffer.Reset(); users.Reset(); groups.Reset(); cursor = 0; folders.clear(); hierarchy.clear(); assigned.clear(); queued.clear(); pending.clear(); memberRecords = 0;
				project = selected;
				if (selected == 0) { snapshot = {}; phase = Phase::Catalogue; }
				else
				{
					if (!snapshot.catalogueReady || std::none_of(snapshot.projects.begin(), snapshot.projects.end(), [&](const auto& item) { return item.id == selected; }))
					{ error = L"Select a readable work area from the project tree."; return false; }
					snapshot.lists.clear(); snapshot.members.clear(); snapshot.ready = false; phase = Phase::Folders;
				}
				snapshot.context = identity; snapshot.datasource = datasource; snapshot.project = selected;
				snapshot.canCreate = services.canCreate && services.canCreate(services.context); return true;
			}
			bool InProject(const Folder& folder)
			{
				LONG id = folder.id; std::unordered_set<LONG> visited;
				while (id > 0 && visited.size() < 512)
				{
					if (id == project) return true;
					if (!visited.insert(id).second) return false;
					const auto found = hierarchy.find(id);
					if (found == hierarchy.end())
					{
						Buffer parent; parent.Reset(aaApi_SelectRichProjectOfFolder(folder.id));
						return parent.Count() == 1 && parent.Number(PROJ_PROP_ID, 0) == project;
					}
					if (found->second.type == AADMS_PROJECT_TYPE_RICH) return false;
					id = found->second.parent;
				}
				return false;
			}
			bool ReadAccessLists(LONG folder, LONG type, std::wstring& error)
			{
				const LONG count = aaApi_SelectAccessControlItems2(AADMSFOAF_IGNORE_PARENTS | AADMSFOAF_IGNORE_ENV | AADMSFOAF_IGNORE_DEFAULT | AADMSFOAF_ALL_WFL_STATES,
					type, folder, 0, 0, 0, 0);
				if (count < 0 || count > 32768) { error = L"ProjectWise could not read a complete set of project security assignments."; return false; }
				for (LONG i = 0; i < count; ++i) if (aaApi_GetAccessControlItemNumericProperty(ACCE_PROP_MEMTYPE, i) == AADMS_MGRTYPE_USERLIST)
				{
					const LONG id = aaApi_GetAccessControlItemNumericProperty(ACCE_PROP_MEMID, i);
					if (id > 0) assigned.insert(id);
				}
				if (assigned.size() > 512) { error = L"This project references more than 512 access userlists. The incomplete view cannot be edited."; return false; }
				return true;
			}
			bool Editable(LONG list)
			{
				const bool administrative = aaApi_HasAdminSetup() && (aaApi_IsCurrentUserAdmin() || aaApi_IsUserRestrictedAdmin(0));
				return administrative || aaApi_CurrentUserHasModifyAccess(AADMSAOTYPE_USERLIST, list, 0, 0, 0) ||
					(aaApi_GetAccessMaskForUser(AADMSAOTYPE_USERLIST, list, 0, 0, 0, actor) & AADMS_ACCESS_OWNER) != 0;
			}
			M::LoadResult Step(M::Snapshot& snapshot, std::wstring& status)
			{
				if (!Valid(false, status)) return M::LoadResult::Failed;
				Errors errors; if (errors.mark < 0) { status = L"ProjectWise error state is unavailable. Refresh to retry."; return M::LoadResult::Failed; }
				if (phase == Phase::Catalogue)
				{
					const auto before = aaApi_GetErrorCount(); buffer.Reset(aaApi_SelectProjectDataBufferByType(AADMS_PROJECT_TYPE_RICH));
					if (!EmptyOrSuccess(buffer, before) || buffer.Count() < 0 || buffer.Count() > 10000)
					{ status = L"A complete work-area catalogue could not be loaded (limit 10000 work areas)."; return M::LoadResult::Failed; }
					phase = Phase::ProjectAccess; cursor = 0;
				}
				if (phase == Phase::ProjectAccess)
				{
					const LONG count = buffer.Count();
					for (int step = 0; cursor < count && step < 8; ++step, ++cursor)
					{
						const LONG id = buffer.Number(PROJ_PROP_ID, cursor);
						if (id > 0 && (aaApi_GetProjectAccess(id, -1) & AADMS_ACCESS_READ)) snapshot.projects.push_back({id, buffer.Text(PROJ_PROP_NAME, cursor)});
					}
					status = L"Loading readable work areas: " + std::to_wstring(cursor) + L" / " + std::to_wstring(count);
					if (cursor < count) return M::LoadResult::Loading;
					buffer.Reset(); users.Reset(aaApi_SelectUserDataBuffer());
					if (!users.value || users.Count() < 0 || users.Count() > 50000) { status = L"The complete user directory could not be loaded (limit 50000 users)."; return M::LoadResult::Failed; }
					phase = Phase::Users; cursor = 0;
				}
				if (phase == Phase::Users)
				{
					const LONG count = users.Count();
					for (int step = 0; cursor < count && step < 500; ++step, ++cursor)
					{
						const LONG id = users.Number(USER_PROP_ID, cursor);
						if (id > 0) snapshot.users.push_back({id, users.Text(USER_PROP_NAME, cursor), users.Text(USER_PROP_DESC, cursor), users.Text(USER_PROP_EMAIL, cursor)});
					}
					status = L"Loading users: " + std::to_wstring(cursor) + L" / " + std::to_wstring(count);
					if (cursor < count) return M::LoadResult::Loading;
					users.Reset(); const auto before = aaApi_GetErrorCount(); groups.Reset(aaApi_SelectGroupMemberDataBuffer());
					if (!EmptyOrSuccess(groups, before) || groups.Count() < 0 || groups.Count() > 250000) { status = L"Complete group membership could not be loaded (limit 250000 links)."; return M::LoadResult::Failed; }
					phase = Phase::Groups; cursor = 0;
				}
				if (phase == Phase::Groups)
				{
					const LONG count = groups.Count();
					for (int step = 0; cursor < count && step < 1000; ++step, ++cursor)
					{
						const LONG group = groups.Number(GRP_MEMBER_PROP_GROUP_ID, cursor), user = groups.Number(GRP_MEMBER_PROP_USER_ID, cursor);
						if (group > 0 && user > 0) snapshot.groups[group].insert(user);
					}
					status = L"Loading group memberships: " + std::to_wstring(cursor) + L" / " + std::to_wstring(count);
					if (cursor < count) return M::LoadResult::Loading;
					groups.Reset(); snapshot.catalogueReady = true; phase = Phase::Done;
					std::sort(snapshot.projects.begin(), snapshot.projects.end(), [](const auto& a, const auto& b) { return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
					return Same() ? M::LoadResult::Ready : M::LoadResult::Failed;
				}
				if (phase == Phase::Folders)
				{
					if (folders.empty())
					{
						if (!(aaApi_GetProjectAccess(project, -1) & AADMS_ACCESS_READ)) { status = L"The selected work area is no longer readable."; return M::LoadResult::Failed; }
						const auto before = aaApi_GetErrorCount(); buffer.Reset(aaApi_SelectProjectDataBufferFromBranch(project, nullptr, nullptr, nullptr, nullptr));
						if (!EmptyOrSuccess(buffer, before) || buffer.Count() < 0 || buffer.Count() > 10000) { status = L"The complete project folder tree could not be loaded (limit 10000 folders)."; return M::LoadResult::Failed; }
						folders.push_back({project, 0, AADMS_PROJECT_TYPE_RICH});
						for (LONG i = 0; i < buffer.Count(); ++i)
						{
							const LONG id = buffer.Number(PROJ_PROP_ID, i);
							if (id > 0 && id != project) folders.push_back({id, buffer.Number(PROJ_PROP_PARENTID, i), buffer.Number(PROJ_PROP_TYPE, i)});
						}
						buffer.Reset(); for (const auto& folder : folders) hierarchy.emplace(folder.id, folder);
					}
					for (int step = 0; static_cast<std::size_t>(cursor) < folders.size() && step < 4; ++step, ++cursor)
					{
						const auto folder = folders[static_cast<std::size_t>(cursor)];
						if (!InProject(folder) || !(aaApi_GetProjectAccess(folder.id, -1) & AADMS_ACCESS_READ)) continue;
						if (!ReadAccessLists(folder.id, AADMSAOTYPE_PROJECT, status) || !ReadAccessLists(folder.id, AADMSAOTYPE_DOCUMENT, status)) return M::LoadResult::Failed;
					}
					status = L"Loading project userlists: " + std::to_wstring(cursor) + L" / " + std::to_wstring(folders.size()) + L" folders";
					if (static_cast<std::size_t>(cursor) < folders.size()) return M::LoadResult::Loading;
					pending.assign(assigned.begin(), assigned.end()); queued = assigned; cursor = 0; phase = Phase::Lists;
				}
				if (phase == Phase::Lists)
				{
					for (int step = 0; static_cast<std::size_t>(cursor) < pending.size() && step < 4; ++step, ++cursor)
					{
						const LONG id = pending[static_cast<std::size_t>(cursor)];
						Buffer list; list.Reset(aaApi_SelectUserListDataBufferByProp(id, AADMSUSRLST_ACCE, 0));
						if (list.Count() != 1) { status = L"A referenced access userlist is unavailable. Refresh the project."; return M::LoadResult::Failed; }
						if (assigned.contains(id)) snapshot.lists.push_back({id, list.Text(USRLST_PROP_NAME, 0), Editable(id)});
						Buffer members; const auto before = aaApi_GetErrorCount(); members.Reset(aaApi_SelectUserListMemberDataBufferByProp(id, -1, -1));
						if (!EmptyOrSuccess(members, before) || members.Count() < 0 || static_cast<std::size_t>(members.Count()) > 32768 - memberRecords)
						{ status = L"Complete nested membership could not be loaded (limit 32768 links)."; return M::LoadResult::Failed; }
						memberRecords += static_cast<std::size_t>(members.Count());
						auto& rows = snapshot.members[id];
						for (LONG i = 0; i < members.Count(); ++i)
						{
							const LONG kind = members.Number(USRLSTMEM_PROP_MEMTYPE, i), member = members.Number(USRLSTMEM_PROP_MEMID, i);
							if (member <= 0 || kind < AADMS_MGRTYPE_USER || kind > AADMS_MGRTYPE_USERLIST) continue;
							rows.push_back({static_cast<M::MemberKind>(kind), member});
							if (kind == AADMS_MGRTYPE_USERLIST && queued.insert(member).second) pending.push_back(member);
						}
						if (pending.size() > 4096) { status = L"Nested membership exceeds 4096 userlists. The incomplete view cannot be edited."; return M::LoadResult::Failed; }
					}
					status = L"Loading userlist memberships: " + std::to_wstring(cursor) + L" / " + std::to_wstring(pending.size());
					if (static_cast<std::size_t>(cursor) < pending.size()) return M::LoadResult::Loading;
					std::sort(snapshot.lists.begin(), snapshot.lists.end(), [](const auto& a, const auto& b) { return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
					snapshot.ready = true; phase = Phase::Done;
					return Same() ? M::LoadResult::Ready : M::LoadResult::Failed;
				}
				return M::LoadResult::Loading;
			}
			bool Inspect(const M::Change& change, bool& direct, bool& editable, std::wstring& error)
			{
				direct = editable = false;
				Errors errors; if (errors.mark < 0) { error = L"The current ProjectWise error state is unavailable."; return false; }
				if (!Same() || change.project != project || !assigned.contains(change.list) || !(aaApi_GetProjectAccess(project, -1) & AADMS_ACCESS_READ))
				{ error = L"The datasource, user or project assignment changed."; return false; }
				Buffer user, list; user.Reset(aaApi_SelectUserDataBufferById(change.user)); list.Reset(aaApi_SelectUserListDataBufferByProp(change.list, AADMSUSRLST_ACCE, 0));
				if (user.Count() != 1 || list.Count() != 1 || user.Text(USER_PROP_NAME, 0) != change.userName || list.Text(USRLST_PROP_NAME, 0) != change.listName)
				{ error = L"A user or userlist was removed or renamed. Refresh and review the changes."; return false; }
				editable = Editable(change.list);
				const LONG count = aaApi_SelectUserListMembers(change.list, AADMS_MGRTYPE_USER, change.user);
				if (count < 0) { error = L"ProjectWise could not verify the current direct membership."; return false; }
				direct = count > 0; return Same();
			}
			bool Write(const M::Change& change, std::wstring& error)
			{
				bool direct = false, editable = false;
				if (!Inspect(change, direct, editable, error) || !editable) { if (error.empty()) error = L"ProjectWise no longer allows this userlist to be edited."; return false; }
				if (direct == change.after) return true;
				Errors errors; if (errors.mark < 0) { error = L"The ProjectWise operation could not be prepared."; return false; }
				const auto before = aaApi_GetErrorCount();
				const BOOL result = change.after ? aaApi_AddUserListMember(change.list, AADMS_MGRTYPE_USER, change.user) : aaApi_RemoveUserListMember(change.list, AADMS_MGRTYPE_USER, change.user);
				if (result) return true;
				const bool detailed = aaApi_GetErrorCount() > before;
				const LONG code = detailed ? aaApi_GetLastErrorId() : 0; const LPCWSTR text = detailed ? aaApi_GetLastErrorMessage() : nullptr;
				error = L"ProjectWise rejected the membership change (error " + std::to_wstring(code) + L").";
				if (text) error += L" " + std::wstring(text);
				const LPCWSTR detail = detailed ? aaApi_GetLastErrorDetail() : nullptr; if (detail) error += L" " + std::wstring(detail);
				return false;
			}
		};
		bool Valid(void* context, bool writing, std::wstring& error)
		{
			__try { return static_cast<Backend*>(context)->Valid(writing, error); }
			__except (SdkException(GetExceptionCode())) { error = L"The ProjectWise user-management dependency is unavailable."; return false; }
		}
		bool Prepare(void* context, std::wstring& error)
		{
			__try { return static_cast<Backend*>(context)->Prepare(error); }
			__except (SdkException(GetExceptionCode())) { error = L"Administrative access could not be established."; return false; }
		}
		bool Begin(void* context, LONG project, M::Snapshot& snapshot, std::wstring& error)
		{
			__try { return static_cast<Backend*>(context)->Begin(project, snapshot, error); }
			__except (SdkException(GetExceptionCode())) { error = L"ProjectWise user-management data is unavailable."; return false; }
		}
		M::LoadResult Step(void* context, M::Snapshot& snapshot, std::wstring& status)
		{
			__try { return static_cast<Backend*>(context)->Step(snapshot, status); }
			__except (SdkException(GetExceptionCode())) { status = L"ProjectWise could not load a complete user-management view."; return M::LoadResult::Failed; }
		}
		bool Inspect(void* context, const M::Change& change, bool& direct, bool& editable, std::wstring& error)
		{
			__try { return static_cast<Backend*>(context)->Inspect(change, direct, editable, error); }
			__except (SdkException(GetExceptionCode())) { error = L"The current membership could not be verified."; return false; }
		}
		bool Write(void* context, const M::Change& change, std::wstring& error)
		{
			__try { return static_cast<Backend*>(context)->Write(change, error); }
			__except (SdkException(GetExceptionCode())) { error = L"The ProjectWise membership operation is unavailable."; return false; }
		}
		bool CreateImpl(void* context, HWND parent, M::User& user, std::wstring& error)
		{
			auto& backend = *static_cast<Backend*>(context);
			return Valid(context, true, error) && backend.services.create && backend.services.create(backend.services.context, parent, user, error);
		}
		bool Create(void* context, HWND parent, M::User& user, std::wstring& error)
		{
			__try { return CreateImpl(context, parent, user, error); }
			__except (SdkException(GetExceptionCode())) { error = L"The ProjectWise account-creation dependency is unavailable."; return false; }
		}
		void Destroy(void* context) { delete static_cast<Backend*>(context); }
	}
	HWND OpenProjectUserManager(HWND owner, HINSTANCE instance, LONG initialProject, UserManagerServices services)
	{
		std::unique_ptr<Backend> backend;
		try { backend = std::make_unique<Backend>(); }
		catch (...) { if (services.destroy) services.destroy(services.context); return nullptr; }
		backend->services = services; backend->owner = owner;
		backend->source = aaApi_GetActiveDatasource(); backend->actor = aaApi_GetCurrentUserId();
		wchar_t datasource[1024]{}; if (!aaApi_GetActiveDatasourceName(datasource, ARRAYSIZE(datasource))) return nullptr;
		backend->datasource = datasource;
		backend->identity = std::to_wstring(reinterpret_cast<UINT_PTR>(backend->source)) + L"/" + std::to_wstring(backend->actor) + L"/" + datasource;
		M::Host host{backend.get(), Valid, Prepare, Begin, Step, Inspect, Write, Create, Destroy}; backend.release();
		return M::Open(owner, instance, host, initialProject);
	}
}
