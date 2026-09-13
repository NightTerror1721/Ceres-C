#include <ceresc/support/arena.h>

#include "framework.h"

#include <cstdint>
#include <vector>

using namespace ceresc;
using namespace ceresc::support;

namespace
{
	struct Point
	{
		int x;
		int y;
	};

	// The largest alignment Arena::create<T> is required to support is alignof(std::max_align_t)
	// itself (see the static_assert in arena.h) - and that value genuinely differs between MSVC
	// and GCC/Clang (long double's ABI differs), so the test must ask for exactly that value
	// rather than hardcoding 16.
	struct alignas(alignof(std::max_align_t)) MaxAligned
	{
		char tag;
		double payload;
	};
}

TEST(arena, create_returns_non_null_and_constructs_the_value)
{
	Arena arena;
	Point* p = arena.create<Point>(3, 4);
	CHECK(p != nullptr);
	CHECK_EQ(p->x, 3);
	CHECK_EQ(p->y, 4);
}

TEST(arena, create_default_constructs_when_no_args_are_given)
{
	Arena arena;
	int* value = arena.create<int>();
	CHECK(value != nullptr);
	*value = 99;
	CHECK_EQ(*value, 99);
}

TEST(arena, sequential_allocations_do_not_overlap)
{
	Arena arena;
	int* a = arena.create<int>(1);
	int* b = arena.create<int>(2);
	CHECK(a != nullptr);
	CHECK(b != nullptr);
	CHECK(a != b);
	CHECK_EQ(*a, 1);
	CHECK_EQ(*b, 2); // writing through b must not have clobbered a
}

TEST(arena, allocations_are_aligned_to_the_requested_type)
{
	Arena arena;
	MaxAligned* value = arena.create<MaxAligned>();
	CHECK(value != nullptr);
	CHECK_EQ(reinterpret_cast<std::uintptr_t>(value) % alignof(MaxAligned), std::uintptr_t{ 0 });
}

TEST(arena, pointers_stay_valid_after_the_arena_grows_a_new_block)
{
	Arena arena;

	struct Chunk
	{
		std::byte bytes[64];
		int tag;
	};

	// ~2000 chunks of 64+ bytes each is well over two 64 KiB blocks - guarantees at least one
	// block boundary is crossed, which is exactly the scenario where a naively-stored block
	// (one whose buffer could move on reallocation) would invalidate earlier pointers.
	constexpr int count = 2000;
	std::vector<Chunk*> chunks;
	chunks.reserve(static_cast<usize>(count));

	for (int i = 0; i < count; ++i)
	{
		Chunk* c = arena.create<Chunk>();
		CHECK(c != nullptr);
		c->tag = i;
		chunks.push_back(c);
	}

	for (int i = 0; i < count; ++i)
		CHECK_EQ(chunks[static_cast<usize>(i)]->tag, i);
}

TEST(arena, oversized_allocation_bigger_than_a_block_is_fully_usable)
{
	Arena arena;
	constexpr usize hugeSize = ArenaBlock::BlockSize + 1024;
	void* raw = arena.allocate(hugeSize);
	CHECK(raw != nullptr);

	auto* bytes = static_cast<std::byte*>(raw);
	bytes[0] = std::byte{ 0xAB };
	bytes[hugeSize - 1] = std::byte{ 0xCD };
	CHECK(bytes[0] == std::byte{ 0xAB });
	CHECK(bytes[hugeSize - 1] == std::byte{ 0xCD });
}
