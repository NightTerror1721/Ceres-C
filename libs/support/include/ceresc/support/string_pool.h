#pragma once

#include "arena.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <compare>

namespace ceresc::support
{
	class PooledString;

	class StringPool
	{
	private:
		friend class PooledString;
		static constexpr inline std::string_view EmptyStringView = std::string_view{};

	private:
		Arena _arena;
		std::unordered_map<std::string_view, std::string_view*> _strings;

	public:
		StringPool() = default;
		StringPool(const StringPool&) = delete;
		StringPool(StringPool&&) = delete;
		~StringPool() = default;

		StringPool& operator=(const StringPool&) = delete;
		StringPool& operator=(StringPool&&) = delete;

	public:
		// Returns an invalid PooledString (operator bool() == false) on allocation failure,
		// exactly like Arena::allocate()/create() - never throws.
		PooledString intern(std::string_view str);

	public:
		PooledString intern(std::string&& str);
		PooledString intern(const std::string& str);
		PooledString intern(const char* str);
	};

	class PooledString
	{
	public:
		friend class StringPool;

	private:
		const std::string_view* _data = nullptr;

	public:
		constexpr PooledString() noexcept = default;
		constexpr PooledString(const PooledString&) noexcept = default;
		constexpr PooledString(PooledString&&) noexcept = default;
		constexpr ~PooledString() noexcept = default;

		constexpr PooledString& operator=(const PooledString&) noexcept = default;
		constexpr PooledString& operator=(PooledString&&) noexcept = default;

		constexpr bool operator==(const PooledString&) const noexcept = default;

	private:
		constexpr explicit PooledString(const std::string_view* data) noexcept : _data(data) {}

	public:
		forceinline constexpr const char* data() const noexcept { return _data ? _data->data() : nullptr; }
		forceinline constexpr std::string_view view() const noexcept { return _data ? *_data : std::string_view{}; }

	public:
		forceinline constexpr explicit operator bool() const noexcept { return _data != nullptr; }
		forceinline constexpr bool operator!() const noexcept { return _data == nullptr; }

		forceinline constexpr operator std::string_view() const noexcept { return view(); }
	};

	inline PooledString StringPool::intern(std::string&& str)
	{
		return intern(std::string_view(str));
	}

	inline PooledString StringPool::intern(const std::string& str)
	{
		return intern(std::string_view(str));
	}

	inline PooledString StringPool::intern(const char* str)
	{
		return intern(std::string_view(str));
	}
}

template <>
struct std::hash<ceresc::support::PooledString>
{
	constexpr std::size_t operator()(const ceresc::support::PooledString& str) const noexcept
	{
		return std::hash<std::string_view>{}(str.view());
	}
};
