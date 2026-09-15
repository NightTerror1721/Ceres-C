#pragma once

#include "expr.h"
#include "type.h"
#include <span>
#include <string_view>

// Decl hierarchy: VarDecl, FunctionDecl, StructDecl, EnumDecl, TypedefDecl. Plus Param and
// FieldDecl/EnumeratorDecl (plain data records, not Decls - see below) and TranslationUnit (the
// parse root, also not a Decl - see below).
//
// VarDecl covers both a global variable and a local one declared inside a block: the same node
// either way, wrapped in a DeclStmt (stmt.h) when it appears in statement position. There is no
// separate GlobalVarDecl/LocalVarDecl split - scope is a sema concern, not a parse-time one.
//
// Param is deliberately NOT a Decl: a function parameter is a {type, name} pair the parser records
// while reading a parameter list, not something anyone visits independently through AstVisitor -
// same reasoning as CallExpr's argument list in expr.h. FunctionDecl stores its parameters as a
// non-owning {pointer, count} view over an arena-allocated array of Param values (mirrors
// CompoundStmt::_stmts in stmt.h), not a std::vector. FieldDecl (a struct member) and
// EnumeratorDecl (an enum constant) are the same idea applied to StructDecl/EnumDecl.
//
// TranslationUnit is also not a Decl: it has no name and isn't itself declared by the C program,
// it's this library's own name for "the parsed file" - the root AstVisitor::visit() lands on. It
// has no subclasses, so unlike Decl its accept() is a plain (non-virtual) method.
//
// FunctionDecl::body() is nullable: a null body means a prototype (`int foo(int x);`), a non-null
// one means a definition. Both forms share this one node kind - sema is what checks that a function
// is eventually defined somewhere, not the parser.
//
// StructDecl/EnumDecl are constructed in two steps, unlike every other node in this hierarchy:
// StructDecl(location, name) alone, then a later setFields() call once the body has been parsed
// (same for EnumDecl/setEnumerators()). This is required for self-referential structs
// (`struct Node { struct Node* next; };`): the parser registers the tag the moment it sees the
// name, before parsing the body, so a field that mentions the same tag - always through a pointer,
// same as real C - resolves to the very StructDecl instance being built. isComplete() is false
// until setFields() runs, which is also how a plain forward declaration (`struct Node;`, no body -
// legal C for an opaque/incomplete type) stays representable: the tag exists in the table, but
// nothing has completed it yet. Both classes still satisfy TriviallyDestructible/Arena::create<T> -
// setFields()/setEnumerators() are plain setters, not dynamic allocation on the class's own part
// (Expr::setType() is the same idiom already, just called by the parser instead of later by sema).
//
// TypedefDecl does not introduce a new TypeKind - Type has no "alias" concept, `typedef` just gives
// an existing const Type* a second name. libs/parser is what actually makes that name usable as a
// type-spec afterward (a name -> const Type* table it consults), this class only records that the
// typedef happened, for the AST/--emit-ast's sake.
//
// This file only forward-declares Stmt/CompoundStmt (FunctionDecl::_body is a bare pointer), the
// same way stmt.h only forward-declares Decl for DeclStmt::_decl - see stmt.h's header comment for
// why an actual mutual #include between the two files would be a real cycle, and why AstVisitor
// (ast_visitor.h) is the one place that includes both fully.
//
// Implemented in Fase 3 of the phased plan (§13). See the architecture plan, §6 (AST structure) and
// §7 (grammar).

namespace ceresc::ast
{
	class Stmt;			// forward declaration only - see the header comment above
	class CompoundStmt;	// forward declaration only - see the header comment above

	class Decl
	{
	protected:
		support::SourceLocation _location;
		std::string_view _name; // a raw view into the source buffer, same as NameExpr::_name in expr.h

	public:
		Decl(support::SourceLocation location, std::string_view name) noexcept : _location(location), _name(name) {}

	public:
		support::SourceLocation location() const noexcept { return _location; }
		std::string_view name() const noexcept { return _name; }

		virtual void accept(AstVisitor& visitor) = 0;
	};
	static_assert(TriviallyDestructible<Decl>, "Decl must be trivially destructible because it is allocated in an arena and never deleted individually");

	class VarDecl final : public Decl
	{
	private:
		const Type* _type;
		Expr* _initializer; // nullable - `int x;` has none

	public:
		VarDecl(support::SourceLocation location, std::string_view name, const Type* type, Expr* initializer = nullptr) noexcept :
			Decl(location, name), _type(type), _initializer(initializer)
		{}

	public:
		const Type* type() const noexcept { return _type; }
		Expr* initializer() const noexcept { return _initializer; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<VarDecl>, "VarDecl must be trivially destructible (Arena-allocated)");

	// A struct member's {type, name} pair - see the header comment above for why this is a plain
	// record, not a Decl.
	struct FieldDecl
	{
		const Type* type = nullptr;
		std::string_view name;
		support::SourceLocation location;
	};
	static_assert(TriviallyDestructible<FieldDecl>, "FieldDecl must be trivially destructible (Arena-allocated)");

