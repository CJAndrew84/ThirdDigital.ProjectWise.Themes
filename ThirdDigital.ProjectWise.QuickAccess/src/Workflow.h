#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace QuickAccess
{
	enum class WorkflowActionKind { Next, Previous, Change, Operation, NewUser, Users, ProjectProperties, ProjectAccess };
	struct WorkflowAction
	{
		UINT command = 0;
		std::wstring label;
		bool enabled = false;
		WorkflowActionKind kind = WorkflowActionKind::Operation;
		bool operator==(const WorkflowAction&) const = default;
	};
	struct WorkflowState
	{
		bool visible = false;
		bool rulesEngine = false;
		std::wstring context;
		std::vector<WorkflowAction> actions;
		std::wstring status;
		bool operator==(const WorkflowState&) const = default;
	};
	struct WorkflowMenuIds { UINT next, previous, change, setFinal, removeFinal; };
	using ContextState = WorkflowState;
	using ContextAction = WorkflowAction;
	using ContextActionKind = WorkflowActionKind;
	constexpr UINT kFirstWorkflow = 1000;
	constexpr std::size_t kMaxWorkflowActions = 128;

	// Read only the native Change State branch, identified by command IDs rather than translated captions.
	bool ReadWorkflowMenu(HMENU menu, WorkflowMenuIds ids, bool actionable, WorkflowState& result);
	bool CanInvokeWorkflow(const WorkflowState& shown, const WorkflowState& current, UINT command) noexcept;
}
