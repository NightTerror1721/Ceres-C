#pragma once

// Sized integer aliases shared across the whole tree.
//
// Not part of the file list in §2 of the architecture plan, but implied by it: every code sample
// from §4 onward (Token, IrInstr, Type, ...) uses u8/u16/u32/u64/i8.../usize without ever defining
// them. Ceres-C is a standalone project and does not link against Ceres's own
// libs/core/include/ceres/core/base/types.h, so it needs its own copy of the same small set of
// aliases - this is that copy.

#include <cstdint>
#include <cstddef>
#include <concepts>
#include <ranges>
#include <optional>
#include <expected>

namespace ceresc
{
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;

	using i8 = std::int8_t;
	using i16 = std::int16_t;
	using i32 = std::int32_t;
	using i64 = std::int64_t;

	using f32 = float;
	using f64 = double;

	using usize = std::size_t;
	using isize = std::make_signed_t<std::size_t>;

	using uoffset = std::size_t;
	using ioffset = std::make_signed_t<std::size_t>;

	template <typename T>
	using Ref = std::reference_wrapper<T>;

	template <typename T>
	using ConstRef = std::reference_wrapper<const T>;

	template <typename T>
	using Optional = std::optional<T>;

	template <typename T>
	using OptionalRef = std::optional<std::reference_wrapper<T>>;

	template <typename T>
	using OptionalConstRef = std::optional<std::reference_wrapper<const T>>;

	template <typename T, typename E>
	using Expected = std::expected<T, E>;

	template <typename T, typename E>
	using ExpectedRef = std::expected<std::reference_wrapper<T>, E>;

	template <typename T, typename E>
	using ExpectedConstRef = std::expected<std::reference_wrapper<const T>, E>;
}

namespace ceresc
{
	template <typename T>
	concept Integral = std::integral<T>;

	template <typename T>
	concept SignedIntegral = std::signed_integral<T>;

	template <typename T>
	concept UnsignedIntegral = std::unsigned_integral<T>;

	template <typename T>
	concept FloatingPoint = std::floating_point<T>;

	template <typename T>
	concept Arithmetic = Integral<T> || FloatingPoint<T>;

	template <typename T, typename U>
	concept SameAs = std::same_as<T, U>;

	template <typename Derived, typename Base>
	concept DerivedFrom = std::derived_from<Derived, Base>;

	template <typename Base, typename Derived>
	concept BaseOf = std::derived_from<Derived, Base>;

	template <typename T>
	concept DefaultConstructible = std::default_initializable<T>;

	template <typename T, typename... Args>
	concept ConstructibleFrom = std::constructible_from<T, Args...>;

	template <typename T>
	concept Destructible = std::destructible<T>;

	template <typename T>
	concept CopyConstructible = std::copy_constructible<T>;

	template <typename T>
	concept MoveConstructible = std::move_constructible<T>;

	template <typename T>
	concept Range = std::ranges::range<T>;

	template <typename T>
	concept View = std::ranges::view<T>;

	template <typename T>
	concept BorrowedRange = std::ranges::borrowed_range<T>;

	template <typename T>
	concept SizedRange = std::ranges::sized_range<T>;

	template <typename T>
	concept CommonRange = std::ranges::common_range<T>;

	template <typename T>
	concept OutputRange = std::ranges::output_range<T, typename T::value_type>;

	template <typename T>
	concept InputRange = std::ranges::input_range<T>;

	template <typename T>
	concept ForwardRange = std::ranges::forward_range<T>;

	template <typename T>
	concept BidirectionalRange = std::ranges::bidirectional_range<T>;

	template <typename T>
	concept RandomAccessRange = std::ranges::random_access_range<T>;

	template <typename T>
	concept ContiguousRange = std::ranges::contiguous_range<T>;

	template <typename T, typename U>
	concept RangeOf = std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, U>;

	template <typename T, typename U>
	concept ViewOf = std::ranges::view<T> && std::same_as<std::ranges::range_value_t<T>, U>;

	template <typename T, typename U>
	concept RangeDerivedFrom = std::ranges::range<T> && std::derived_from<std::ranges::range_value_t<T>, U>;

	template <typename T, typename U>
	concept ViewDerivedFrom = std::ranges::view<T> && std::derived_from<std::ranges::range_value_t<T>, U>;
}

#ifndef forceinline
#	ifdef _MSC_VER
#		define forceinline __forceinline
#	elif defined(__GNUC__) || defined(__clang__)
#		define forceinline inline __attribute__((always_inline))
#	else
#		define forceinline inline
#	endif
#endif
