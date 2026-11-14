#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace QuickAccess::Notifications
{
	using Time = std::uint64_t;
	struct Document
	{
		LONG project = 0, id = 0, sequence = 0, workflow = 0, state = 0;
		std::wstring guid, name, version, workflowName, stateName, link;
	};
	bool SameDocument(const Document& a, const Document& b) noexcept;
	bool SameState(const Document& a, const Document& b) noexcept;
	struct Change { Document before, after; };
	struct Batch
	{
		std::wstring context, datasource, initiator;
		std::vector<Change> changes;
	};
	struct Probe { std::uint64_t token = 0; Document before; };
	class Watcher
	{
	public:
		static constexpr std::size_t kMaxDocuments = 64, kMaxPending = 128;
		std::uint64_t Begin(const std::wstring& context, const std::wstring& datasource, const std::wstring& initiator,
			const std::vector<Document>& before, Time now);
		void Complete(std::uint64_t token, Time now);
		void Rebase(std::uint64_t token, const std::vector<Document>& before);
		std::vector<Probe> Due(const std::wstring& context, Time now, std::size_t budget = 8);
		void Observe(std::uint64_t token, const Document& before, const Document* after, Time now);
		std::vector<Batch> TakeReady(const std::wstring& context, Time now);
		void Clear() noexcept { pending_.clear(); }
		std::size_t Pending() const noexcept;
	private:
		struct Candidate
		{
			Document before, last;
			Time next = 0, stableSince = 0;
			unsigned delay = 1500, failures = 0;
			bool changed = false, confirmed = false, rebased = false;
		};
		struct PendingBatch
		{
			std::uint64_t token = 0;
			Batch info;
			std::vector<Candidate> documents;
			Time expires = 0, lastChange = 0;
			bool complete = false;
		};
		std::vector<PendingBatch> pending_;
		std::uint64_t serial_ = 0;
	};
	enum class Channel { Teams, Email };
	std::wstring Message(const Batch& batch);
	bool ComposeLink(Channel channel, const std::wstring& recipients, const std::wstring& subject, const std::wstring& message,
		std::wstring& uri, std::wstring& error);
	struct Composer
	{
		void* context = nullptr;
		bool (*open)(void*, const std::wstring& uri, std::wstring& error) = nullptr;
		bool (*copy)(void*, const std::wstring& message, std::wstring& error) = nullptr;
		bool (*valid)(void*, const std::wstring& context) = nullptr;
	};
	INT_PTR ShowDialog(HWND owner, HINSTANCE instance, const Batch& batch, Composer composer);
}
