#include "NotificationStore.h"
#include <wincrypt.h>
#include <cstring>
#include <limits>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace QuickAccess::Notifications
{
	namespace
	{
		constexpr wchar_t kKey[] = L"Software\\AtkinsRealis\\ProjectWise\\UI\\NotificationBaskets";
		constexpr std::size_t kMaxBytes = 1024 * 1024;
		struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
		struct Data { DATA_BLOB value{}; ~Data() { if (value.pbData) LocalFree(value.pbData); } };
		void Number(std::vector<BYTE>& bytes, std::uint32_t value)
		{
			const auto offset = bytes.size(); bytes.resize(offset + sizeof(value)); std::memcpy(bytes.data() + offset, &value, sizeof(value));
		}
		bool Text(std::vector<BYTE>& bytes, const std::wstring& value)
		{
			if (value.size() > 8192) return false;
			Number(bytes, static_cast<std::uint32_t>(value.size()));
			const auto offset = bytes.size(); bytes.resize(offset + value.size() * sizeof(wchar_t));
			if (!value.empty()) std::memcpy(bytes.data() + offset, value.data(), value.size() * sizeof(wchar_t));
			return bytes.size() <= kMaxBytes;
		}
		bool Write(std::vector<BYTE>& bytes, const Document& document)
		{
			for (LONG number : {document.project, document.id, document.sequence, document.workflow, document.state}) Number(bytes, static_cast<std::uint32_t>(number));
			return Text(bytes, document.guid) && Text(bytes, document.name) && Text(bytes, document.version) && Text(bytes, document.workflowName) && Text(bytes, document.stateName) && Text(bytes, document.link);
		}
		struct Reader
		{
			const BYTE* data; std::size_t size, position = 0;
			bool Number(std::uint32_t& value)
			{
				if (position > size || sizeof(value) > size - position) return false;
				std::memcpy(&value, data + position, sizeof(value)); position += sizeof(value); return true;
			}
			bool Text(std::wstring& value)
			{
				std::uint32_t count = 0;
				if (!Number(count) || count > 8192 || count * sizeof(wchar_t) > size - position) return false;
				value.resize(count); if (count) std::memcpy(value.data(), data + position, count * sizeof(wchar_t));
				position += count * sizeof(wchar_t); return value.find(L'\0') == std::wstring::npos;
			}
			bool DocumentValue(Document& document)
			{
				LONG* fields[]{&document.project, &document.id, &document.sequence, &document.workflow, &document.state};
				for (auto* field : fields) { std::uint32_t value = 0; if (!Number(value) || value > static_cast<std::uint32_t>(MAXLONG)) return false; *field = static_cast<LONG>(value); }
				return document.project > 0 && document.id > 0 && Text(document.guid) && !document.guid.empty() && Text(document.name) && Text(document.version) &&
					Text(document.workflowName) && Text(document.stateName) && Text(document.link);
			}
		};
		DATA_BLOB Entropy(const std::wstring& owner)
		{
			return {static_cast<DWORD>(owner.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(owner.data()))};
		}
	}
	bool EraseBasket(const std::wstring& owner) noexcept
	{
		if (owner.empty() || owner.size() > 1024) return false;
		Key key;
		const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &key.value);
		if (opened == ERROR_FILE_NOT_FOUND) return true;
		if (opened != ERROR_SUCCESS) return false;
		const auto result = RegDeleteValueW(key.value, owner.c_str()); return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
	}
	bool SaveBasket(const std::wstring& owner, const Batch& batch) noexcept
	{
		try
		{
			if (owner.empty() || owner.size() > 1024 || batch.changes.size() > Basket::kMaxItems) return false;
			if (batch.changes.empty()) return EraseBasket(owner);
			std::vector<BYTE> bytes; Number(bytes, 0x31424E50); // PNB1, explicit storage version
			if (!Text(bytes, owner) || !Text(bytes, batch.datasource) || !Text(bytes, batch.initiator) || !Text(bytes, batch.note)) return false;
			Number(bytes, static_cast<std::uint32_t>(batch.linkApp));
			Number(bytes, static_cast<std::uint32_t>(batch.changes.size()));
			for (const auto& change : batch.changes) if (!SameDocument(change.before, change.after) || !Write(bytes, change.before) || !Write(bytes, change.after) || !Text(bytes, change.observedAt)) return false;
			DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()}; auto entropy = Entropy(owner); Data encrypted;
			if (!CryptProtectData(&input, L"ProjectWise notification basket", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted.value)) return false;
			Key key;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value, nullptr) != ERROR_SUCCESS) return false;
			return RegSetValueExW(key.value, owner.c_str(), 0, REG_BINARY, encrypted.value.pbData, encrypted.value.cbData) == ERROR_SUCCESS;
		}
		catch (...) { return false; }
	}
	bool LoadBasket(const std::wstring& owner, const std::wstring& context, Batch& batch) noexcept
	{
		batch = {};
		try
		{
			if (owner.empty() || owner.size() > 1024 || context.empty()) return false;
			Key key; const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_QUERY_VALUE, &key.value);
			if (opened == ERROR_FILE_NOT_FOUND) return true;
			if (opened != ERROR_SUCCESS) return false;
			DWORD type = 0, count = 0;
			const auto found = RegQueryValueExW(key.value, owner.c_str(), nullptr, &type, nullptr, &count);
			if (found == ERROR_FILE_NOT_FOUND) return true;
			if (found != ERROR_SUCCESS || type != REG_BINARY || count == 0 || count > kMaxBytes + 16384) return false;
			std::vector<BYTE> bytes(count);
			if (RegQueryValueExW(key.value, owner.c_str(), nullptr, &type, bytes.data(), &count) != ERROR_SUCCESS || type != REG_BINARY || count > bytes.size()) return false;
			DATA_BLOB input{count, bytes.data()}; auto entropy = Entropy(owner); Data plain;
			if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.value) || plain.value.cbData > kMaxBytes) return false;
			Reader reader{plain.value.pbData, plain.value.cbData}; std::uint32_t version = 0, items = 0, app = 0; std::wstring storedOwner; Batch restored;
			if (!reader.Number(version) || version != 0x31424E50 || !reader.Text(storedOwner) || storedOwner != owner || !reader.Text(restored.datasource) ||
				!reader.Text(restored.initiator) || !reader.Text(restored.note) || !reader.Number(app) || app > 3 || !reader.Number(items) || items > Basket::kMaxItems) return false;
			restored.linkApp = static_cast<LinkApp>(app);
			for (std::uint32_t i = 0; i < items; ++i)
			{
				Change change;
				if (!reader.DocumentValue(change.before) || !reader.DocumentValue(change.after) || !reader.Text(change.observedAt) || !SameDocument(change.before, change.after)) return false;
				restored.changes.push_back(std::move(change));
			}
			if (reader.position != reader.size) return false;
			restored.context = context; batch = std::move(restored); return true;
		}
		catch (...) { batch = {}; return false; }
	}
}
