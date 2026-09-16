#include <ceresc/parser/parser.h>

#include <memory>
#include <optional>

namespace ceresc::parser
{
	using support::SourceLocation;

	Parser::Parser(lexer::Lexer& lexer, support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept :
		_lexer(lexer), _arena(arena), _diagnostics(diagnostics), _current(lexer.next()), _next(lexer.next())
	{
		// `va_list` is a builtin type name rather than something a header declares: this compiler
		// has no system include directory to find a <stdarg.h> in, so the type and the four
		// operations on it are known to the compiler itself (docs/09-Variadic-Convention.md).
		// It is a `char*` because that is exactly what it holds - a cursor into the caller's frame,
		// advanced a byte count at a time by va_arg - and spelling it as an ordinary pointer means
		// assignment, copying and parameter passing all already work on it.
		_typedefTable.emplace("va_list", Type::makePointer(_arena, &Type::Char));
	}

	std::optional<ast::VaOp> Parser::vaBuiltinFor(std::string_view name) noexcept
	{
		if (name == "va_start") return ast::VaOp::Start;
		if (name == "va_arg")   return ast::VaOp::Arg;
		if (name == "va_end")   return ast::VaOp::End;
		if (name == "va_copy")  return ast::VaOp::Copy;
		return std::nullopt;
	}

	std::optional<ast::MachineOp> Parser::machineBuiltinFor(std::string_view name) noexcept
	{
		if (name == "__builtin_sti")  return ast::MachineOp::Sti;
		if (name == "__builtin_cli")  return ast::MachineOp::Cli;
		if (name == "__builtin_halt") return ast::MachineOp::Halt;
		return std::nullopt;
	}

	Expr* Parser::parseMachineBuiltin(SourceLocation location, ast::MachineOp op)
	{
		advance(); // the builtin's name
		if (!expect(TokenKind::LParen, "'(' after a machine builtin"))
			return nullptr;
		// None of the three takes an operand, so the argument list is empty or the program is wrong.
		if (!check(TokenKind::RParen))
		{
			_diagnostics.error(_current.location(), "'{}' takes no arguments", ast::machineOpName(op));
			return nullptr;
		}
		advance(); // ')'
		return _arena.create<ast::MachineOpExpr>(location, op);
	}

	Expr* Parser::parseVaBuiltin(SourceLocation location, ast::VaOp op)
	{
		advance(); // the builtin's name
		if (!expect(TokenKind::LParen, "'(' after a variadic builtin"))
			return nullptr;

		Expr* list = parseAssignment();
		if (!list)
			return nullptr;

		Expr* second = nullptr;
		const Type* argumentType = nullptr;
		if (op != ast::VaOp::End)
		{
			if (!expect(TokenKind::Comma, "','"))
				return nullptr;
			if (op == ast::VaOp::Arg)
			{
				// The one operand in this grammar that is a type rather than an expression, which
				// is the whole reason va_arg cannot be an ordinary function.
				argumentType = parseTypeName();
				if (!argumentType)
					return nullptr;
			}
			else
			{
				second = parseAssignment();
				if (!second)
					return nullptr;
			}
		}

		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		return _arena.create<ast::VaExpr>(location, op, list, second, argumentType);
	}

	Token Parser::advance() noexcept
	{
		Token previous = _current;
		_current = _next;
		_next = _lexer.next();
		return previous;
	}

	bool Parser::match(TokenKind kind) noexcept
	{
		if (!check(kind))
			return false;
		advance();
		return true;
	}

	bool Parser::expect(TokenKind kind, std::string_view what) noexcept
	{
		if (match(kind))
			return true;

		_diagnostics.error(_current.location(), "expected {} but found '{}'", what,
			_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
		return false;
	}

	Expr* Parser::wrapUnary(UnaryOp op, SourceLocation location, Expr* operand) noexcept
	{
		if (!operand)
			return nullptr;
		return _arena.create<ast::UnaryExpr>(location, op, operand);
	}

	std::span<Expr* const> Parser::copyArgsToArena(const std::vector<Expr*>& args) noexcept
	{
		if (args.empty())
			return {};

		void* memory = _arena.allocate(sizeof(Expr*) * args.size(), alignof(Expr*));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating an expression list");
			return {};
		}

		Expr** stored = static_cast<Expr**>(memory);
		for (usize i = 0; i < args.size(); ++i)
			stored[i] = args[i];
		return std::span<Expr* const>(stored, args.size());
	}

	std::span<Stmt* const> Parser::copyStmtsToArena(const std::vector<Stmt*>& stmts) noexcept
	{
		if (stmts.empty())
			return {};

		void* memory = _arena.allocate(sizeof(Stmt*) * stmts.size(), alignof(Stmt*));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating block statements");
			return {};
		}

		Stmt** stored = static_cast<Stmt**>(memory);
		for (usize i = 0; i < stmts.size(); ++i)
			stored[i] = stmts[i];
		return std::span<Stmt* const>(stored, stmts.size());
	}

	std::span<Decl* const> Parser::copyDeclsToArena(const std::vector<Decl*>& decls) noexcept
	{
		if (decls.empty())
			return {};

		void* memory = _arena.allocate(sizeof(Decl*) * decls.size(), alignof(Decl*));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating top-level declarations");
			return {};
		}

