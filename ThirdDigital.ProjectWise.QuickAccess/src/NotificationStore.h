#pragma once
#include "Notifications.h"

namespace QuickAccess::Notifications
{
	bool SaveBasket(const std::wstring& owner, const Batch& batch) noexcept;
	bool LoadBasket(const std::wstring& owner, const std::wstring& context, Batch& batch) noexcept;
	bool EraseBasket(const std::wstring& owner) noexcept;
}
