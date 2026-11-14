#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace QuickAccess::VersionStacks
{
	// These are copies of rows already visible in Explorer, never the result of an unrestricted version query.
	struct Row
	{
		LONG project = 0, document = 0, original = 0, sequence = 0;
		int sourceIndex = -1, image = -1;
		UINT stateImages = 0;
		bool groupable = false, historical = false, selected = false, focused = false;
		std::wstring identity; // opaque native identity; names are never used as identity
		std::vector<std::wstring> cells;
	};
	using Family = std::pair<LONG, LONG>;
	struct Group
	{
		Family family;
		std::vector<std::size_t> rows;
	};
	struct Entry
	{
		std::size_t row = 0, group = 0;
		bool parent = false;
	};
	std::vector<Group> GroupRows(const std::vector<Row>& rows);
	std::vector<Entry> VisibleRows(const std::vector<Group>& groups, const std::set<Family>& expanded);

	struct Column { std::wstring label; int width = 120, format = LVCFMT_LEFT; };
	struct Snapshot
	{
		std::wstring context, message;
		std::vector<Column> columns;
		std::vector<int> columnOrder;
		std::vector<Row> rows;
		HIMAGELIST images = nullptr, stateImages = nullptr; // borrowed only during read; the view duplicates them
	};
	enum class ReadResult { Unchanged, Ready, Unavailable };
	struct Host
	{
		void* context = nullptr;
		ReadResult (*read)(void*, Snapshot&, bool force) = nullptr;
		// Host validates datasource/user/folder and every source-row identity before altering native selection.
		bool (*select)(void*, const Snapshot&, const std::vector<std::size_t>&, std::size_t focus) = nullptr;
		bool (*activate)(void*, const Snapshot&, std::size_t row) = nullptr;
		bool (*menu)(void*, const Snapshot&, POINT screen) = nullptr;
		void (*classic)(void*) = nullptr;
		void (*destroy)(void*) = nullptr;
	};
	inline constexpr int kList = 310, kExpand = 311, kCollapse = 312, kClassic = 313;
	// Takes ownership of Host.context through Host.destroy, including on creation failure.
	HWND Create(HWND parent, HINSTANCE instance, const Host& host) noexcept;
	bool Refresh(HWND window, bool force = false) noexcept;
}
