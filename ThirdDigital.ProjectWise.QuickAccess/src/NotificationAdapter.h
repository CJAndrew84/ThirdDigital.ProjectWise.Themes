#pragma once
#include <windows.h>
#include <cstdint>

namespace QuickAccess
{
	bool StartWorkflowNotifications(HINSTANCE instance, bool (*workflowCommand)(UINT)) noexcept;
	void SetWorkflowNotificationsAvailable(bool available) noexcept;
	std::uint64_t BeginWorkflowNotification(UINT command) noexcept;
	void EndWorkflowNotification(std::uint64_t token) noexcept;
}
