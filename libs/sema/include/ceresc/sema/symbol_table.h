#pragma once

#include <ceresc/ast/type.h>
#include <ceresc/support/source_location.h>
#include <ceresc/support/types.h>
#include <string_view>
#include <unordered_map>

// Symbol, SymbolKind and Scope - a chain of lexical scopes (global -> function -> block, nested
// per `{ }`), each an unordered_map<string_view, Symbol> with a pointer to its parent.
//
// This is the properly-scoped symbol table libs/parser's own _structTable/_enumTable/_typedefTable
// (parser.h) deliberately are not: those three stay flat for the whole file on purpose (struct/
// enum/typedef names, resolved once at parse time into the AST itself - see decl.h/type.h), while
// this table exists to resolve *value*-level identifiers (variables, parameters, functions, enum
// constants) with real block scoping, which the parser never attempted. Sema (sema.h) owns a stack
// of these, pushing one on every CompoundStmt/function body/for-init and popping it on the way out.
//
// A Scope is not Arena-allocated: unlike every class in libs/ast, it is not part of the AST and
// does not need to outlive a single Sema::check() call, so it is an ordinary heap object (Sema
// keeps it alive via std::unique_ptr while it's on the scope stack - see sema.h) with a ordinary
// non-trivial destructor, unlike the TriviallyDestructible discipline arena.h documents.
//
// See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::ast
{
	class VarDecl;
	class FunctionDecl;
}

namespace ceresc::sema
{
	enum class SymbolKind : u8
	{
		Variable,	 // a VarDecl - global or local, see decl.h's own note on why there's one kind for both
		Parameter,	 // a function Param (see decl.h - not a Decl, so it has no node of its own to point back to)
		Function,	 // a FunctionDecl - always file-scope in this subset, no nested functions
		EnumConstant // an enumerator - typed `int`, same as real C (see decl.h's EnumeratorDecl)
	};

	struct Symbol
	{
		SymbolKind kind = SymbolKind::Variable;
		std::string_view name;
		const ast::Type* type = nullptr;
		support::SourceLocation location = {};

		bool isGlobal = false;					 // true when declared directly in the translation unit's own scope
		ast::VarDecl* varDecl = nullptr;		 // set for Variable
		ast::FunctionDecl* funcDecl = nullptr;	 // set for Function
		i64 enumConstantValue = 0;				 // set for EnumConstant
	};

	class Scope
	{
	private:
		Scope* _parent;
		std::unordered_map<std::string_view, Symbol> _symbols;

	public:
		explicit Scope(Scope* parent = nullptr) noexcept : _parent(parent) {}
		Scope(const Scope&) = delete;
		Scope(Scope&&) = delete;
		~Scope() = default;

		Scope& operator=(const Scope&) = delete;
		Scope& operator=(Scope&&) = delete;

	public:
		Scope* parent() const noexcept { return _parent; }

		// Declares `symbol` in this scope. Fails (returns false, leaves the scope unchanged) if
		// `symbol.name` is already declared in THIS scope - real C allows shadowing a name from an
		// outer scope but not redeclaring one already in the same scope.
		bool declare(const Symbol& symbol);

		// Looks up `name` in this scope, then its parent chain. Returns nullptr if not found
		// anywhere.
		Symbol* lookup(std::string_view name) noexcept;

		// Looks up `name` in this scope only, no parent chain - used to detect redeclaration
		// before calling declare().
		Symbol* lookupInThisScope(std::string_view name) noexcept;
	};
}