	class StructDecl final : public Decl
	{
	private:
		FieldDecl const* _fields = nullptr; // non-owning view over an arena-allocated array - see the header comment above
		u32 _fieldCount = 0;
		bool _complete = false; // false until setFields() runs - see the header comment above

	public:
		StructDecl(support::SourceLocation location, std::string_view name) noexcept : Decl(location, name) {}

	public:
		std::span<const FieldDecl> fields() const noexcept { return { _fields, _fieldCount }; }
		bool isComplete() const noexcept { return _complete; }

		void setFields(std::span<const FieldDecl> fields) noexcept
		{
			_fields = fields.data();
			_fieldCount = static_cast<u32>(fields.size());
			_complete = true;
		}

		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<StructDecl>, "StructDecl must be trivially destructible (Arena-allocated)");

	// An enum constant's {name, optional explicit value} pair - see the header comment above for why
	// this is a plain record, not a Decl. `value` is null for an implicitly auto-incremented
	// constant (`enum { A, B }`) - assigning the actual integer values is sema's job, not the
	// parser's, same as everywhere else this project defers constant evaluation.
	struct EnumeratorDecl
	{
		std::string_view name;
		Expr* value = nullptr; // nullable
		support::SourceLocation location;
	};
	static_assert(TriviallyDestructible<EnumeratorDecl>, "EnumeratorDecl must be trivially destructible (Arena-allocated)");

	class EnumDecl final : public Decl
	{
	private:
		EnumeratorDecl const* _enumerators = nullptr; // non-owning view over an arena-allocated array
		u32 _enumeratorCount = 0;
		bool _complete = false; // false until setEnumerators() runs - see the header comment above

	public:
		EnumDecl(support::SourceLocation location, std::string_view name) noexcept : Decl(location, name) {}

	public:
		std::span<const EnumeratorDecl> enumerators() const noexcept { return { _enumerators, _enumeratorCount }; }
		bool isComplete() const noexcept { return _complete; }

		void setEnumerators(std::span<const EnumeratorDecl> enumerators) noexcept
		{
			_enumerators = enumerators.data();
			_enumeratorCount = static_cast<u32>(enumerators.size());
			_complete = true;
		}

		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<EnumDecl>, "EnumDecl must be trivially destructible (Arena-allocated)");

	class TypedefDecl final : public Decl
	{
	private:
		const Type* _underlyingType;

	public:
		TypedefDecl(support::SourceLocation location, std::string_view name, const Type* underlyingType) noexcept :
			Decl(location, name), _underlyingType(underlyingType)
		{}

	public:
		const Type* underlyingType() const noexcept { return _underlyingType; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<TypedefDecl>, "TypedefDecl must be trivially destructible (Arena-allocated)");

	// A function parameter's {type, name} pair - see the header comment above for why this is a
	// plain record, not a Decl.
	struct Param
	{
		const Type* type = nullptr;
		std::string_view name;
		support::SourceLocation location;
	};
	static_assert(TriviallyDestructible<Param>, "Param must be trivially destructible (Arena-allocated)");

	class FunctionDecl final : public Decl
	{
	private:
		const Type* _returnType;
		Param const* _params; // non-owning view over an arena-allocated array - see the header comment above
		u32 _paramCount;
		CompoundStmt* _body; // nullable - see the header comment above

	public:
		FunctionDecl(support::SourceLocation location, std::string_view name, const Type* returnType, std::span<const Param> params, CompoundStmt* body = nullptr) noexcept :
			Decl(location, name), _returnType(returnType), _params(params.data()), _paramCount(static_cast<u32>(params.size())), _body(body)
		{}

	public:
		const Type* returnType() const noexcept { return _returnType; }
		std::span<const Param> params() const noexcept { return { _params, _paramCount }; }
		CompoundStmt* body() const noexcept { return _body; }
		bool isDefinition() const noexcept { return _body != nullptr; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<FunctionDecl>, "FunctionDecl must be trivially destructible (Arena-allocated)");

	// The root of a parsed file: a flat list of top-level declarations. Not a Decl itself - see the
	// header comment above.
	class TranslationUnit final
	{
	private:
		support::SourceLocation _location;
		Decl* const* _decls; // non-owning view over an arena-allocated array, same pattern as FunctionDecl::_params
		u32 _declCount;

	public:
		TranslationUnit(support::SourceLocation location, std::span<Decl* const> decls) noexcept :
			_location(location), _decls(decls.data()), _declCount(static_cast<u32>(decls.size()))
		{}

	public:
		support::SourceLocation location() const noexcept { return _location; }
		std::span<Decl* const> decls() const noexcept { return { _decls, _declCount }; }

		void accept(AstVisitor& visitor); // not virtual: TranslationUnit has no subclasses, unlike Decl
	};
	static_assert(TriviallyDestructible<TranslationUnit>, "TranslationUnit must be trivially destructible (Arena-allocated)");
}
