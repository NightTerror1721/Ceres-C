#pragma once

#include <ceresc/lexer/lexer.h>
#include <ceresc/ast/ast_visitor.h>
#include <ceresc/support/arena.h>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Parser - Token[] -> AST (§7 of the architecture plan).
//
// Recursive descent for declarations and statements; precedence climbing for expressions (levels
// 2-11 of the precedence table share one parametrized function, only the assignment, ternary, cast,
// and prefix/postfix levels get dedicated ones). A syntax error does not abort the file: panic-mode
// recovery resynchronizes on the next `;`/`}` (inside a statement, see synchronizeStatement()) or
// the next type-spec-start token (at top level, see synchronizeDeclaration()), so one pass reports
// every syntax error in a broken file, not just the first - see parseCompoundStatement() and
// parseTranslationUnit(), which are the two loops that call these after a failed inner parse
// instead of propagating nullptr straight up and giving up on the whole block/file.
//
// Expressions alone keep Fase 2's narrower, fail-fast behavior: a syntax error inside an expression
// reports a diagnostic and the failing parse* function returns nullptr, which propagates up through
// its caller instead of building on a missing piece - there is no mid-expression synchronization
// point to jump to (`;`/`}` belong to the statement enclosing the expression, not the expression
// itself). The one exception is a token that matches no production at all (parsePrimary()/
// parseTypeSpec()'s default case): that always consumes the bad token before returning nullptr, so
// at least one token of progress is guaranteed even inside an expression.
//
// Ternary (`cond ? then : else`) is its own dedicated function, parseTernary(), called from
// parseAssignment() in place of jumping straight to parseBinary(2) - see expr.h's TernaryExpr for
// why it sits there in the precedence chain.
//
// struct/enum/typedef declarations and switch/case/goto statements are covered here too. Three
// points worth knowing before touching this part of the file:
//
//  - struct/enum are parsed inline as part of the ordinary type-spec grammar (parseStructTypeSpec()/
//    parseEnumTypeSpec(), called from parseTypeSpec()), not as separate top-level productions - a
//    bare `struct Foo { ... };` declaration and a `struct Foo instance;` variable declaration both
//    go through parseTypeName() first, exactly like every other type-spec; parseExternalDecl()/
//    parseDeclStatement() only need one extra check afterward (is the type-spec a struct/enum AND
//    is the very next token ';'?) to tell the two forms apart. This also means the classic combined
//    idiom `struct Foo { int x; } instance;` and a bare forward declaration `struct Foo;` both work
//    for free, with no special-casing beyond that one check.
//  - Self-referential structs (`struct Node { struct Node* next; };`) work because the tag is
//    registered in _structTable the moment its name is read, before the field list is parsed - see
//    decl.h's own note on why StructDecl/EnumDecl are built in two steps (construct, then a later
//    setFields()/setEnumerators()) instead of one, unlike every other node in this project.
//  - typedef does not add a new kind of Type (see decl.h): parseTypedefDecl() just records the name
//    in _typedefTable, and isTypeSpecStart(const Token&)/parseTypeSpec() consult that table to treat
//    a typedef'd identifier as a type-spec from then on, exactly like a keyword. _structTable/
//    _enumTable/_typedefTable are flat, whole-Parser-lifetime maps with no scope stack - a local
//    struct/enum/typedef declared inside one function stays visible for the rest of the file. Real
//    C scopes these to their enclosing block; this parser does not enforce that (nor does it check
//    that goto's target label exists, or that case/default only appear inside a switch) - all of
//    that needs a proper symbol table, which is libs/sema's job, not this file's.
//
// case/default are ordinary labeled-statements exactly like real C (see stmt.h's own note): they
// wrap exactly the one statement that follows their ':', not "everything until the next case" -
// switch's fallthrough behavior falls out of its body being an ordinary CompoundStmt, nothing
// special is built for it here.
//
// Deliberately takes a Lexer&, not a pre-lexed Token[]: the grammar in §3 never needs more than one
// token of lookahead beyond the current one (used only to disambiguate `(type-name)` from a
// parenthesized expression, for cast-expr and sizeof - see parseCast()/parseSizeof()), so pulling
// tokens from the Lexer on demand, two at a time, is simpler than materializing the whole stream
// up front.
//
// Includes ast_visitor.h, not just expr.h: this file constructs concrete Expr subclasses directly
// (Arena::create<IntLiteralExpr>(...) and so on), and MSVC needs each class's accept() override to
// be a visible *definition* (not just expr.h's declaration) in this translation unit to emit its
// vtable correctly - GCC tolerates the declaration-only form here, MSVC does not. Verified by
// hitting the MSVC link error (unresolved accept()) with expr.h alone and fixing it this way.
//
// Also takes an Arena&, not a StringPool&: every AST node this file builds goes through
// Arena::create<T> (see libs/ast's own note on why every node type is trivially destructible), and
// nothing here needs to intern a string - NameExpr borrows the Token's own lexeme view, and
// StringLiteralExpr reuses the PooledString the Lexer already interned into the token's value.
//
// UnaryOp/BinaryOp/AssignOp translation from TokenKind happens here, not in libs/ast: ast does not
// depend on lexer (see the architecture plan's §1 dependency diagram), so this is the one place
// that is allowed to know both.
//
// Implemented in Fase 2 (expressions) and Fase 3 (declarations/statements, the ternary operator,
// struct/enum/typedef, switch/case/goto) of the phased plan (§13).

