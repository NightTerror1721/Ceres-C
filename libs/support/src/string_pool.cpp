#include <ceresc/support/string_pool.h>

#include <cstring>
#include <limits>

namespace ceresc::support
{
	PooledString StringPool::intern(std::string_view str)
	{
		if (str.empty())
			return PooledString(&EmptyStringView);

		if (auto it = _strings.find(str); it != _strings.end())
			return PooledString(it->second);

		// size() == max() would overflow the "+ 1" below - treat it the same as any other
		// allocation failure rather than let it wrap around to a 0-byte request.
		if (str.size() == (std::numeric_limits<usize>::max)())
			return PooledString{};

		// Allocate a copy of the string in the arena. Arena::allocate()/create() report exhaustion
		// by returning nullptr, not by throwing (see arena.h) - intern() follows the same
		// convention instead of introducing the only throwing path in the codebase: a failed
		// intern() is an invalid PooledString (operator bool() == false), for the caller to check
		// exactly like any other arena allocation.
		usize size = str.size();
		char* buffer = static_cast<char*>(_arena.allocate(size + 1, alignof(char)));
		if (buffer == nullptr)
			return PooledString{};

		std::memcpy(buffer, str.data(), size);
		buffer[size] = '\0';
		std::string_view* interned = _arena.create<std::string_view>(buffer, size);
		if (interned == nullptr)
			return PooledString{};

		_strings.emplace(*interned, interned);
		return PooledString(interned);
	}
}