		Decl** stored = static_cast<Decl**>(memory);
		for (usize i = 0; i < decls.size(); ++i)
			stored[i] = decls[i];
		return std::span<Decl* const>(stored, decls.size());
	}

	std::span<const Param> Parser::copyParamsToArena(const std::vector<Param>& params) noexcept
	{
		if (params.empty())
			return {};

		void* memory = _arena.allocate(sizeof(Param) * params.size(), alignof(Param));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating function parameters");
			return {};
		}

		Param* stored = static_cast<Param*>(memory);
		for (usize i = 0; i < params.size(); ++i)
			std::construct_at(stored + i, params[i]);
		return std::span<const Param>(stored, params.size());
	}

	std::span<const FieldDecl> Parser::copyFieldsToArena(const std::vector<FieldDecl>& fields) noexcept
	{
		if (fields.empty())
			return {};

		void* memory = _arena.allocate(sizeof(FieldDecl) * fields.size(), alignof(FieldDecl));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating struct fields");
			return {};
		}

		FieldDecl* stored = static_cast<FieldDecl*>(memory);
		for (usize i = 0; i < fields.size(); ++i)
			std::construct_at(stored + i, fields[i]);
		return std::span<const FieldDecl>(stored, fields.size());
	}

	std::span<const EnumeratorDecl> Parser::copyEnumeratorsToArena(const std::vector<EnumeratorDecl>& enumerators) noexcept
	{
		if (enumerators.empty())
			return {};

		void* memory = _arena.allocate(sizeof(EnumeratorDecl) * enumerators.size(), alignof(EnumeratorDecl));
		if (!memory)
		{
			_diagnostics.error(_current.location(), "out of memory allocating enumerators");
			return {};
		}

		EnumeratorDecl* stored = static_cast<EnumeratorDecl*>(memory);
		for (usize i = 0; i < enumerators.size(); ++i)
			std::construct_at(stored + i, enumerators[i]);
		return std::span<const EnumeratorDecl>(stored, enumerators.size());
	}

	void Parser::synchronizeStatement() noexcept
	{
		// Skip until we consume a ';' (the end of the broken statement) or reach a '}' we don't
		// consume (the enclosing parseCompoundStatement() loop owns that one) or EOF.
		while (!isAtEnd() && !check(TokenKind::RBrace))
		{
			if (match(TokenKind::Semicolon))
				return;
			advance();
		}
	}

	void Parser::synchronizeDeclaration() noexcept
	{
		// Unlike synchronizeStatement(), there is no enclosing '}' at top level to hand a stray one
		// back to - so a '}' here is just skipped like any other token, and the only stopping
		// condition is the start of the next declaration (or EOF). Stopping on '}' instead would
		// loop forever on a stray '}' at top level, since nothing above parseTranslationUnit() would
		// ever consume it.
		while (!isAtEnd() && !isTypeSpecStart(_current) && !check(TokenKind::KwTypedef) &&
			!isDeclSpecifierStart(_current.kind()))
			advance();
	}

	// ---- Expressions (§7's precedence table) --------------------------------------------------

	Expr* Parser::parseExpression()
	{
		return parseAssignment();
	}

	Expr* Parser::parseAssignment()
	{
		Expr* lhs = parseTernary();

		const auto& table = assignOpTable();
		auto it = table.find(_current.kind());
		if (it == table.end())
			return lhs;
		if (!lhs)
			return nullptr;

		AssignOp op = it->second;
		SourceLocation location = _current.location();
		advance();

		Expr* value = parseAssignment(); // right-associative
		if (!value)
			return nullptr;

		return _arena.create<ast::AssignExpr>(location, op, lhs, value);
	}

	Expr* Parser::parseTernary()
	{
		SourceLocation location = _current.location();
		Expr* cond = parseBinary(2); // 2 = lowest binary level (||); climbs up through 11
		if (!cond)
			return nullptr;

		if (!match(TokenKind::Question))
			return cond;

		Expr* thenExpr = parseAssignment(); // the middle branch allows a full assignment-expression, same as real C
		if (!thenExpr)
			return nullptr;

		if (!expect(TokenKind::Colon, "':'"))
			return nullptr;

		Expr* elseExpr = parseTernary(); // right-associative: a ? b : c ? d : e groups as a ? b : (c ? d : e)
		if (!elseExpr)
			return nullptr;

		return _arena.create<ast::TernaryExpr>(location, cond, thenExpr, elseExpr);
	}

	Expr* Parser::parseBinary(int minPrecedence)
	{
		Expr* lhs = parseCast();
		if (!lhs)
			return nullptr;

		const auto& table = binaryOpTable();
		for (;;)
		{
			auto it = table.find(_current.kind());
			if (it == table.end() || it->second.first < minPrecedence)
				break;

			BinaryOp op = it->second.second;
			int precedence = it->second.first;
			SourceLocation location = _current.location();
			advance();

			Expr* rhs = parseBinary(precedence + 1); // left-associative: rhs needs strictly higher precedence
			if (!rhs)
				return nullptr;

			lhs = _arena.create<ast::BinaryExpr>(location, op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseCast()
	{
		SourceLocation location = _current.location();
		if (check(TokenKind::LParen) && (isDeclSpecifierStart(_next.kind()) || isTypeSpecStart(_next)))
		{
			advance(); // '('
			const Type* targetType = parseTypeName();
			expect(TokenKind::RParen, "')'");

			Expr* operand = parseCast(); // right-recursive: (int)(float)x is a chain of casts
			if (!operand)
				return nullptr;
			return _arena.create<ast::CastExpr>(location, targetType, operand);
		}
		return parseUnary();
	}

	Expr* Parser::parseUnary()
	{
		SourceLocation location = _current.location();
		switch (_current.kind())
		{
			case TokenKind::Ampersand: advance(); return wrapUnary(UnaryOp::AddressOf, location, parseUnary());
			case TokenKind::Star: advance(); return wrapUnary(UnaryOp::Deref, location, parseUnary());
			case TokenKind::Minus: advance(); return wrapUnary(UnaryOp::Negate, location, parseUnary());
			case TokenKind::Bang: advance(); return wrapUnary(UnaryOp::LogicalNot, location, parseUnary());
			case TokenKind::Tilde: advance(); return wrapUnary(UnaryOp::BitwiseNot, location, parseUnary());
			case TokenKind::PlusPlus: advance(); return wrapUnary(UnaryOp::PreIncrement, location, parseUnary());
			case TokenKind::MinusMinus: advance(); return wrapUnary(UnaryOp::PreDecrement, location, parseUnary());
			case TokenKind::KwSizeof: return parseSizeof(location);
			case TokenKind::KwAlignof: return parseAlignof(location);
			default: return parsePostfix();
		}
	}

	Expr* Parser::parseSizeof(SourceLocation location)
	{
		advance(); // 'sizeof'

		if (check(TokenKind::LParen) && (isDeclSpecifierStart(_next.kind()) || isTypeSpecStart(_next)))
		{
			advance(); // '('
			const Type* argumentType = parseTypeName();
			expect(TokenKind::RParen, "')'");
			return _arena.create<ast::SizeofExpr>(location, argumentType);
		}

		Expr* operand = parseUnary();
		if (!operand)
			return nullptr;
		return _arena.create<ast::SizeofExpr>(location, operand);
	}

	Expr* Parser::parseAlignof(SourceLocation location)
	{
		advance();
		if (!expect(TokenKind::LParen, "'(' after 'alignof'"))
			return nullptr;
		const Type* argumentType = parseTypeName();
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		return argumentType ? _arena.create<ast::AlignofExpr>(location, argumentType) : nullptr;
	}

	Expr* Parser::parsePostfix()
	{
		Expr* expr = parsePrimary();
		if (!expr)
			return nullptr;

		for (;;)
		{
			SourceLocation location = _current.location();

			if (match(TokenKind::LBracket))
			{
				Expr* index = parseExpression();
				expect(TokenKind::RBracket, "']'");
				if (!index)
					return nullptr;
				expr = _arena.create<ast::IndexExpr>(location, expr, index);
			}
			else if (match(TokenKind::LParen))
			{
				std::vector<Expr*> args;
				if (!check(TokenKind::RParen))
				{
					do
					{
						Expr* arg = parseAssignment();
						if (!arg)
							return nullptr;
						args.push_back(arg);
					} while (match(TokenKind::Comma));
				}
				expect(TokenKind::RParen, "')'");
				expr = _arena.create<ast::CallExpr>(location, expr, copyArgsToArena(args));
			}
			else if (match(TokenKind::Dot))
			{
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(_current.location(), "expected member name after '.'");
					return nullptr;
				}
				std::string_view name = _current.lexeme();
				advance();
				expr = _arena.create<ast::MemberExpr>(location, expr, name, false);
			}
			else if (match(TokenKind::Arrow))
			{
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(_current.location(), "expected member name after '->'");
					return nullptr;
				}
				std::string_view name = _current.lexeme();
				advance();
				expr = _arena.create<ast::MemberExpr>(location, expr, name, true);
			}
			else if (match(TokenKind::PlusPlus))
			{
				expr = _arena.create<ast::UnaryExpr>(location, UnaryOp::PostIncrement, expr);
			}
			else if (match(TokenKind::MinusMinus))
			{
				expr = _arena.create<ast::UnaryExpr>(location, UnaryOp::PostDecrement, expr);
			}
			else
			{
				break;
			}

			if (!expr)
				return nullptr;
		}
		return expr;
	}

	Expr* Parser::parsePrimary()
	{
		SourceLocation location = _current.location();
		switch (_current.kind())
		{
			case TokenKind::Identifier:
			{
				std::string_view name = _current.lexeme();
				// The variadic builtins are syntax, not calls - va_arg's second operand is a type,
				// and all four write through the va_list the caller named. Only treated as such in
				// call position, so a program that uses one of these names for something else of
				// its own keeps working.
				if (std::optional<ast::VaOp> op = vaBuiltinFor(name); op && _next.is(TokenKind::LParen))
					return parseVaBuiltin(location, *op);
				// The machine builtins are the same idea one step lower: a name in call position that
				// stands for an instruction rather than for a function.
				if (std::optional<ast::MachineOp> machineOp = machineBuiltinFor(name); machineOp && _next.is(TokenKind::LParen))
					return parseMachineBuiltin(location, *machineOp);
				advance();
				return _arena.create<ast::NameExpr>(location, name);
			}
			case TokenKind::LiteralInt:
			{
				auto value = _current.integralValue();
				advance();
				return _arena.create<ast::IntLiteralExpr>(location, value);
			}
			case TokenKind::LiteralFloat:
			{
				auto value = _current.floatingValue();
				advance();
				return _arena.create<ast::FloatLiteralExpr>(location, value);
			}
			case TokenKind::LiteralChar:
			{
				auto value = _current.charValue();
				advance();
				return _arena.create<ast::CharLiteralExpr>(location, value);
			}
			case TokenKind::LiteralBool:
			{
				auto value = _current.boolValue();
				advance();
				return _arena.create<ast::BoolLiteralExpr>(location, value);
			}
			case TokenKind::LiteralString:
			{
				auto value = _current.stringValue();
				advance();
				return _arena.create<ast::StringLiteralExpr>(location, value);
			}
			case TokenKind::LParen:
			{
				advance();
				Expr* inner = parseExpression();
				expect(TokenKind::RParen, "')'");
				return inner;
			}
			default:
			{
				_diagnostics.error(location, "expected expression but found '{}'",
					_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
				advance(); // guarantee forward progress even without a real synchronization point (see header comment)
				return nullptr;
			}
		}
	}

	// ---- type-name (current subset: primitives + signed/unsigned/short/long, no struct/enum/typedef
	// yet - see type.h) ---------------------------------------------------------------------------

	Parser::DeclSpecifiers Parser::parseDeclSpecifiers()
	{
		DeclSpecifiers specifiers;
		specifiers.location = _current.location();

		for (;;)
		{
			SourceLocation here = _current.location();
			ast::StorageClass storageClass = ast::StorageClass::None;
			switch (_current.kind())
			{
				case TokenKind::KwConst:
					advance();
					if (specifiers.isConst)
						_diagnostics.error(here, "duplicate 'const'");
					specifiers.isConst = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwVolatile:
					advance();
					if (specifiers.isVolatile)
						_diagnostics.error(here, "duplicate 'volatile'");
					specifiers.isVolatile = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwRestrict:
					advance();
					if (specifiers.isRestrict)
						_diagnostics.error(here, "duplicate 'restrict'");
					specifiers.isRestrict = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwInline:
					advance();
					if (specifiers.isInline)
						_diagnostics.error(here, "duplicate 'inline'");
					specifiers.isInline = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwInterrupt:
					advance();
					if (specifiers.isInterrupt)
						_diagnostics.error(here, "duplicate '__interrupt'");
					specifiers.isInterrupt = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwStatic: storageClass = ast::StorageClass::Static; break;
				case TokenKind::KwExtern: storageClass = ast::StorageClass::Extern; break;
				case TokenKind::KwAuto:   storageClass = ast::StorageClass::Auto; break;
				case TokenKind::KwRegister: storageClass = ast::StorageClass::Register; break;
				default:
					return specifiers;
			}

			advance();
			if (specifiers.storageClass != ast::StorageClass::None)
			{
				// Not "duplicate": `static extern` is two different answers to one question, and
				// saying which two is more useful than saying there are two.
				_diagnostics.error(here, "cannot combine '{}' with '{}' on the same declaration",
					ast::storageClassName(storageClass), ast::storageClassName(specifiers.storageClass));
			}
			specifiers.storageClass = storageClass;
			specifiers.sawAny = true;
		}
	}

	// One run of `const`/`volatile`, in any order, folded into flags the caller already holds. The
	// same loop serves all three positions a qualifier can occupy - before the type-spec, after it,
	// and after each `*` - because C allows either word, in either order, wherever one of them is
	// allowed at all. Only the POSITION decides what they qualify, never which word came first,
	// which is also what makes a qualifier written on one side a duplicate of the same one written
	// on the other.
	void Parser::parseQualifierRun(bool& isConst, bool& isVolatile)
	{
		for (;;)
		{
			if (check(TokenKind::KwConst))
			{
				if (isConst)
					_diagnostics.error(_current.location(), "duplicate 'const'");
				isConst = true;
				advance();
				continue;
			}
			if (check(TokenKind::KwVolatile))
			{
				if (isVolatile)
					_diagnostics.error(_current.location(), "duplicate 'volatile'");
				isVolatile = true;
				advance();
				continue;
			}
			return;
		}
	}

	const Type* Parser::parseTypeName()
	{
		return parseTypeName(false, false);
	}

	const Type* Parser::parseTypeName(bool leadingConst, bool leadingVolatile)
	{
		// A qualifier may sit on either side of the type-spec - `const int` and `int const` are the
		// same type in C - and again after every `*`, where it means something different: `const
		// char* p` is a pointer to const, `char* const p` is a const pointer. Those are two distinct
		// types and the difference is exactly which side of the star the word is on, which is why
		// this cannot be collapsed into one leading flag.
		DeclSpecifiers leading = parseDeclSpecifiers();
		if (leading.storageClass != ast::StorageClass::None || leading.isInline || leading.isInterrupt)
		{
			_diagnostics.error(leading.location,
				"a storage-class specifier is not allowed here - it belongs to a declaration, not to a type name");
		}

		const Type* base = parseTypeSpec();
		if (!base)
			return nullptr;

		bool isConst = leading.isConst || leadingConst;
		bool isVolatile = leading.isVolatile || leadingVolatile;
		bool isRestrict = leading.isRestrict;
		parseQualifierRun(isConst, isVolatile); // trailing form: `int const`, `int volatile`
		if (isConst)
			base = Type::withConst(_arena, base);
		if (isVolatile)
			base = Type::withVolatile(_arena, base);

		while (match(TokenKind::Star))
		{
			base = Type::makePointer(_arena, base);
			bool pointerIsConst = false;
			bool pointerIsVolatile = false;
			parseQualifierRun(pointerIsConst, pointerIsVolatile);
			if (pointerIsConst)
				base = Type::withConst(_arena, base);
			if (pointerIsVolatile)
				base = Type::withVolatile(_arena, base);
		}
		if (isRestrict)
		{
			if (!base->isPointer())
				_diagnostics.error(leading.location, "'restrict' requires a pointer type");
			else
				base = Type::withRestrict(_arena, base);
		}
		return base;
	}

	const Type* Parser::parseTypeSpec()
	{
		SourceLocation location = _current.location();

		if (check(TokenKind::Identifier))
		{
			auto it = _typedefTable.find(_current.lexeme());
			if (it != _typedefTable.end())
			{
				advance();
				return it->second;
			}
			// Not a registered typedef name - falls through to the same "expected type name"
			// diagnostic as any other non-type-spec token, via the switch's default case below.
		}

		switch (_current.kind())
		{
			case TokenKind::KwVoid: advance(); return &Type::Void;
			case TokenKind::KwBool: advance(); return &Type::Bool;
			case TokenKind::KwFloat: advance(); return &Type::Float;
			case TokenKind::KwChar: advance(); return &Type::Char;
			case TokenKind::KwShort: advance(); match(TokenKind::KwInt); return &Type::Short;
			case TokenKind::KwLong: advance(); match(TokenKind::KwInt); return &Type::Long;
			case TokenKind::KwInt: advance(); return &Type::Int;
			case TokenKind::KwSigned:
			case TokenKind::KwUnsigned:
			{
				bool isUnsigned = check(TokenKind::KwUnsigned);
				advance();
				if (match(TokenKind::KwChar))
					return isUnsigned ? &Type::UChar : &Type::SChar;
				if (match(TokenKind::KwShort))
				{
					match(TokenKind::KwInt);
					return isUnsigned ? &Type::UShort : &Type::Short;
				}
				if (match(TokenKind::KwLong))
				{
					match(TokenKind::KwInt);
					return isUnsigned ? &Type::ULong : &Type::Long;
				}
				match(TokenKind::KwInt); // optional and redundant: "unsigned" alone already means "unsigned int"
				return isUnsigned ? &Type::UInt : &Type::Int;
			}
			case TokenKind::KwStruct: return parseStructTypeSpec();
			case TokenKind::KwUnion: return parseStructTypeSpec(true);
			case TokenKind::KwEnum: return parseEnumTypeSpec();
			default:
				_diagnostics.error(location, "expected type name but found '{}'",
					_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
				advance(); // guarantee forward progress, same reasoning as parsePrimary()'s default case
				return nullptr;
		}
	}

	// ---- struct/enum type-specs (also how struct/enum DECLARATIONS get parsed - see parser.h's
	// header comment: parseExternalDecl()/parseDeclStatement() call parseTypeName() -> parseTypeSpec()
	// -> here exactly like any other type-spec, then just check whether a ';' immediately follows to
	// tell "just declaring the tag" apart from "declaring a tag and a variable") -------------------

	const Type* Parser::parseStructTypeSpec(bool isUnion)
	{
		SourceLocation location = _current.location();
		advance(); // 'struct' or 'union'

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected a {} tag name", isUnion ? "union" : "struct");
			advance();
			return nullptr;
		}
		std::string_view tagName = _current.lexeme();
		advance();

		auto it = _structTable.find(tagName);
		StructDecl* decl = (it != _structTable.end()) ? it->second : nullptr;
		if (!decl)
		{
			// Registered here, BEFORE the body (if any) is parsed - not after - so a field that
			// refers back to this same tag (always through a pointer, e.g. `struct Node* next;`)
			// resolves to this exact StructDecl instance. See decl.h's note on StructDecl's two-step
			// construction.
			decl = _arena.create<StructDecl>(location, tagName, isUnion);
			_structTable[tagName] = decl;
		}

		if (match(TokenKind::LBrace))
		{
			if (decl->isComplete())
				_diagnostics.error(location, "redefinition of '{} {}'", isUnion ? "union" : "struct", tagName);

			std::vector<FieldDecl> fields;
			while (!check(TokenKind::RBrace) && !isAtEnd())
			{
				SourceLocation fieldLoc = _current.location();
				const Type* fieldType = parseTypeName();
				if (!fieldType)
				{
					synchronizeStatement(); // reuses the statement-level recovery: skip to ';' or '}'
					continue;
				}
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(_current.location(), "expected a field name");
					synchronizeStatement();
					continue;
				}
				std::string_view fieldName = _current.lexeme();
				advance();
				if (check(TokenKind::LBracket))
					fieldType = parseArrayDeclaratorSuffix(fieldType, /*isParameter=*/false);
				if (!expect(TokenKind::Semicolon, "';'"))
				{
					synchronizeStatement();
					continue;
				}
				fields.push_back(FieldDecl{ fieldType, fieldName, fieldLoc });
			}
			expect(TokenKind::RBrace, "'}'");
			decl->setFields(copyFieldsToArena(fields));
		}

		return isUnion ? Type::makeUnion(_arena, decl) : Type::makeStruct(_arena, decl);
	}

	const Type* Parser::parseEnumTypeSpec()
	{
		SourceLocation location = _current.location();
		advance(); // 'enum'

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected an enum tag name after 'enum'");
			advance();
			return nullptr;
		}
		std::string_view tagName = _current.lexeme();
		advance();

		auto it = _enumTable.find(tagName);
		EnumDecl* decl = (it != _enumTable.end()) ? it->second : nullptr;
		if (!decl)
		{
			decl = _arena.create<EnumDecl>(location, tagName);
			_enumTable[tagName] = decl;
		}

		if (match(TokenKind::LBrace))
		{
			if (decl->isComplete())
				_diagnostics.error(location, "redefinition of 'enum {}'", tagName);

			std::vector<EnumeratorDecl> enumerators;
			while (!check(TokenKind::RBrace) && !isAtEnd())
			{
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(_current.location(), "expected an enumerator name");
					synchronizeStatement(); // no per-enumerator ';' to stop on, but this still bounds progress via '}'/EOF
					break;
				}
				SourceLocation enumLoc = _current.location();
				std::string_view enumName = _current.lexeme();
				advance();

				Expr* value = nullptr;
				if (match(TokenKind::Equal))
				{
					value = parseAssignment(); // a constant-expression in real C - sema checks constancy, not the parser
					if (!value)
						break;
				}
				enumerators.push_back(EnumeratorDecl{ enumName, value, enumLoc });

				if (!match(TokenKind::Comma))
					break; // no comma - must be the closing '}', checked by expect() below
			}
			expect(TokenKind::RBrace, "'}'");
			decl->setEnumerators(copyEnumeratorsToArena(enumerators));
		}

		return Type::makeEnum(_arena, decl);
	}

	// ---- statements (§7's grammar) -----------------------------------------------------------------
	//
	// parseCompoundStatement() and parseTranslationUnit() (below) are the two loops that turn a
	// failed inner parse into panic-mode recovery instead of giving up: on nullptr they call
	// synchronizeStatement()/synchronizeDeclaration() and keep collecting, so one bad statement or
	// declaration does not take the rest of the block/file down with it. Every other function in
	// this section still returns nullptr on failure and lets it propagate to its own caller,
	// exactly like Fase 2's expression parsing - only those two loops actually swallow a nullptr.

	Stmt* Parser::parseStatement()
	{
		switch (_current.kind())
		{
			case TokenKind::LBrace: return parseCompoundStatement();
			case TokenKind::KwIf: return parseIfStatement();
			case TokenKind::KwWhile: return parseWhileStatement();
			case TokenKind::KwDo: return parseDoWhileStatement();
			case TokenKind::KwFor: return parseForStatement();
			case TokenKind::KwReturn: return parseReturnStatement();
			case TokenKind::KwBreak: return parseBreakStatement();
			case TokenKind::KwContinue: return parseContinueStatement();
			case TokenKind::KwSwitch: return parseSwitchStatement();
			case TokenKind::KwCase: return parseCaseStatement();
			case TokenKind::KwDefault: return parseDefaultStatement();
			case TokenKind::KwGoto: return parseGotoStatement();
			case TokenKind::KwTypedef:
			{
				SourceLocation location = _current.location();
				Decl* decl = parseTypedefDecl();
				if (!decl)
					return nullptr;
				return _arena.create<ast::DeclStmt>(location, decl);
			}
			case TokenKind::Semicolon:
			{
				SourceLocation location = _current.location();
				advance();
				return _arena.create<ast::EmptyStmt>(location);
			}
			default:
				if (check(TokenKind::Identifier) && _next.is(TokenKind::Colon))
					return parseLabeledStatement();
				// A declaration may begin with its storage class instead of its type - `static int n;`
				// is a declaration just as much as `int n;` is, and reaching parseExprStatement() with
				// `static` in hand is what used to produce "expected expression but found 'static'".
				if (isTypeSpecStart(_current) || isDeclSpecifierStart(_current.kind()))
					return parseDeclStatement();
				return parseExprStatement();
		}
	}

	Stmt* Parser::parseCompoundStatement()
	{
		SourceLocation location = _current.location();
		if (!expect(TokenKind::LBrace, "'{'"))
			return nullptr;

		std::vector<Stmt*> stmts;
		while (!check(TokenKind::RBrace) && !isAtEnd())
		{
			Stmt* stmt = parseStatement();
			if (!stmt)
			{
				synchronizeStatement();
				continue;
			}
			stmts.push_back(stmt);
		}

		if (!expect(TokenKind::RBrace, "'}'"))
			return nullptr;

		return _arena.create<ast::CompoundStmt>(location, copyStmtsToArena(stmts));
	}

	Stmt* Parser::parseIfStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'if'

		if (!expect(TokenKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!cond)
			return nullptr;

		Stmt* thenStmt = parseStatement();
		if (!thenStmt)
			return nullptr;

		Stmt* elseStmt = nullptr;
		if (match(TokenKind::KwElse))
		{
			elseStmt = parseStatement();
			if (!elseStmt)
				return nullptr;
		}

		return _arena.create<ast::IfStmt>(location, cond, thenStmt, elseStmt);
	}

	Stmt* Parser::parseWhileStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'while'

		if (!expect(TokenKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!cond)
			return nullptr;

		Stmt* body = parseStatement();
		if (!body)
			return nullptr;

		return _arena.create<ast::WhileStmt>(location, cond, body);
	}

	Stmt* Parser::parseDoWhileStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'do'

		Stmt* body = parseStatement();
		if (!body)
			return nullptr;

		if (!expect(TokenKind::KwWhile, "'while'"))
			return nullptr;
		if (!expect(TokenKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!cond)
			return nullptr;
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		return _arena.create<ast::DoWhileStmt>(location, body, cond);
	}

	Stmt* Parser::parseForStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'for'

		if (!expect(TokenKind::LParen, "'('"))
			return nullptr;

		Stmt* init = nullptr;
		if (match(TokenKind::Semicolon))
		{
			// no init-clause - `for (;` already consumed its ';'
		}
		else if (isTypeSpecStart(_current) || isDeclSpecifierStart(_current.kind()))
		{
			init = parseDeclStatement(); // consumes its own trailing ';'
			if (!init)
				return nullptr;
		}
		else
		{
			Expr* initExpr = parseExpression();
			if (!initExpr)
				return nullptr;
			if (!expect(TokenKind::Semicolon, "';'"))
				return nullptr;
			init = _arena.create<ast::ExprStmt>(initExpr->location(), initExpr);
		}

		Expr* cond = nullptr;
		if (!check(TokenKind::Semicolon))
		{
			cond = parseExpression();
			if (!cond)
				return nullptr;
		}
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		Expr* increment = nullptr;
		if (!check(TokenKind::RParen))
		{
			increment = parseExpression();
			if (!increment)
				return nullptr;
		}
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;

		Stmt* body = parseStatement();
		if (!body)
			return nullptr;

		return _arena.create<ast::ForStmt>(location, init, cond, increment, body);
	}

	Stmt* Parser::parseReturnStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'return'

		Expr* value = nullptr;
		if (!check(TokenKind::Semicolon))
		{
			value = parseExpression();
			if (!value)
				return nullptr;
		}
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		return _arena.create<ast::ReturnStmt>(location, value);
	}

	Stmt* Parser::parseBreakStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'break'
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;
		return _arena.create<ast::BreakStmt>(location);
	}

	Stmt* Parser::parseContinueStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'continue'
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;
		return _arena.create<ast::ContinueStmt>(location);
	}

	Stmt* Parser::parseSwitchStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'switch'

		if (!expect(TokenKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!cond)
			return nullptr;

		Stmt* body = parseStatement(); // typically a CompoundStmt full of case/default labels - see stmt.h
		if (!body)
			return nullptr;

		return _arena.create<ast::SwitchStmt>(location, cond, body);
	}

	Stmt* Parser::parseCaseStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'case'

		Expr* value = parseExpression(); // a constant-expression in real C - sema checks constancy, not the parser
		if (!expect(TokenKind::Colon, "':'"))
			return nullptr;
		if (!value)
			return nullptr;

		Stmt* body = parseStatement(); // exactly the one statement following ':' - see stmt.h
		if (!body)
			return nullptr;

		return _arena.create<ast::CaseStmt>(location, value, body);
	}

	Stmt* Parser::parseDefaultStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'default'

		if (!expect(TokenKind::Colon, "':'"))
			return nullptr;

		Stmt* body = parseStatement();
		if (!body)
			return nullptr;

		return _arena.create<ast::DefaultStmt>(location, body);
	}

	Stmt* Parser::parseGotoStatement()
	{
		SourceLocation location = _current.location();
		advance(); // 'goto'

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected a label name after 'goto'");
			return nullptr;
		}
		std::string_view label = _current.lexeme();
		advance();

		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		return _arena.create<ast::GotoStmt>(location, label);
	}

	Stmt* Parser::parseLabeledStatement()
	{
		SourceLocation location = _current.location();
		std::string_view label = _current.lexeme();
		advance(); // identifier
		advance(); // ':'

		Stmt* body = parseStatement();
		if (!body)
			return nullptr;

		return _arena.create<ast::LabelStmt>(location, label, body);
	}

	Stmt* Parser::parseDeclStatement()
	{
		SourceLocation location = _current.location();
		// Same split as parseExternalDecl(): the storage class belongs to the declaration, the
		// `const` to the type. A local may say `static`, `extern`, `auto` or nothing at all; which
		// of those make sense in a block is sema's call, not this one's.
		DeclSpecifiers specifiers = parseDeclSpecifiers();
		const Type* type = parseTypeName(specifiers.isConst, specifiers.isVolatile);
		if (!type)
			return nullptr;
		if (specifiers.isRestrict)
		{
			if (!type->isPointer())
				_diagnostics.error(specifiers.location, "'restrict' requires a pointer type");
			else
				type = Type::withRestrict(_arena, type);
		}

		// `struct Foo { ... };` / `enum Bar { ... };` with no variable declarator: parseTypeName()
		// already registered/completed the tag (see parseStructTypeSpec()/parseEnumTypeSpec()), so
		// there is nothing left to declare - just wrap the tag itself in the DeclStmt.
		if (check(TokenKind::Semicolon) && (type->isAggregate() || type->isEnum()))
		{
			advance();
			Decl* tagDecl = type->isAggregate() ? static_cast<Decl*>(type->structDecl()) : static_cast<Decl*>(type->enumDecl());
			return _arena.create<ast::DeclStmt>(location, tagDecl);
		}

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected an identifier in declaration");
			return nullptr;
		}
		std::string_view name = _current.lexeme();
		advance();

		if (check(TokenKind::LBracket))
			type = parseArrayDeclaratorSuffix(type, /*isParameter=*/false);

		Decl* decl = finishVarDecl(location, name, type, specifiers);
		if (!decl)
			return nullptr;
		return _arena.create<ast::DeclStmt>(location, decl);
	}

	Stmt* Parser::parseExprStatement()
	{
		SourceLocation location = _current.location();
		Expr* expr = parseExpression();
		if (!expr)
			return nullptr;
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;
		return _arena.create<ast::ExprStmt>(location, expr);
	}

	// ---- declarations (§7's grammar) ---------------------------------------------------------------

	TranslationUnit* Parser::parseTranslationUnit()
	{
		SourceLocation location = _current.location();
		std::vector<Decl*> decls;
		while (!isAtEnd())
		{
			Decl* decl = parseExternalDecl();
			if (!decl)
			{
				synchronizeDeclaration();
				continue;
			}
			decls.push_back(decl);
		}
		return _arena.create<TranslationUnit>(location, copyDeclsToArena(decls));
	}

	Decl* Parser::parseExternalDecl()
	{
		SourceLocation location = _current.location();

		if (check(TokenKind::KwTypedef))
			return parseTypedefDecl();

		// Storage classes and `const` come first and belong to the DECLARATION, so they are read
		// here rather than inside parseTypeName() - which would have no one to hand a storage class
		// to, and rejects one for that reason. The `const` half is put back on the type afterwards.
		DeclSpecifiers specifiers = parseDeclSpecifiers();

		if (!isTypeSpecStart(_current))
		{
			_diagnostics.error(_current.location(), "expected a declaration but found '{}'",
				_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
			return nullptr;
		}

		const Type* type = parseTypeName(specifiers.isConst, specifiers.isVolatile);
		if (!type)
			return nullptr;
		if (specifiers.isRestrict)
		{
			if (!type->isPointer())
				_diagnostics.error(specifiers.location, "'restrict' requires a pointer type");
			else
				type = Type::withRestrict(_arena, type);
		}

		// `struct Foo { ... };` / `enum Bar { ... };` with no variable declarator - see
		// parseDeclStatement()'s identical check for the local-statement equivalent.
		if (check(TokenKind::Semicolon) && (type->isAggregate() || type->isEnum()))
		{
			advance();
			return type->isAggregate() ? static_cast<Decl*>(type->structDecl()) : static_cast<Decl*>(type->enumDecl());
		}

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected an identifier in declaration");
			return nullptr;
		}
		std::string_view name = _current.lexeme();
		advance();

		if (check(TokenKind::LParen))
			return finishFunctionDecl(location, name, type, specifiers);

		if (check(TokenKind::LBracket))
			type = parseArrayDeclaratorSuffix(type, /*isParameter=*/false);
		return finishVarDecl(location, name, type, specifiers);
	}

	Decl* Parser::parseTypedefDecl()
	{
		SourceLocation location = _current.location();
		advance(); // 'typedef'

		const Type* underlyingType = parseTypeName();
		if (!underlyingType)
			return nullptr;

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(_current.location(), "expected a name after 'typedef'");
			return nullptr;
		}
		std::string_view name = _current.lexeme();
		advance();

		if (check(TokenKind::LBracket))
			underlyingType = parseArrayDeclaratorSuffix(underlyingType, /*isParameter=*/false);

		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		_typedefTable[name] = underlyingType; // makes `name` usable as a type-spec from here on - see isTypeSpecStart(const Token&)/parseTypeSpec()
		return _arena.create<ast::TypedefDecl>(location, name, underlyingType);
	}

	Expr* Parser::parseInitializer()
	{
		if (!check(TokenKind::LBrace))
			return parseAssignment();

		SourceLocation location = _current.location();
		advance(); // '{'

		if (check(TokenKind::RBrace))
		{
			// §3's initializer-list needs at least one element - see parser.h's own note.
			_diagnostics.error(location, "an initializer list needs at least one value");
			advance(); // '}'
			return nullptr;
		}

		std::vector<Expr*> elements;
		while (true)
		{
			Expr* element = parseInitializer(); // an element is itself an `initializer` - nesting
			if (!element)
				return nullptr;
			elements.push_back(element);

			if (!check(TokenKind::Comma))
				break;
			SourceLocation commaLocation = _current.location();
			advance(); // ','
			if (check(TokenKind::RBrace))
			{
				// `{ 1, 2, }` - real C allows it, §3's grammar does not. Reported where the comma
				// actually is rather than at the brace, and the list is kept: the values are all
				// there, so there is nothing to recover from beyond the stray comma itself.
				_diagnostics.error(commaLocation, "a trailing ',' in an initializer list is not accepted in this version");
				break;
			}
		}

		if (!expect(TokenKind::RBrace, "'}'"))
			return nullptr;

		return _arena.create<ast::InitListExpr>(location, copyArgsToArena(elements));
	}

	Decl* Parser::finishVarDecl(SourceLocation location, std::string_view name, const Type* type,
		const DeclSpecifiers& specifiers)
	{
		if (specifiers.isInterrupt)
		{
			// `__interrupt` describes how a function is ENTERED AND LEFT - no arguments, every
			// register restored, `iret` at the end. A variable has none of that to describe.
			_diagnostics.error(specifiers.location, "'__interrupt' is only allowed on a function");
		}
		Expr* initializer = nullptr;
		if (match(TokenKind::Equal))
		{
			// parseInitializer(), not parseAssignment(): a declarator is the one position §3's
			// grammar allows a brace initializer-list in.
			initializer = parseInitializer();
		}
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		if (specifiers.isInline)
			_diagnostics.error(specifiers.location, "'inline' is only allowed on a function");

		return _arena.create<ast::VarDecl>(location, name, type, initializer, specifiers.storageClass);
	}

	Decl* Parser::finishFunctionDecl(SourceLocation location, std::string_view name, const Type* returnType,
		const DeclSpecifiers& specifiers)
	{
		advance(); // '('

		std::vector<Param> params;
		bool isVariadic = false;
		if (!parseParamList(params, isVariadic))
			return nullptr;
		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;

		CompoundStmt* body = nullptr;
		if (check(TokenKind::LBrace))
		{
			Stmt* bodyStmt = parseCompoundStatement();
			if (!bodyStmt)
				return nullptr;
			body = static_cast<CompoundStmt*>(bodyStmt); // parseCompoundStatement() only ever returns a CompoundStmt* (or nullptr)
		}
		else if (!expect(TokenKind::Semicolon, "';' or a function body"))
		{
			return nullptr;
		}

		if (specifiers.storageClass == ast::StorageClass::Auto)
		{
			// `auto` means automatic STORAGE, which a function does not have. Rejected here rather
			// than in sema because there is nothing type-dependent about it.
			_diagnostics.error(specifiers.location, "'auto' is not allowed on a function");
		}
		if (specifiers.storageClass == ast::StorageClass::Register)
		{
			// Same: `register` asks for a kind of storage a function does not have either. Only
			// `static`, `extern` and `inline` say anything about one.
			_diagnostics.error(specifiers.location, "'register' is not allowed on a function");
		}
		if (specifiers.isInline && !body)
			_diagnostics.error(specifiers.location, "'inline' is only meaningful on a function definition, not on a prototype");
		if (specifiers.isInterrupt && specifiers.isInline)
		{
			// Nothing calls an interrupt handler, so there is no call site to inline it into.
			_diagnostics.error(specifiers.location, "'__interrupt' cannot be combined with 'inline'");
		}

		// Neither `auto` nor `register` survives onto the node: both were rejected just above, and
		// carrying one forward would leave every later phase asking what a register-resident
		// function is supposed to be. Error recovery continues with the default linkage instead.
		ast::StorageClass storageClass = specifiers.storageClass;
		if (storageClass == ast::StorageClass::Auto || storageClass == ast::StorageClass::Register)
			storageClass = ast::StorageClass::None;

		return _arena.create<ast::FunctionDecl>(location, name, returnType, copyParamsToArena(params), body,
			storageClass, specifiers.isInline, isVariadic, specifiers.isInterrupt);
	}

	bool Parser::parseParamList(std::vector<Param>& outParams, bool& outIsVariadic)
	{
		outIsVariadic = false;

		if (check(TokenKind::RParen))
			return true; // foo()

		if (check(TokenKind::KwVoid) && _next.is(TokenKind::RParen))
		{
			advance(); // 'void' - foo(void) means the same as foo(), same as real C
			return true;
		}

		if (check(TokenKind::Ellipsis))
		{
			// `f(...)` with no fixed parameter before it. Real C89 rejects it too, and here the
			// reason is not merely conformance: va_start() names the last fixed parameter to find
			// where the variadic arguments begin (docs/09-Variadic-Convention.md), so a list with
			// no fixed parameter has nothing such a call could ever name.
			_diagnostics.error(_current.location(), "'...' requires at least one named parameter before it");
			return false;
		}

		do
		{
			if (check(TokenKind::Ellipsis))
			{
				// Only ever valid as the whole of the last entry: `f(int x, ...)`. Everything else
				// the loop could reach here - `f(int x, ..., int y)`, `f(int x, ...,)` - is caught
				// by the RParen check below, since the ellipsis consumes no declarator of its own.
				advance(); // '...'
				outIsVariadic = true;
				if (!check(TokenKind::RParen))
				{
					_diagnostics.error(_current.location(), "'...' must be the last entry in a parameter list");
					return false;
				}
				return true;
			}

			SourceLocation location = _current.location();
			const Type* type = parseTypeName();
			if (!type)
				return false;

			if (!check(TokenKind::Identifier))
			{
				_diagnostics.error(_current.location(), "expected a parameter name");
				return false;
			}
			std::string_view name = _current.lexeme();
			advance();

			if (check(TokenKind::LBracket))
				type = parseArrayDeclaratorSuffix(type, /*isParameter=*/true);
			else if (type->isArray())
				type = Type::makePointer(_arena, type->arrayElementType());

			outParams.push_back(Param{ type, name, location });
		} while (match(TokenKind::Comma));

		return true;
	}

	// ---- array declarator suffix (see parser.h's header comment on this method for the parameter-
	// decay rule and why every dimension outside a parameter declarator must be an INT_LITERAL) ------

	const Type* Parser::parseArrayDeclaratorSuffix(const Type* elementType, bool isParameter)
	{
		SourceLocation location = _current.location(); // the first '[' - already checked by the caller

		std::vector<std::optional<u32>> dims;
		bool first = true;
		while (match(TokenKind::LBracket))
		{
			bool outerParamDim = isParameter && first;
			if (check(TokenKind::RBracket))
			{
				if (!outerParamDim)
					_diagnostics.error(_current.location(),
						"array size is required here (this version cannot infer it from an initializer)");
				dims.push_back(std::nullopt);
			}
			else if (!check(TokenKind::LiteralInt))
			{
				_diagnostics.error(_current.location(), "expected an integer constant for the array size but found '{}'",
					_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
				dims.push_back(std::nullopt);
				// Resync to the closing ']' so the declarator doesn't lose every dimension after this
				// one just because a single size expression wasn't a plain integer literal.
				while (!check(TokenKind::RBracket) && !check(TokenKind::Semicolon) && !check(TokenKind::Comma) && !isAtEnd())
					advance();
				if (!check(TokenKind::RBracket))
				{
					first = false;
					continue;
				}
			}
			else
			{
				SourceLocation sizeLocation = _current.location();
				u64 rawSize = _current.integralValue();
				advance();
				if (!outerParamDim && (rawSize == 0 || rawSize > 0xFFFFFFFFull))
				{
					_diagnostics.error(sizeLocation, "array size must be a positive integer that fits in 32 bits");
					dims.push_back(std::nullopt);
				}
				else
				{
					dims.push_back(static_cast<u32>(rawSize));
				}
			}
			expect(TokenKind::RBracket, "']'");
			first = false;
		}

		if (elementType->isVoid())
			_diagnostics.error(location, "array has invalid element type 'void'");

		// Parameter decay (real C): the OUTERMOST dimension of a parameter's array declarator is not
		// part of the type at all - it becomes a pointer, and its size (even if given) is ignored -
		// see parser.h's header comment. Build inner-to-outer as usual, but stop one short of the
		// outermost dimension when decaying, then wrap the result in a pointer instead of an array.
		usize decayedOuterDims = (isParameter && !dims.empty()) ? 1 : 0;
		const Type* type = elementType;
		for (usize i = dims.size(); i > decayedOuterDims; --i)
			type = Type::makeArray(_arena, type, dims[i - 1].value_or(1)); // value_or: already diagnosed above

		if (isParameter && !dims.empty())
			type = Type::makePointer(_arena, type);

		return type;
	}

	// ---- static tables --------------------------------------------------------------------------

	const std::unordered_map<TokenKind, std::pair<int, BinaryOp>>& Parser::binaryOpTable()
	{
		static const std::unordered_map<TokenKind, std::pair<int, BinaryOp>> table = {
			{ TokenKind::PipePipe, { 2, BinaryOp::LogicalOr } },
			{ TokenKind::AmpersandAmpersand, { 3, BinaryOp::LogicalAnd } },
			{ TokenKind::Pipe, { 4, BinaryOp::BitOr } },
			{ TokenKind::Caret, { 5, BinaryOp::BitXor } },
			{ TokenKind::Ampersand, { 6, BinaryOp::BitAnd } },
			{ TokenKind::EqualEqual, { 7, BinaryOp::Eq } },
			{ TokenKind::BangEqual, { 7, BinaryOp::Ne } },
			{ TokenKind::Less, { 8, BinaryOp::Lt } },
			{ TokenKind::LessEqual, { 8, BinaryOp::Le } },
			{ TokenKind::Greater, { 8, BinaryOp::Gt } },
			{ TokenKind::GreaterEqual, { 8, BinaryOp::Ge } },
			{ TokenKind::LessLess, { 9, BinaryOp::Shl } },
			{ TokenKind::GreaterGreater, { 9, BinaryOp::Shr } },
			{ TokenKind::Plus, { 10, BinaryOp::Add } },
			{ TokenKind::Minus, { 10, BinaryOp::Sub } },
			{ TokenKind::Star, { 11, BinaryOp::Mul } },
			{ TokenKind::Slash, { 11, BinaryOp::Div } },
			{ TokenKind::Percent, { 11, BinaryOp::Mod } },
		};
		return table;
	}

	const std::unordered_map<TokenKind, AssignOp>& Parser::assignOpTable()
	{
		static const std::unordered_map<TokenKind, AssignOp> table = {
			{ TokenKind::Equal, AssignOp::Assign },
			{ TokenKind::PlusEqual, AssignOp::AddAssign },
			{ TokenKind::MinusEqual, AssignOp::SubAssign },
			{ TokenKind::StarEqual, AssignOp::MulAssign },
			{ TokenKind::SlashEqual, AssignOp::DivAssign },
			{ TokenKind::PercentEqual, AssignOp::ModAssign },
			{ TokenKind::AmpersandEqual, AssignOp::AndAssign },
			{ TokenKind::PipeEqual, AssignOp::OrAssign },
			{ TokenKind::CaretEqual, AssignOp::XorAssign },
			{ TokenKind::LessLessEqual, AssignOp::ShlAssign },
			{ TokenKind::GreaterGreaterEqual, AssignOp::ShrAssign },
		};
		return table;
	}
}