namespace ceresc::parser
{
	using ast::Expr;
	using ast::Type;
	using ast::UnaryOp;
	using ast::BinaryOp;
	using ast::AssignOp;
	using ast::Stmt;
	using ast::CompoundStmt;
	using ast::Decl;
	using ast::Param;
	using ast::FieldDecl;
	using ast::EnumeratorDecl;
	using ast::StructDecl;
	using ast::EnumDecl;
	using ast::TranslationUnit;
	using lexer::Token;
	using lexer::TokenKind;

	class Parser
	{
	private:
		lexer::Lexer& _lexer;
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;
		Token _current;
		Token _next; // one token of lookahead beyond _current - see the header comment above

		// Flat, whole-Parser-lifetime symbol tables for struct/enum tags and typedef names - see the
		// header comment above for why these have no scope stack.
		std::unordered_map<std::string_view, StructDecl*> _structTable;
		std::unordered_map<std::string_view, EnumDecl*> _enumTable;
		std::unordered_map<std::string_view, const Type*> _typedefTable;

	public:
		Parser() = delete;
		Parser(const Parser&) = delete;
		Parser(Parser&&) = default;
		~Parser() = default;

		Parser& operator=(const Parser&) = delete;
		Parser& operator=(Parser&&) = delete;

	public:
		explicit Parser(lexer::Lexer& lexer, support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept;

	public:
		// Entry points. parseExpression()/parseTypeName() are Fase 2; parseTranslationUnit()/
		// parseExternalDecl()/parseStatement() are Fase 3 - see the header comment above for what
		// that now covers (struct/enum/typedef, switch/case/goto).
		Expr* parseExpression();
		const Type* parseTypeName();
		// parseTypeName() for a caller that has already read the declaration's specifiers and so has
		// consumed leading qualifiers of its own. It matters WHERE those land: they qualify the
		// base type, before any `*`, so `const char* p` is a pointer to const char - applying one to
		// the finished type instead would silently produce `char* const p`, a const pointer to
		// ordinary char, which is a different type and the opposite promise.
		//
		// `volatile` has to come through here for the same reason and not a weaker one: with
		// `volatile int* p` the qualifier is what keeps the accesses through `p` observable, and
		// putting it on the pointer instead moves the guarantee onto a local nothing needed it for
		// while leaving the device register it was written for unprotected.
		//
		// `restrict` is deliberately NOT in this list. It can only qualify a pointer, so the
		// finished type is the right place for it, and each caller applies it there itself.
		const Type* parseTypeName(bool leadingConst, bool leadingVolatile);

		TranslationUnit* parseTranslationUnit();
		// One top-level declaration, which may declare several objects - `int a, b, c;` returns all
		// three. Empty on failure (after reporting), like every other parse failure here.
		std::vector<Decl*> parseExternalDecl();
		Stmt* parseStatement();

		bool isAtEnd() const noexcept { return _current.isEndOfFile(); }

	private:
		// Precedence table (§7), lowest to highest:
		//   1: parseAssignment (right-assoc)     1.5: parseTernary (right-assoc)
		//   2-11: parseBinary (left-assoc, one table)
		//  12: parseCast (right-assoc)            13: parseUnary (right-assoc, prefix + sizeof)
		//  14: parsePostfix (left-assoc, chained) -> parsePrimary
		Expr* parseAssignment();
		Expr* parseTernary();
		Expr* parseBinary(int minPrecedence);
		Expr* parseCast();
		Expr* parseUnary();
		Expr* parseSizeof(support::SourceLocation location);
		Expr* parseAlignof(support::SourceLocation location);
		Expr* parsePostfix();
		Expr* parsePrimary();

		const Type* parseTypeSpec();

	private:
		// Statements (§7's grammar). parseStatement() dispatches on the current token; its default
		// case falls to a labeled-statement (Identifier followed by ':'), then a local declaration
		// (current token starts a type-spec), then finally parseExprStatement().
		Stmt* parseCompoundStatement();
		Stmt* parseIfStatement();
		Stmt* parseWhileStatement();
		Stmt* parseDoWhileStatement();
		Stmt* parseForStatement();
		Stmt* parseReturnStatement();
		Stmt* parseBreakStatement();
		Stmt* parseContinueStatement();
		Stmt* parseSwitchStatement();
		Stmt* parseCaseStatement();
		Stmt* parseDefaultStatement();
		Stmt* parseGotoStatement();
		Stmt* parseLabeledStatement(); // `identifier ':' statement` - a goto target
		Stmt* parseDeclStatement(); // a local VarDecl wrapped in a DeclStmt - also used directly by parseForStatement() for its init-clause
		Stmt* parseExprStatement();

	private:
		// What a declaration says about itself before its type-spec even starts: `static`, `extern`,
		// `auto`, `inline` and `const`, in any order, each at most once. Collected into one record
		// because C allows them in any order and in any combination the language itself permits
		// (`static inline`, `const static`) - a sequence of ifs at each declaration site could not
		// say "two storage classes" or "const twice" without repeating itself four times.
		//
		// `sawAny` rather than comparing against a default: `auto` is the default at block scope, so
		// "was `auto` written" and "is this automatic" are different questions, and only the first
		// one can be an error at file scope.
		struct DeclSpecifiers
		{
			ast::StorageClass storageClass = ast::StorageClass::None;
			bool isInline = false;
			bool isConst = false;
			bool isVolatile = false;
			bool isRestrict = false;
			bool isInterrupt = false;
			bool sawAny = false;
			support::SourceLocation location{};
		};

		// The type-spec and its qualifiers alone - everything before the declarator. Split out
		// because a `*` belongs to the declarator, not to the type it derives from.
		//
		// `outRegisterRequest`, when non-null, marks the one position in the grammar where C does
		// allow a storage-class specifier inside what is otherwise a type: a parameter declaration,
		// where `register` and nothing else is legal. It is null everywhere else, which is what
		// keeps `static` in a cast or a `sizeof` an error.
		const Type* parseBaseType(bool leadingConst, bool leadingVolatile, bool& outLeadingRestrict,
			support::SourceLocation& outSpecifierLocation, bool* outRegisterRequest = nullptr);

		// ---- declarators ------------------------------------------------------------------------
		//
		// C's declarator syntax reads outside-in but BUILDS the type inside-out, and the two orders
		// are not the same: `int *f(void)` is a function returning `int*`, while `int (*f)(void)` is
		// a pointer to a function returning `int`. The only difference is a pair of parentheses, and
		// nothing that walks the tokens left to right can tell them apart on its own.
		//
		// So a declarator is parsed into this little tree first and applied to its base type after,
		// by applyDeclarator(). The rule it implements is C's own, stated once:
		//
		//     `T D`         declares D as having a type derived from T
		//     `T *D`        -> in D, that type is "pointer to T"
		//     `T D[N]`      -> "array N of T"
		//     `T D(params)` -> "function(params) returning T"
		//     `T (D)`       -> the same as `T D`
		//
		// Peeling the OUTERMOST construct each time gives the order: the leading `*`s first (they are
		// outermost when no parentheses separate them), then the suffixes right to left, and finally
		// whatever the parentheses enclosed. Worked through on applyDeclarator() itself.
		struct PointerLevel
		{
			bool isConst = false;
			bool isVolatile = false;
			bool isRestrict = false;
		};

		// One `[N]` or `(params)` written after the direct-declarator.
		struct DeclaratorSuffix
		{
			bool isFunction = false;
			u32 arraySize = 0;
			bool hasArraySize = false;
			std::vector<Param> params;   // function only; a parameter's NAME lives here, not in the type
			bool isVariadic = false;
			support::SourceLocation location{};
		};

		struct Declarator
		{
			std::vector<PointerLevel> pointers;      // left to right as written
			std::vector<DeclaratorSuffix> suffixes;  // left to right as written; APPLIED in reverse
			std::unique_ptr<Declarator> nested;      // whatever `( ... )` enclosed, if anything
			std::string_view name;                   // empty for an abstract declarator
			support::SourceLocation nameLocation{};
			bool ok = true;
		};

		// Parses one declarator. `allowAbstract` permits the nameless form a cast or `sizeof` uses
		// (`int (*)(int)`); without it, a missing name is an error.
		Declarator parseDeclarator(bool allowAbstract);
		// The `[N]` / `(params)` run after a direct-declarator, shared by both forms.
		void parseDeclaratorSuffixes(Declarator& declarator);
		// Derives the declared type by applying `declarator` to `base` - see the comment above.
		// `isParameter` applies C's array-parameter decay to the outermost dimension.
		//
		// `outSignature`, when given, ends up pointing at the function suffix that produced the
		// final type, or null when the final type is not a function. That is not always the first
		// suffix written: in `int (*get(void))(int)` the list that makes `get` a function is the
		// `(void)` inside the parentheses, and `(int)` belongs to what it returns. Only the order
		// the derivations are APPLIED in knows which, so this records it as it goes.
		const Type* applyDeclarator(const Type* base, const Declarator& declarator, bool isParameter,
			const DeclaratorSuffix** outSignature = nullptr);

		// At a '(' in direct-declarator position, does it open a nested declarator rather than a
		// parameter list? `(*)`, `(f)` and `((...))` are declarators; `()` and `(int)` are lists.
		// This is the one genuine ambiguity in C's declarator grammar and this is how it is resolved.
		bool nestedDeclaratorFollows() const noexcept;

		// Consumes every leading storage-class specifier and type qualifier, reporting a duplicate
		// or a second storage class. Always returns - a declaration with a bad specifier still has a
		// type and a name worth parsing, same panic-mode philosophy as everywhere else here.
		DeclSpecifiers parseDeclSpecifiers();

		// Consumes one run of `const`/`volatile` in any order, folding them into flags the caller
		// already holds - see parseTypeName()'s definition for why all three qualifier positions
		// share this one loop.
		void parseQualifierRun(bool& isConst, bool& isVolatile);

		// Declarations. A variable and a function declaration share the same `type-name identifier`
		// prefix - parseExternalDecl() parses that prefix once, then branches on whether a '(' follows.
		// One declarator plus whatever follows it. Whether it declares a function or an object is
		// decided by the declarator itself, exactly as in C - see the definition.
		Decl* finishDeclarator(support::SourceLocation location, const Type* base, const DeclSpecifiers& specifiers,
			bool leadingRestrict, support::SourceLocation specifierLocation, bool* outIsFunctionDefinition = nullptr);

		// The comma-separated init-declarator-list a base type introduces: one Decl per declarator,
		// each with its own `*`/`[...]`/initializer - `int a, *b[3], c = 1;` is three declarations.
		// Consumes the terminating ';' (a function DEFINITION ends the list without one). Returns
		// false, after reporting, on a malformed declarator - the caller does panic recovery.
		bool parseInitDeclaratorList(support::SourceLocation location, const Type* base, const DeclSpecifiers& specifiers,
			bool leadingRestrict, support::SourceLocation specifierLocation, std::vector<Decl*>& outDecls);

		// `unsizedArray`: the declarator omitted the array's size - it is taken from the initializer here.
		Decl* finishVarDecl(support::SourceLocation location, std::string_view name, const Type* type,
			const DeclSpecifiers& specifiers, bool unsizedArray = false);

		// §3's `initializer ::= assignment-expr | "{" initializer-list "}"` - the ONE production
		// that can produce an InitListExpr (expr.h's own note on why that node lives in the Expr
		// hierarchy at all depends on this being the only place). Recursive, because an element of
		// an initializer-list is itself an `initializer`, which is what makes `{ { 1, 2 }, { 3, 4 } }`
		// parse. Returns null on a malformed list, after reporting; the caller is a declarator, which
		// already knows how to keep going without one.
		//
		// A trailing comma is accepted (`{ 1, 2, }`, and one field to a line is how a long list is written),
		// and an element of a brace list may name what it is for: `.field = v`, `[index] = v`, `.a[2].b = v`.
		// `{}` is still rejected - the production requires at least one element.
		Expr* parseInitializer();
		Expr* parseDesignatedInit();
		// The body or the `;` after a declarator that turned out to name a function. The parameter
		// list has already been read as part of that declarator - it is what made the type a
		// function type - so this only takes the pieces rather than parsing them again. Does not
		// consume the trailing ';' of a prototype: that belongs to the init-declarator list. Sets
		// `outIsDefinition` (when given) for a function that was followed by a body.
		Decl* finishFunctionDecl(support::SourceLocation location, std::string_view name, const Type* functionType,
			std::span<const Param> params, bool isVariadic, const DeclSpecifiers& specifiers,
			bool* outIsDefinition = nullptr);
		// Reads the parameter list between an already-consumed '(' and its ')'. `outIsVariadic` is
		// set when the list ended in `...`, which is never itself a Param: the ellipsis says that
		// arguments MAY follow the ones named here, so outParams keeps describing exactly the fixed
		// parameters and nothing else (ast::FunctionDecl::isVariadic()).
		bool parseParamList(std::vector<Param>& outParams, bool& outIsVariadic);
		// The typedef itself, preceded by any struct/union/enum whose body it defined on the way
		// (`typedef enum { A, B } T;`), or empty when it failed.
		std::vector<Decl*> parseTypedefDecl();

		// The variadic builtins (__builtin_va_start, __builtin_va_arg, __builtin_va_end and
		// __builtin_va_copy), recognized by name in call position rather than declared by a header -
		// this compiler has no system include directory to find a <stdarg.h> in, and
		// __builtin_va_arg's second operand is a type-name, which no ordinary call could express.
		// See docs/09-Variadic-Convention.md.
		static std::optional<ast::VaOp> vaBuiltinFor(std::string_view name) noexcept;
		Expr* parseVaBuiltin(support::SourceLocation location, ast::VaOp op);

		// The machine builtins (__builtin_sti/__builtin_cli/__builtin_halt), recognized the same way
		// and for the same reason: there is no header to declare them in, and nothing an ordinary
		// function could contain but the one instruction. See docs/10-Interrupts.md.
		// `__interrupt_vector(NUMBER, handler);` - a top-level declaration of its own, which is what
		// lets the handler and the number it answers live in different files. See decl.h's
		// InterruptVectorDecl and docs/10-Interrupts.md.
		Decl* parseInterruptVectorDecl();

		// `_Static_assert(condition[, "message"]);` - an identifier in the language's reserved namespace rather
		// than a keyword token, so the lexer needs no new kind for it. Recognized only when a '(' follows.
		bool isStaticAssertStart() const noexcept;
		Decl* parseStaticAssert();

		// `__asm__("label")` after a declarator: the name the assembler is to use for the symbol. Recognized
		// (`__asm__` or `__asm`, followed by a '(') the way _Static_assert is, without a token kind of its own.
		bool isAsmLabelStart() const noexcept;
		bool parseAsmLabel(support::PooledString& out);

		// The function whose body is being parsed, for `__func__`; empty outside one.
		std::string_view _currentFunctionName;

		static std::optional<ast::MachineOp> machineBuiltinFor(std::string_view name) noexcept;
		Expr* parseMachineBuiltin(support::SourceLocation location, ast::MachineOp op);


		// `long long`, `unsigned long long`, `double` and `long double` all name a width this
		// machine does not have: there is no 64-bit register and no f64 register anywhere in Ceres.
		// They are accepted as SPELLINGS of the 32-bit type they cap to (type.h) rather than
		// rejected, because a program that uses one is asking for a wide number and gets a number -
		// but never silently, because it is not the number it asked for. `written` is what the
		// program said, `actual` what it got.
		const Type* cappedToMachineWidth(support::SourceLocation location, std::string_view written,
			std::string_view actual, const Type* type);

		// struct/enum are parsed as part of the type-spec grammar, not as their own top-level
		// productions - see the header comment above.
		const Type* parseStructTypeSpec(bool isUnion = false);
		const Type* parseEnumTypeSpec();

		// `struct { ... }`, `union { ... }` and `enum { ... }` with no tag get one made up: the tables
		// below are keyed by tag, and nothing else about the type needs a name. The text lives in the
		// arena because the tables and the AST hold string_views.
		std::string_view makeAnonymousTag(std::string_view kind);
		unsigned _anonymousTagCount = 0;

		// Every struct/union/enum whose BODY the type-spec parser just read, in the order the bodies
		// closed (an inner one before the one that contains it). A declaration that defines a tag as
		// part of something else - `typedef enum { A, B } T;`, `enum { X, Y } v;`, `struct { ... } s;` -
		// takes these and emits them ahead of itself, because that is what makes sema declare the
		// enumerators and check the layout. A bare `enum E { ... };` already emits its own tag.
		std::vector<Decl*> _definedTags;
		std::vector<Decl*> takeDefinedTags();

		// `int a[] = { 1, 2, 3 };` and `char s[] = "hi";` - an array whose outermost size is left out and
		// taken from its initializer. Only a VARIABLE declarator may do it: finishDeclarator() raises
		// _allowUnsizedArray around its applyDeclarator() call, which then builds the array with size 0 and
		// sets _unsizedArrayPending; finishVarDecl() replaces it with the real length once the initializer
		// has been read. Everywhere else (a struct member, a typedef, a cast) the omitted size is still an error.
		bool _allowUnsizedArray = false;
		bool _unsizedArrayPending = false;
		// The number of elements an initializer gives an array of `element`, or -1 when it cannot say.
		static i64 inferArrayLength(const Type* element, const Expr* initializer);

	private:
		Token advance() noexcept;
		bool check(TokenKind kind) const noexcept { return _current.is(kind); }
		bool match(TokenKind kind) noexcept;
		bool expect(TokenKind kind, std::string_view what) noexcept;

		// Panic-mode recovery (see the header comment above): called after a sub-parse inside a
		// block/file fails, so the enclosing loop can skip the broken construct and keep going
		// instead of aborting the whole block/file on one bad statement/declaration.
		void synchronizeStatement() noexcept;
		void synchronizeDeclaration() noexcept;

		Expr* wrapUnary(UnaryOp op, support::SourceLocation location, Expr* operand) noexcept;
		// Shared by the two nodes that own a variable-length Expr list: CallExpr's arguments and
		// InitListExpr's elements (expr.h) - both need the same {pointer, count} arena copy.
		std::span<Expr* const> copyArgsToArena(const std::vector<Expr*>& args) noexcept;
		std::span<Stmt* const> copyStmtsToArena(const std::vector<Stmt*>& stmts) noexcept;
		std::span<Decl* const> copyDeclsToArena(const std::vector<Decl*>& decls) noexcept;
		std::span<const Param> copyParamsToArena(std::span<const Param> params) noexcept;
		std::span<const FieldDecl> copyFieldsToArena(const std::vector<FieldDecl>& fields) noexcept;
		std::span<const EnumeratorDecl> copyEnumeratorsToArena(const std::vector<EnumeratorDecl>& enumerators) noexcept;

	private:
		// True for the tokens that can, by their kind ALONE, start a type-name (primitives +
		// signed/unsigned/short/long combinations, plus struct/enum - see parseStructTypeSpec()/
		// parseEnumTypeSpec()). This overload cannot recognize a typedef'd identifier - that needs
		// the actual lexeme, not just the TokenKind - so it exists mainly for parseTypeSpec()'s own
		// switch (which needs a TokenKind to switch over) and as the building block for the
		// token-aware overload below. Prefer isTypeSpecStart(const Token&) everywhere else.
		// True for the tokens that can begin a declaration WITHOUT being a type-spec: the storage
		// classes and `const`. Kept apart from isTypeSpecStart() because they are not types - they
		// are what comes before one - but a statement that starts with any of them is still a
		// declaration, and so is a top-level one.
		static constexpr bool isDeclSpecifierStart(TokenKind kind) noexcept
		{
			switch (kind)
			{
				case TokenKind::KwConst:
				case TokenKind::KwVolatile:
				case TokenKind::KwRestrict:
				case TokenKind::KwStatic:
				case TokenKind::KwExtern:
				case TokenKind::KwAuto:
				case TokenKind::KwRegister:
				case TokenKind::KwInline:
				case TokenKind::KwInterrupt:
					return true;
				default:
					return false;
			}
		}

		static constexpr bool isTypeSpecStart(TokenKind kind) noexcept
		{
			switch (kind)
			{
				case TokenKind::KwVoid:
				case TokenKind::KwBool:
				case TokenKind::KwFloat:
				case TokenKind::KwDouble: // a spelling of `float` here - see cappedToMachineWidth()
				case TokenKind::KwChar:
				case TokenKind::KwShort:
				case TokenKind::KwInt:
				case TokenKind::KwLong:
				case TokenKind::KwSigned:
				case TokenKind::KwUnsigned:
				case TokenKind::KwStruct:
				case TokenKind::KwUnion:
				case TokenKind::KwEnum:
					return true;
				default:
					return false;
			}
		}

		// Typedef-aware: also true for an Identifier token whose lexeme is a registered typedef name
		// (see _typedefTable and parseTypedefDecl()). Used to disambiguate `(type-name)` from a
		// parenthesized expression with exactly one token of lookahead past `(` (cast-expr and
		// sizeof); to tell parseStatement() a local declaration apart from an expression/labeled
		// statement; and to tell synchronizeDeclaration() where the next top-level declaration
		// starts. Not static, unlike the TokenKind overload above, since it must consult
		// _typedefTable.
		bool isTypeSpecStart(const Token& token) const noexcept
		{
			if (isTypeSpecStart(token.kind()))
				return true;
			return token.kind() == TokenKind::Identifier && _typedefTable.contains(token.lexeme());
		}

		static const std::unordered_map<TokenKind, std::pair<int, BinaryOp>>& binaryOpTable();
		static const std::unordered_map<TokenKind, AssignOp>& assignOpTable();
	};
}
