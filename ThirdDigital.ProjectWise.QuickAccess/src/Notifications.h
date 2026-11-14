#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

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
	struct Change { Document before, after; std::wstring observedAt; };
	enum class LinkApp { Choose = 0, Explorer = 1, Web = 2, WebView = 3 };
	struct Batch
	{
		std::wstring context, datasource, initiator;
		std::vector<Change> changes;
		std::wstring note;
		LinkApp linkApp = LinkApp::Choose;
	};
	class Basket
	{
	public:
		static constexpr std::size_t kMaxItems = 250;
		bool Add(const Batch& batch);
		void Remove(std::size_t index);
		void Clear() noexcept { batch_ = {}; }
		void Replace(Batch batch) { batch_ = std::move(batch); }
		const Batch& Items() const noexcept { return batch_; }
	private:
		Batch batch_;
	};
	bool IsHttpsLink(const std::wstring& value) noexcept;
	std::wstring ModernDocumentLink(const std::wstring& nativeLink, const std::wstring& datasource, const std::wstring& documentGuid);
	std::wstring WithLinkApp(const std::wstring& link, LinkApp app);
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
	// Returns IDC_NOTIFY_PARK after choosing Add to basket; changes survive in batch for the host to store.
	INT_PTR ShowDialog(HWND owner, HINSTANCE instance, Batch& batch, Composer composer, bool basket = false);
}
