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
// same way stmt.h only forward-declares Decl for DeclStmt::_decls - see stmt.h's header comment for
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

	// The storage-class specifier a declaration was written with. `const` is NOT here: it is a type
	// QUALIFIER, so it lives on the Type (Type::isConst()) and travels with the type through
	// pointers and arrays, which a per-declaration flag could not do.
	//
	// `inline` is deliberately not in this enum either, even though §3's grammar lists it next to
	// the others: real C lets it combine with them (`static inline`), so FunctionDecl carries it as
	// its own flag rather than as a fifth mutually-exclusive value.
	enum class StorageClass : u8
	{
		None,    // the default: external linkage at file scope, automatic storage in a block
		Static,  // internal linkage at file scope; at block scope, storage that outlives the call
		Extern,  // a declaration, not a definition: defined by another translation unit or in CASM
		Auto,    // explicit automatic storage. Legal only in a block, where it is also the default
		Register // request register residency; taking its address is forbidden
	};

	constexpr std::string_view storageClassName(StorageClass storageClass) noexcept
	{
		switch (storageClass)
		{
			case StorageClass::Static: return "static";
			case StorageClass::Extern: return "extern";
			case StorageClass::Auto:   return "auto";
			case StorageClass::Register: return "register";
			case StorageClass::None:   break;
		}
		return "";
	}

	class VarDecl final : public Decl
	{
	private:
		const Type* _type;
		Expr* _initializer; // nullable - `int x;` has none
		StorageClass _storageClass = StorageClass::None;

	public:
		VarDecl(support::SourceLocation location, std::string_view name, const Type* type, Expr* initializer = nullptr,
			StorageClass storageClass = StorageClass::None) noexcept :
			Decl(location, name), _type(type), _initializer(initializer), _storageClass(storageClass)
		{}

	public:
		const Type* type() const noexcept { return _type; }
		Expr* initializer() const noexcept { return _initializer; }
		StorageClass storageClass() const noexcept { return _storageClass; }

		// True for a declaration that reserves no storage of its own: `extern int x;` names
		// something another unit defines. `extern int x = 1;` is a definition despite the keyword,
		// which is exactly why this asks about the initializer too.
		bool isExternDeclaration() const noexcept { return _storageClass == StorageClass::Extern && !_initializer; }

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
		bool _isUnion = false;

	public:
		StructDecl(support::SourceLocation location, std::string_view name, bool isUnion = false) noexcept : Decl(location, name), _isUnion(isUnion) {}

	public:
		std::span<const FieldDecl> fields() const noexcept { return { _fields, _fieldCount }; }
		bool isComplete() const noexcept { return _complete; }
		bool isUnion() const noexcept { return _isUnion; }

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

	// `__interrupt_vector(NUMBER, handler);` - points one interrupt number at one `__interrupt`
	// function. A declaration of its own rather than part of the handler's, which is the whole
	// design decision here: CeresASM keeps `interrupt N: label` separate from the label it names
	// (26-Interrupt-Vector-Binding.md), and keeping the two apart is what lets a library publish a
	// handler while the application picks the vector - or lets a hand-written .casm bind a handler
	// written in C, and the other way round.
	//
	// Its `name()` is the handler's, which is also what makes it a Decl rather than a plain record:
	// everything that walks the unit looking for a name finds this one too. It declares no object
	// and emits no code - only a binding the linker resolves and the loader applies before the
	// program's first instruction (docs/10-Interrupts.md).
	//
	// `number` stays an Expr because the vector may be written as any constant expression - a
	// literal, an enum constant, a macro. sema is what folds and range-checks it.
	class InterruptVectorDecl final : public Decl
	{
	private:
		Expr* _number;
		support::SourceLocation _numberLocation;
		i64 _resolvedNumber = 0; // filled in by sema once the expression folds

	public:
		InterruptVectorDecl(support::SourceLocation location, std::string_view handlerName, Expr* number,
			support::SourceLocation numberLocation) noexcept :
			Decl(location, handlerName), _number(number), _numberLocation(numberLocation)
		{}

	public:
		Expr* number() const noexcept { return _number; }
		support::SourceLocation numberLocation() const noexcept { return _numberLocation; }
		i64 resolvedNumber() const noexcept { return _resolvedNumber; }
		void setResolvedNumber(i64 value) noexcept { _resolvedNumber = value; }

		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<InterruptVectorDecl>, "InterruptVectorDecl must be trivially destructible (Arena-allocated)");

	// `_Static_assert(condition, "message");` - checked by sema, and nothing else ever sees it: it emits no code and
	// no data. It is a declaration so that it can stand wherever one can: at file scope, in a block, and (moved to
	// just after the struct that holds it) in a struct body. The message is optional, as in C23.
	class StaticAssertDecl final : public Decl
	{
	private:
		Expr* _condition;
		support::PooledString _message;
		bool _hasMessage;

	public:
		StaticAssertDecl(support::SourceLocation location, Expr* condition, support::PooledString message, bool hasMessage) noexcept :
			Decl(location, {}), _condition(condition), _message(message), _hasMessage(hasMessage)
		{}

	public:
		Expr* condition() const noexcept { return _condition; }
		bool hasMessage() const noexcept { return _hasMessage; }
		support::PooledString message() const noexcept { return _message; }

		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<StaticAssertDecl>, "StaticAssertDecl must be trivially destructible (Arena-allocated)");

	// A function parameter's {type, name} pair - see the header comment above for why this is a
	// plain record, not a Decl.
	struct Param
	{
		const Type* type = nullptr;
		std::string_view name;
		support::SourceLocation location;
		// The program wrote `register` on this parameter. C allows exactly one storage-class
		// specifier inside a parameter list and this is it, for the same reason it allows it on a
		// local: it is a request, and the one promise it carries is that the address is never
		// taken. Not part of the parameter's TYPE, so two declarations of the same function need
		// not agree about it - which is why it does not travel in ast::Type's own param list.
		bool isRegister = false;
	};
	static_assert(TriviallyDestructible<Param>, "Param must be trivially destructible (Arena-allocated)");

	class FunctionDecl final : public Decl
	{
	private:
		const Type* _returnType;
		Param const* _params; // non-owning view over an arena-allocated array - see the header comment above
		u32 _paramCount;
		CompoundStmt* _body; // nullable - see the header comment above
		StorageClass _storageClass = StorageClass::None;
		bool _isInline = false;
		bool _isVariadic = false;
		bool _isInterrupt = false;

	public:
		FunctionDecl(support::SourceLocation location, std::string_view name, const Type* returnType, std::span<const Param> params,
			CompoundStmt* body = nullptr, StorageClass storageClass = StorageClass::None, bool isInline = false,
			bool isVariadic = false, bool isInterrupt = false) noexcept :
			Decl(location, name), _returnType(returnType), _params(params.data()), _paramCount(static_cast<u32>(params.size())),
			_body(body), _storageClass(storageClass), _isInline(isInline), _isVariadic(isVariadic), _isInterrupt(isInterrupt)
		{}

	public:
		const Type* returnType() const noexcept { return _returnType; }
		std::span<const Param> params() const noexcept { return { _params, _paramCount }; }
		CompoundStmt* body() const noexcept { return _body; }
		bool isDefinition() const noexcept { return _body != nullptr; }
		StorageClass storageClass() const noexcept { return _storageClass; }

		// `inline` on a definition. Here it is a request the optimizer honours rather than a
		// linkage rule: the function is still emitted, and the inliner treats it as worth inlining
		// regardless of its size whenever inlining is on at all (libs/ir/ir_optimizer.cpp).
		bool isInline() const noexcept { return _isInline; }

		// A `...` closed the parameter list: this function accepts arguments beyond params(), and
		// those extra arguments follow the variadic half of the calling convention rather than the
		// ordinary one (docs/09-Variadic-Convention.md). params() still describes exactly the FIXED
		// parameters, so every arity/type rule that reads it keeps meaning what it always meant -
		// what changes is only that `args.size() == params().size()` stops being the whole story.
		bool isVariadic() const noexcept { return _isVariadic; }

		// Declared `__interrupt`: this function is not called, it is DISPATCHED TO, by the machine,
		// at a point the surrounding code never chose. That changes its whole contract with the
		// world - it takes no arguments and returns nothing (nobody is there to pass or read one),
		// it must leave every register exactly as it found it (the hardware saves only the flags and
		// the PC), and it ends in `iret` rather than `ret`. `__interrupt_vector` is what points a
		// vector at one; the two are separate for the same reason CeresASM keeps them separate, so
		// that a handler and the number it answers can live in different files.
		// See docs/10-Interrupts.md.
		bool isInterruptHandler() const noexcept { return _isInterrupt; }

		// True when the function has external linkage - i.e. `global` in the generated CASM, and
		// therefore visible to another object at link time. `static` is the only thing that takes
		// that away; `extern` is what every function has by default anyway.
		bool hasExternalLinkage() const noexcept { return _storageClass != StorageClass::Static; }

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
