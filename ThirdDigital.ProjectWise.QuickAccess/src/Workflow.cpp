#include "Workflow.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace QuickAccess
{
	namespace
	{
		bool Standard(UINT command, WorkflowMenuIds ids) noexcept
		{
			return command == ids.next || command == ids.previous || command == ids.change;
		}

		HMENU FindBranch(HMENU menu, WorkflowMenuIds ids, bool& enabled, int depth = 0)
		{
			if (depth > 16) return nullptr;
			bool found = false;
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
			{
				MENUITEMINFOW item{sizeof(item)};
				item.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
				if (!GetMenuItemInfoW(menu, i, TRUE, &item)) return nullptr;
				if (item.hSubMenu != nullptr)
				{
					bool childEnabled = enabled && (item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
					// WRE can replace every standard child. The native Change State popup ID still anchors its branch.
					if (item.wID == ids.change) { enabled = childEnabled; return item.hSubMenu; }
					if (const HMENU branch = FindBranch(item.hSubMenu, ids, childEnabled, depth + 1))
					{
						enabled = childEnabled;
						return branch;
					}
				}
				else if ((item.fType & MFT_SEPARATOR) == 0 && Standard(item.wID, ids)) found = true;
			}
			return found ? menu : nullptr;
		}

		std::wstring Label(HMENU menu, int index)
		{
			MENUITEMINFOW info{sizeof(info)};
			info.fMask = MIIM_STRING;
			if (!GetMenuItemInfoW(menu, index, TRUE, &info)) return {};
			std::wstring raw(static_cast<std::size_t>(info.cch) + 1, L'\0');
			info.dwTypeData = raw.data();
			info.cch = static_cast<UINT>(raw.size());
			if (!GetMenuItemInfoW(menu, index, TRUE, &info)) return {};
			raw.resize(info.cch);
			std::wstring text;
			for (std::size_t i = 0; i < raw.size() && raw[i] != L'\t'; ++i)
			{
				if (raw[i] == L'&')
				{
					if (i + 1 == raw.size() || raw[i + 1] != L'&') continue;
					++i;
				}
				text += raw[i];
			}
			while (!text.empty() && iswspace(text.back())) text.pop_back();
			const auto first = std::find_if_not(text.begin(), text.end(), [](wchar_t c) { return iswspace(c) != 0; });
			text.erase(text.begin(), first);
			return text;
		}

		bool Collect(HMENU menu, WorkflowMenuIds ids, bool enabled, std::vector<WorkflowAction>& actions, int depth = 0)
		{
			if (depth > 16) return false;
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
			{
				MENUITEMINFOW item{sizeof(item)};
				item.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
				if (!GetMenuItemInfoW(menu, i, TRUE, &item)) return false;
				const bool available = enabled && (item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
				if (item.hSubMenu != nullptr)
				{
					if (!Collect(item.hSubMenu, ids, available, actions, depth + 1)) return false;
					continue;
				}
				if ((item.fType & MFT_SEPARATOR) != 0 || item.wID == 0 || item.wID == static_cast<UINT>(-1) ||
					item.wID == ids.setFinal || item.wID == ids.removeFinal) continue;
				if (actions.size() >= kMaxWorkflowActions ||
					std::any_of(actions.begin(), actions.end(), [&](const auto& a) { return a.command == item.wID; })) return false;
				const auto kind = item.wID == ids.next ? WorkflowActionKind::Next : item.wID == ids.previous ? WorkflowActionKind::Previous :
					item.wID == ids.change ? WorkflowActionKind::Change : WorkflowActionKind::Operation;
				auto label = Label(menu, i);
				if (label.empty()) return false;
				actions.push_back({item.wID, std::move(label), available, kind});
			}
			return true;
		}
	}

	bool ReadWorkflowMenu(HMENU menu, WorkflowMenuIds ids, bool actionable, WorkflowState& result)
	{
		result.actions.clear();
		result.rulesEngine = false;
		bool enabled = actionable;
		const HMENU branch = FindBranch(menu, ids, enabled);
		if (branch == nullptr || !Collect(branch, ids, enabled, result.actions)) { result.actions.clear(); return false; }
		result.rulesEngine = std::any_of(result.actions.begin(), result.actions.end(), [](const auto& a) { return a.kind == WorkflowActionKind::Operation; });
		if (result.rulesEngine)
			std::erase_if(result.actions, [](const auto& a) { return a.kind != WorkflowActionKind::Operation; });
		else
		{
			// Keep the requested Next / Previous / Change order, preserving native captions and permissions.
			std::stable_sort(result.actions.begin(), result.actions.end(), [](const auto& a, const auto& b) { return a.kind < b.kind; });
		}
		return !result.actions.empty();
	}

	bool CanInvokeWorkflow(const WorkflowState& shown, const WorkflowState& current, UINT command) noexcept
	{
		if (!shown.visible || !current.visible || shown.context.empty() || shown.context != current.context || shown.rulesEngine != current.rulesEngine) return false;
		const auto before = std::find_if(shown.actions.begin(), shown.actions.end(), [command](const auto& a) { return a.command == command; });
		const auto after = std::find_if(current.actions.begin(), current.actions.end(), [command](const auto& a) { return a.command == command; });
		return before != shown.actions.end() && after != current.actions.end() && before->enabled && after->enabled &&
			before->label == after->label && before->kind == after->kind;
	}
}
