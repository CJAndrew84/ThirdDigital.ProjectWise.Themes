#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include "Workflow.h"

namespace QuickAccess
{
	constexpr std::size_t kShowAllVersions = 22;
	constexpr std::size_t kButtonCount = 23;
	constexpr UINT kFirstButton = 100;
	constexpr UINT kMaximise = 200;
	constexpr UINT kMinimise = 201;
	constexpr UINT kHide = 202;
	constexpr UINT kSearch = 203;
	constexpr UINT kSearchText = 204;
	constexpr UINT kCommandPage = 205;
	constexpr UINT kNavigationRow = 206;
	constexpr UINT kFirstNavigation = 210;
	constexpr std::size_t kNavigationCount = 3;

	enum class Tab { Home, View, Search, Tools, Workflow };
	enum class DisplayMode { Expanded, Minimized, Hidden };
	Tab ButtonTab(std::size_t button) noexcept;

	struct ButtonState
	{
		bool enabled = false;
		bool checked = false;
	};

	using States = std::array<ButtonState, kButtonCount>;
	using NavigationStates = std::array<ButtonState, kNavigationCount>;
	NavigationStates NavigationAvailability(bool historyEnabled, LONG count, LONG position, bool hasParent) noexcept;

	struct Host
	{
		void* context;
		bool (*refresh)(void* context, States& states);
		bool (*execute)(void* context, std::size_t button);
		bool (*search)(void* context, const wchar_t* text) = nullptr;
		HWND (*createAddress)(void* context, HWND parent) = nullptr;
		bool (*refreshNavigation)(void* context, HWND address, NavigationStates& states) = nullptr;
		bool (*navigate)(void* context, std::size_t button) = nullptr;
		bool (*focusContent)(void* context) = nullptr;
		bool (*refreshAddress)(void* context, HWND address) = nullptr;
		bool (*refreshWorkflow)(void* context, WorkflowState& workflow) = nullptr;
		bool (*executeWorkflow)(void* context, const WorkflowState& shown, UINT command) = nullptr;
	};

	bool ReadMenuState(HMENU menu, UINT command, ButtonState& state) noexcept;
	HWND SelectionControl(HWND owner, HWND tree, HWND list, HWND previous) noexcept;
	HWND Create(HWND owner, HINSTANCE instance, Host host) noexcept;
	bool Dock(HWND window, bool dock) noexcept;
	bool IsDocked(HWND window) noexcept;
	bool SetDisplayMode(HWND window, DisplayMode mode) noexcept;
	DisplayMode GetDisplayMode(HWND window) noexcept;
	bool Restore(HWND window) noexcept;
	bool SelectTab(HWND window, Tab tab, bool reveal = true) noexcept;
	Tab SelectedTab(HWND window) noexcept;
	bool IsPopupOpen(HWND window) noexcept;
	void DismissPopup(HWND window) noexcept;
	bool Search(HWND window) noexcept;
	bool Navigate(HWND window, UINT buttonId) noexcept;
	bool HandleKey(HWND window, UINT key, bool shift = false, bool control = false, bool alt = false) noexcept;
	double ContrastRatio(COLORREF first, COLORREF second) noexcept;
	COLORREF ContrastColour(COLORREF preferred, COLORREF background, double minimum) noexcept;
	void Refresh(HWND window) noexcept;
	bool Invoke(HWND window, UINT buttonId) noexcept;
	bool InvokeWorkflow(HWND window, UINT buttonId) noexcept;
}
