#pragma once

#include <ceresc/lexer/lexer.h>
#include <ceresc/ast/ast_visitor.h>
#include <ceresc/support/arena.h>
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

		TranslationUnit* parseTranslationUnit();
		Decl* parseExternalDecl();
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
		// Declarations. A variable and a function declaration share the same `type-name identifier`
		// prefix - parseExternalDecl() parses that prefix once, then branches on whether a '(' follows.
		Decl* finishVarDecl(support::SourceLocation location, std::string_view name, const Type* type);

		// §3's `initializer ::= assignment-expr | "{" initializer-list "}"` - the ONE production
		// that can produce an InitListExpr (expr.h's own note on why that node lives in the Expr
		// hierarchy at all depends on this being the only place). Recursive, because an element of
		// an initializer-list is itself an `initializer`, which is what makes `{ { 1, 2 }, { 3, 4 } }`
		// parse. Returns null on a malformed list, after reporting; the caller is a declarator, which
		// already knows how to keep going without one.
		//
		// No trailing comma: the grammar of §3 is `initializer ("," initializer)*`, and §3 is
		// explicit that it is the contract "ni más ni menos". `{}` is rejected for the same reason -
		// the production requires at least one element.
		Expr* parseInitializer();
		Decl* finishFunctionDecl(support::SourceLocation location, std::string_view name, const Type* returnType);
		bool parseParamList(std::vector<Param>& outParams);
		Decl* parseTypedefDecl();

		// direct-declarator's "[" INT_LITERAL? "]")* suffix (§7's grammar) - called after an
		// identifier at every declarator site (local/global VarDecl, struct field, typedef, param)
		// once `elementType` (everything parseTypeName() already built, i.e. the base type plus any
		// leading `*`s) and the name are both known. Builds nested Array types outermost-first
		// (`int m[3][4]` -> Array(Array(int, 4), 3), matching real C), so it must see every `[...]`
		// group before constructing any single Type.
		//
		// `isParameter` selects real C's own special case for a parameter declarator: an array
		// dimension there is not a real array, it decays to a pointer immediately (`void f(int a[10])`
		// means `void f(int* a)`, the 10 is documentation only - real C ignores it) - and, precisely
		// because it decays, the outermost `[...]` may be empty there (`int a[]`, `int m[][4]`) with
		// no diagnostic. Everywhere else (a plain variable, a struct field, a typedef) there is no
		// decay, and a missing size is never inferred from a brace initializer in this subset (§3's
		// `direct-declarator` requires an INT_LITERAL, and `int a[] = { 1, 2 }` therefore is not a
		// form this grammar accepts, even though parseInitializer() understands the right-hand side
		// perfectly well), so every dimension must
		// carry an INT_LITERAL or this reports a diagnostic and treats that dimension as size 1 to
		// keep building a usable (if wrong) type instead of returning null and losing the declarator
		// entirely - consistent with this parser's panic-mode philosophy of never letting one bad
		// piece take down a whole declaration it doesn't have to (see the header comment above).
		//
		// Only called once `check(TokenKind::LBracket)` is already true - see call sites.
		const Type* parseArrayDeclaratorSuffix(const Type* elementType, bool isParameter);

		// struct/enum are parsed as part of the type-spec grammar, not as their own top-level
		// productions - see the header comment above.
		const Type* parseStructTypeSpec();
		const Type* parseEnumTypeSpec();

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
		std::span<const Param> copyParamsToArena(const std::vector<Param>& params) noexcept;
		std::span<const FieldDecl> copyFieldsToArena(const std::vector<FieldDecl>& fields) noexcept;
		std::span<const EnumeratorDecl> copyEnumeratorsToArena(const std::vector<EnumeratorDecl>& enumerators) noexcept;

	private:
		// True for the tokens that can, by their kind ALONE, start a type-name (primitives +
		// signed/unsigned/short/long combinations, plus struct/enum - see parseStructTypeSpec()/
		// parseEnumTypeSpec()). This overload cannot recognize a typedef'd identifier - that needs
		// the actual lexeme, not just the TokenKind - so it exists mainly for parseTypeSpec()'s own
		// switch (which needs a TokenKind to switch over) and as the building block for the
		// token-aware overload below. Prefer isTypeSpecStart(const Token&) everywhere else.
		static constexpr bool isTypeSpecStart(TokenKind kind) noexcept
		{
			switch (kind)
			{
				case TokenKind::KwVoid:
				case TokenKind::KwBool:
				case TokenKind::KwFloat:
				case TokenKind::KwChar:
				case TokenKind::KwShort:
				case TokenKind::KwInt:
				case TokenKind::KwLong:
				case TokenKind::KwSigned:
				case TokenKind::KwUnsigned:
				case TokenKind::KwStruct:
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
