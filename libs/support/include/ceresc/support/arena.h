#pragma once

#include "types.h"
#include <memory>
#include <algorithm>
#include <vector>
#include <type_traits>
#include <utility>
#include <limits>

// Arena - a bump allocator over 64 KiB blocks.
//
// Every AST node and every IR instruction is allocated here, never with an individual `new`. Its
// lifetime is the whole translation unit: it is torn down all at once when a file finishes
// compiling, so no AST/IR node needs its own destructor. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13).

namespace ceresc::support
{
	class Arena;

	class ArenaBlock
	{
	public:
		friend class Arena;
		using Byte = std::byte;
		static inline constexpr usize BlockSize = 64 * 1024; // 64 KiB

	private:
		usize _used = 0;
		usize _capacity;
		std::unique_ptr<Byte[]> _data;

	public:
		ArenaBlock(usize size = BlockSize) : _capacity(std::max(size, BlockSize)), _data(std::make_unique<Byte[]>(_capacity)) {}
		ArenaBlock(const ArenaBlock&) = delete;
		ArenaBlock(ArenaBlock&&) = default;
		~ArenaBlock() = default;

		ArenaBlock& operator=(const ArenaBlock&) = delete;
		ArenaBlock& operator=(ArenaBlock&&) = default;

	private:
		static constexpr bool isPowerOfTwo(usize x)
		{
			return x != 0 && (x & (x - 1)) == 0;
		}

		static constexpr bool tryAdd(usize a, usize b, usize& out)
		{
			if (a > (std::numeric_limits<usize>::max)() - b)
				return false;

			out = a + b;
			return true;
		}

		static constexpr bool tryAlignUp(usize value, usize alignment, usize& out)
		{
			if (!isPowerOfTwo(alignment))
				return false;

			const usize mask = alignment - 1;
			usize sum = 0;
			if (!tryAdd(value, mask, sum))
				return false;

			out = sum & ~mask;
			return true;
		}

	private:
		[[nodiscard]] void* allocate(usize size, usize alignment = alignof(std::max_align_t))
		{
			usize start = 0;
			if (!tryAlignUp(_used, alignment, start))
				return nullptr;

			if (start > _capacity || size > _capacity - start)
				return nullptr;

			Byte* ptr = _data.get() + start;
			_used = start + size;
			return ptr;
		}

		[[nodiscard]] bool hasSpace(usize size, usize alignment = alignof(std::max_align_t)) const
		{
			usize start = 0;
			if (!tryAlignUp(_used, alignment, start))
				return false;

			return start <= _capacity && size <= _capacity - start;
		}
	};

	class Arena
	{
	public:
		using Block = ArenaBlock;
		using Byte = Block::Byte;

	private:
		std::vector<Block> _blocks;

	public:
		Arena() = default;
		Arena(const Arena&) = delete;
		Arena(Arena&&) = default;
		~Arena() = default;

		Arena& operator=(const Arena&) = delete;
		Arena& operator=(Arena&&) = default;

	public:
		[[nodiscard]] void* allocate(usize size, usize alignment = alignof(std::max_align_t))
		{
			if (alignment == 0 || alignment > alignof(std::max_align_t))
				return nullptr;

			usize extra = 0;
			if (!Block::tryAdd(size, alignment - 1, extra))
				return nullptr;

			if (_blocks.empty() || !_blocks.back().hasSpace(size, alignment))
				_blocks.emplace_back(std::max(extra, Block::BlockSize));

			return _blocks.back().allocate(size, alignment);
		}

		template <TriviallyDestructible T, typename... Args>
		[[nodiscard]] T* create(Args&&... args)
		{
			static_assert(!std::is_array_v<T>, "Arena::create<T> not supported for array types.");
			static_assert(alignof(T) <= alignof(std::max_align_t), "Arena::create<T> not supported for types with alignment greater than alignof(std::max_align_t).");
			void* ptr = allocate(sizeof(T), alignof(T));
			if (ptr == nullptr)
				return nullptr;
			return std::construct_at(static_cast<T*>(ptr), std::forward<Args>(args)...);
		}
	};
}
