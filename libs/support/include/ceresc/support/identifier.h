#pragma once

#include "types.h"
#include <compare>

namespace ceresc::support
{
	template <UnsignedIntegral T>
	class GenericIdentifier
	{
	public:
		using ValueType = T;
		static inline constexpr ValueType InvalidValue = 0;
		static const GenericIdentifier Invalid;

	private:
		ValueType _value = 0;

	public:
		constexpr GenericIdentifier() noexcept = default;
		constexpr GenericIdentifier(const GenericIdentifier&) noexcept = default;
		constexpr GenericIdentifier(GenericIdentifier&&) noexcept = default;
		constexpr ~GenericIdentifier() noexcept = default;

		constexpr GenericIdentifier& operator=(const GenericIdentifier&) noexcept = default;
		constexpr GenericIdentifier& operator=(GenericIdentifier&&) noexcept = default;

		constexpr bool operator==(const GenericIdentifier&) const noexcept = default;
		constexpr auto operator<=>(const GenericIdentifier&) const noexcept = default;

	private:
		constexpr explicit GenericIdentifier(ValueType value) noexcept : _value(value) {}

	public:
		constexpr ValueType value() const noexcept { return _value; }

	public:
		constexpr explicit operator bool() const noexcept { return _value != InvalidValue; }
		constexpr bool operator!() const noexcept { return _value == InvalidValue; }

		constexpr explicit operator ValueType() const noexcept { return _value; }

	public:
		static constexpr GenericIdentifier make(ValueType value) noexcept { return GenericIdentifier(value); }
		static constexpr GenericIdentifier invalid() noexcept { return Invalid; }
	};

	template <UnsignedIntegral T>
	inline constexpr const GenericIdentifier<T> GenericIdentifier<T>::Invalid = GenericIdentifier<T>(GenericIdentifier<T>::InvalidValue);


	template <UnsignedIntegral T>
	class GenericIdentifierGenerator
	{
	public:
		using IdentifierType = GenericIdentifier<T>;
		using ValueType = typename IdentifierType::ValueType;
		static inline constexpr ValueType InvalidValue = IdentifierType::InvalidValue;

	private:
		ValueType _nextValue = IdentifierType::InvalidValue + 1;

	public:
		constexpr GenericIdentifierGenerator() noexcept = default;
		constexpr GenericIdentifierGenerator(const GenericIdentifierGenerator&) noexcept = default;
		constexpr GenericIdentifierGenerator(GenericIdentifierGenerator&&) noexcept = default;
		constexpr ~GenericIdentifierGenerator() noexcept = default;

		constexpr GenericIdentifierGenerator& operator=(const GenericIdentifierGenerator&) noexcept = default;
		constexpr GenericIdentifierGenerator& operator=(GenericIdentifierGenerator&&) noexcept = default;

	public:
		constexpr IdentifierType generate() noexcept { return IdentifierType::make(_nextValue++); }
		constexpr void reset() noexcept { _nextValue = IdentifierType::InvalidValue + 1; }

	public:
		constexpr IdentifierType operator()() noexcept { return generate(); }
	};
}

template <ceresc::UnsignedIntegral T>
struct std::hash<ceresc::support::GenericIdentifier<T>>
{
	constexpr std::size_t operator()(const ceresc::support::GenericIdentifier<T>& id) const noexcept
	{
		return static_cast<std::size_t>(id.value());
	}
};
