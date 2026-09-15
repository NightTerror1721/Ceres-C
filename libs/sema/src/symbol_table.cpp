#include <ceresc/sema/symbol_table.h>

namespace ceresc::sema
{
	bool Scope::declare(const Symbol& symbol)
	{
		return _symbols.try_emplace(symbol.name, symbol).second;
	}

	Symbol* Scope::lookup(std::string_view name) noexcept
	{
		for (Scope* scope = this; scope != nullptr; scope = scope->_parent)
		{
			auto it = scope->_symbols.find(name);
			if (it != scope->_symbols.end())
				return &it->second;
		}
		return nullptr;
	}

	Symbol* Scope::lookupInThisScope(std::string_view name) noexcept
	{
		auto it = _symbols.find(name);
		return it != _symbols.end() ? &it->second : nullptr;
	}
}
