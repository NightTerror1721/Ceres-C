#include <ceresc/parser/parser.h>

#include <cstring>
#include <format>
#include <memory>
#include <optional>

namespace ceresc::parser
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

	using support::SourceLocation;

	Parser::Parser(lexer::Lexer& lexer, support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept :
		_lexer(lexer), _arena(arena), _diagnostics(diagnostics), _current(lexer.next()), _next(lexer.next())
	{
		// `__builtin_va_list` is a builtin type name rather than something a header declares: this
		// compiler has no system include directory to find a <stdarg.h> in, so the type and the
		// four operations on it are known to the compiler itself (docs/09-Variadic-Convention.md).
		// It is a `char*` because that is exactly what it holds - a cursor into the caller's frame,
		// advanced a byte count at a time by __builtin_va_arg - and spelling it as an ordinary
		// pointer means assignment, copying and parameter passing all already work on it.
		_typedefTable.emplace("__builtin_va_list", Type::makePointer(_arena, &Type::Char));
	}

	std::optional<ast::VaOp> Parser::vaBuiltinFor(std::string_view name) noexcept
	{
		if (name == "__builtin_va_start") return ast::VaOp::Start;
		if (name == "__builtin_va_arg")   return ast::VaOp::Arg;
		if (name == "__builtin_va_end")   return ast::VaOp::End;
		if (name == "__builtin_va_copy")  return ast::VaOp::Copy;
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
			_diagnostics.error(DiagId::MachineBuiltinTakesNoArguments, _current.location(), "'{}' takes no arguments", ast::machineOpName(op));
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
				// is the whole reason __builtin_va_arg cannot be an ordinary function.
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

		_diagnostics.error(DiagId::ExpectedToken, _current.location(), "expected {} but found '{}'", what,
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
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating an expression list");
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
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating block statements");
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
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating top-level declarations");
			return {};
		}

		Decl** stored = static_cast<Decl**>(memory);
		for (usize i = 0; i < decls.size(); ++i)
			stored[i] = decls[i];
		return std::span<Decl* const>(stored, decls.size());
	}

	std::span<const Param> Parser::copyParamsToArena(std::span<const Param> params) noexcept
	{
		if (params.empty())
			return {};

		void* memory = _arena.allocate(sizeof(Param) * params.size(), alignof(Param));
		if (!memory)
		{
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating function parameters");
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
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating struct fields");
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
			_diagnostics.error(DiagId::OutOfMemory, _current.location(), "out of memory allocating enumerators");
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
			!check(TokenKind::KwInterruptVector) && !isDeclSpecifierStart(_current.kind()))
			advance();
	}

	// ---- Expressions (§7's precedence table) --------------------------------------------------

	Expr* Parser::parseExpression()
	{
		// The comma operator, the loosest of all: `a, b` evaluates a, throws it away and is b. It only
		// exists here, where a whole expression is wanted (a statement, a condition, a for clause, the
		// inside of parentheses); an argument, an initializer or an enumerator is an assignment
		// expression, so there a comma is still what separates one from the next.
		Expr* expr = parseAssignment();
		while (check(TokenKind::Comma))
		{
			SourceLocation location = _current.location();
			advance();
			Expr* rhs = parseAssignment();
			expr = _arena.create<ast::BinaryExpr>(location, BinaryOp::Comma, expr, rhs);
		}
		return expr;
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
			// `(int[]){ 1, 2 }` is the one place a type name may leave an array's size out: the list gives it.
			_allowUnsizedArray = true;
			_unsizedArrayPending = false;
			const Type* targetType = parseTypeName();
			_allowUnsizedArray = false;
			const bool unsized = _unsizedArrayPending;
			_unsizedArrayPending = false;
			expect(TokenKind::RParen, "')'");

			// `(T)` and then a brace: not a cast of a block but a compound literal.
			if (targetType && check(TokenKind::LBrace))
				return parseCompoundLiteral(location, targetType, unsized);
			if (unsized)
				_diagnostics.error(DiagId::ArraySizeRequired, location, "array size is required here (a cast cannot leave it out)");

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
			// C's grammar: unary-operator cast-expression. The operand is a cast-expr, not a unary-expr,
			// or `*(T*)p`, `-(int)x`, `!(bool)x` and `~(unsigned)x` cannot be written without an extra
			// pair of parentheses. ++/-- stay unary-expr, as in C: `++(int)x` is not an lvalue anyway.
			case TokenKind::Ampersand: advance(); return wrapUnary(UnaryOp::AddressOf, location, parseCast());
			case TokenKind::Star: advance(); return wrapUnary(UnaryOp::Deref, location, parseCast());
			case TokenKind::Minus: advance(); return wrapUnary(UnaryOp::Negate, location, parseCast());
			case TokenKind::Bang: advance(); return wrapUnary(UnaryOp::LogicalNot, location, parseCast());
			case TokenKind::Tilde: advance(); return wrapUnary(UnaryOp::BitwiseNot, location, parseCast());
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

	Expr* Parser::parseCompoundLiteral(SourceLocation location, const Type* type, bool unsized)
	{
		Expr* parsed = parseInitializer(); // the current token is '{', so this is a brace list
		auto* list = dynamic_cast<ast::InitListExpr*>(parsed);
		if (!list)
			return nullptr;

		if (unsized && type->isArray())
		{
			i64 length = inferArrayLength(type->arrayElementType(), list);
			if (length <= 0)
			{
				_diagnostics.error(DiagId::ArraySizeRequired, location, "cannot infer the size of this array from its initializer: give it a size");
				return nullptr;
			}
			type = Type::makeArray(_arena, type->arrayElementType(), static_cast<u32>(length), type->isConst(), type->isVolatile());
		}

		if (_currentFunctionName.empty())
		{
			// Outside a function there is no frame to hold it, and C gives such a literal static storage anyway: a
			// variable of a generated name, put in the unit before the declaration that uses it, and the
			// expression is that variable.
			std::string text = std::format("__complit{}", _hoistedCount++);
			char* memory = static_cast<char*>(_arena.allocate(text.size() + 1, 1));
			std::memcpy(memory, text.c_str(), text.size() + 1);
			std::string_view name(memory, text.size());
			_hoistedDecls.push_back(_arena.create<ast::VarDecl>(location, name, type, list, ast::StorageClass::Static));
			return parsePostfixTail(_arena.create<ast::NameExpr>(location, name));
		}

		return parsePostfixTail(_arena.create<ast::CompoundLiteralExpr>(location, type, list));
	}

	Expr* Parser::parsePostfix()
	{
		Expr* primary = parsePrimary();
		if (!primary)
			return nullptr;
		return parsePostfixTail(primary);
	}

	Expr* Parser::parsePostfixTail(Expr* expr)
	{
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
					_diagnostics.error(DiagId::ExpectedMemberName, _current.location(), "expected member name after '.'");
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
					_diagnostics.error(DiagId::ExpectedMemberName, _current.location(), "expected member name after '->'");
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
				// The variadic builtins are syntax, not calls - __builtin_va_arg's second operand
				// is a type, and all four write through the __builtin_va_list the caller named.
				// Only treated as such in call position, so a program that uses one of these
				// names for something else of its own keeps working.
				if (std::optional<ast::VaOp> op = vaBuiltinFor(name); op && _next.is(TokenKind::LParen))
					return parseVaBuiltin(location, *op);
				// The machine builtins are the same idea one step lower: a name in call position that
				// stands for an instruction rather than for a function.
				if (std::optional<ast::MachineOp> machineOp = machineBuiltinFor(name); machineOp && _next.is(TokenKind::LParen))
					return parseMachineBuiltin(location, *machineOp);
				// `__func__` is the name of the function being parsed, as a string: the same thing a
				// literal spelled out by hand would be, made here because only the parser knows the name.
				if (!_currentFunctionName.empty() && (name == "__func__" || name == "__FUNCTION__"))
				{
					support::PooledString text = _lexer.stringPool().intern(_currentFunctionName);
					advance();
					return _arena.create<ast::StringLiteralExpr>(location, text);
				}
				advance();
				return _arena.create<ast::NameExpr>(location, name);
			}
			case TokenKind::LiteralInt:
			{
				auto value = _current.integralValue();
				bool isUnsigned = _current.isUnsigned();
				advance();
				return _arena.create<ast::IntLiteralExpr>(location, value, isUnsigned);
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
				_diagnostics.error(DiagId::ExpectedExpression, location, "expected expression but found '{}'",
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
			if (isAttributeStart())
			{
				parseAttributes(&specifiers.attributes);
				continue;
			}
			SourceLocation here = _current.location();
			ast::StorageClass storageClass = ast::StorageClass::None;
			switch (_current.kind())
			{
				case TokenKind::KwConst:
					advance();
					if (specifiers.isConst)
						_diagnostics.error(DiagId::DuplicateQualifier, here, "duplicate 'const'");
					specifiers.isConst = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwVolatile:
					advance();
					if (specifiers.isVolatile)
						_diagnostics.error(DiagId::DuplicateQualifier, here, "duplicate 'volatile'");
					specifiers.isVolatile = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwRestrict:
					advance();
					if (specifiers.isRestrict)
						_diagnostics.error(DiagId::DuplicateQualifier, here, "duplicate 'restrict'");
					specifiers.isRestrict = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwInline:
					advance();
					if (specifiers.isInline)
						_diagnostics.error(DiagId::DuplicateQualifier, here, "duplicate 'inline'");
					specifiers.isInline = true;
					specifiers.sawAny = true;
					continue;
				case TokenKind::KwInterrupt:
					advance();
					if (specifiers.isInterrupt)
						_diagnostics.error(DiagId::DuplicateQualifier, here, "duplicate '__interrupt'");
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
				_diagnostics.error(DiagId::ConflictingStorageClass, here, "cannot combine '{}' with '{}' on the same declaration",
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
					_diagnostics.error(DiagId::DuplicateQualifier, _current.location(), "duplicate 'const'");
				isConst = true;
				advance();
				continue;
			}
			if (check(TokenKind::KwVolatile))
			{
				if (isVolatile)
					_diagnostics.error(DiagId::DuplicateQualifier, _current.location(), "duplicate 'volatile'");
				isVolatile = true;
				advance();
				continue;
			}
			return;
		}
	}

	const Type* Parser::parseBaseType(bool leadingConst, bool leadingVolatile, bool& outLeadingRestrict,
		support::SourceLocation& outSpecifierLocation, bool* outRegisterRequest)
	{
		// The type-SPEC and its qualifiers, and nothing past them: every `*` belongs to the
		// declarator that follows, not to this, because which side of a star a qualifier sits on is
		// what decides whether it qualifies the pointer or the pointee (see parseDeclarator()).
		//
		// A qualifier may sit on either side of the type-spec - `const int` and `int const` are the
		// same type in C, and so are `volatile int` and `int volatile`.
		DeclSpecifiers leading = parseDeclSpecifiers();
		if (outRegisterRequest && leading.storageClass == ast::StorageClass::Register)
		{
			// The one legal case - see parser.h. Everything else about the specifier run is
			// checked below exactly as it is anywhere else.
			*outRegisterRequest = true;
		}
		else if (leading.storageClass != ast::StorageClass::None || leading.isInline || leading.isInterrupt)
		{
			if (outRegisterRequest)
				_diagnostics.error(DiagId::StorageClassOnParameter, leading.location,
					"'{}' is not allowed on a parameter - 'register' is the only storage-class specifier a parameter may carry",
					ast::storageClassName(leading.storageClass));
			else
				_diagnostics.error(DiagId::StorageClassInTypeName, leading.location,
					"a storage-class specifier is not allowed here - it belongs to a declaration, not to a type name");
		}
		outLeadingRestrict = leading.isRestrict;
		outSpecifierLocation = leading.location;

		const Type* base = parseTypeSpec();
		if (!base)
			return nullptr;

		bool isConst = leading.isConst || leadingConst;
		bool isVolatile = leading.isVolatile || leadingVolatile;
		parseQualifierRun(isConst, isVolatile); // trailing form: `int const`, `int volatile`
		if (isConst)
			base = Type::withConst(_arena, base);
		if (isVolatile)
			base = Type::withVolatile(_arena, base);
		return base;
	}

	const Type* Parser::parseTypeName()
	{
		return parseTypeName(false, false);
	}

	const Type* Parser::parseTypeName(bool leadingConst, bool leadingVolatile)
	{
		bool leadingRestrict = false;
		support::SourceLocation specifierLocation{};
		const Type* base = parseBaseType(leadingConst, leadingVolatile, leadingRestrict, specifierLocation);
		if (!base)
			return nullptr;

		// A type-name is a declarator with the name left out: `int (*)(int)` in a cast is exactly
		// `int (*f)(int)` without the `f`. Parsing both through one function is what keeps the two
		// spellings of one type from drifting apart.
		Declarator declarator = parseDeclarator(/*allowAbstract=*/true);
		if (!declarator.ok)
			return nullptr;
		if (!declarator.name.empty())
		{
			_diagnostics.error(DiagId::NameIsNotAType, declarator.nameLocation,
				"'{}' names something here, but this position takes a type rather than a declaration", declarator.name);
		}

		const Type* type = applyDeclarator(base, declarator, /*isParameter=*/false);
		if (type && leadingRestrict)
		{
			if (!type->isPointer())
				_diagnostics.error(DiagId::RestrictRequiresPointer, specifierLocation, "'restrict' requires a pointer type");
			else
				type = Type::withRestrict(_arena, type);
		}
		return type;
	}

	const Type* Parser::cappedToMachineWidth(SourceLocation location, std::string_view written,
		std::string_view actual, const Type* type)
	{
		_diagnostics.warning(DiagId::CappedTypeWidth, location,
			"'{}' is 32 bits here: this machine has no 64-bit type at all, so it is exactly '{}'",
			written, actual);
		return type;
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
			case TokenKind::KwLong:
			{
				advance();
				// The one place `long` does not introduce an integer at all.
				if (match(TokenKind::KwDouble))
					return cappedToMachineWidth(location, "long double", "float", &Type::Float);
				if (match(TokenKind::KwLong))
				{
					match(TokenKind::KwInt);
					return cappedToMachineWidth(location, "long long", "long", &Type::Long);
				}
				match(TokenKind::KwInt);
				return &Type::Long;
			}
			case TokenKind::KwDouble:
				advance();
				return cappedToMachineWidth(location, "double", "float", &Type::Float);
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
					if (match(TokenKind::KwLong))
					{
						match(TokenKind::KwInt);
						return cappedToMachineWidth(location,
							isUnsigned ? "unsigned long long" : "signed long long",
							isUnsigned ? "unsigned long" : "long",
							isUnsigned ? &Type::ULong : &Type::Long);
					}
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
				_diagnostics.error(DiagId::ExpectedTypeName, location, "expected type name but found '{}'",
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
		parseAttributes(nullptr);   // `struct __attribute__((...)) S`

		std::string_view tagName;
		if (check(TokenKind::Identifier))
		{
			tagName = _current.lexeme();
			advance();
		}
		else if (check(TokenKind::LBrace))
		{
			// `typedef struct { ... } T;` - the tag is optional in C when a body follows.
			tagName = makeAnonymousTag(isUnion ? "union" : "struct");
		}
		else
		{
			_diagnostics.error(DiagId::ExpectedTagName, _current.location(), "expected a {} tag name", isUnion ? "union" : "struct");
			advance();
			return nullptr;
		}

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
				_diagnostics.error(DiagId::RedefinitionOfTag, location, "redefinition of '{} {}'", isUnion ? "union" : "struct", tagName);

			std::vector<FieldDecl> fields;
			std::vector<Decl*> assertions; // `_Static_assert` inside the body: checked once the struct is complete
			while (!check(TokenKind::RBrace) && !isAtEnd())
			{
				if (isStaticAssertStart())
				{
					if (Decl* assertion = parseStaticAssert())
						assertions.push_back(assertion);
					else
						synchronizeStatement();
					continue;
				}
				parseAttributes(nullptr);
				SourceLocation fieldLoc = _current.location();
				bool fieldLeadingRestrict = false;
				SourceLocation fieldSpecifierLocation{};
				const Type* fieldBase = parseBaseType(false, false, fieldLeadingRestrict, fieldSpecifierLocation);
				if (!fieldBase)
				{
					synchronizeStatement(); // reuses the statement-level recovery: skip to ';' or '}'
					continue;
				}
				// One base type, then a comma-separated run of declarators - `int x, *y, a[3];` is
				// three fields. An ordinary declarator, so a field may be a function pointer -
				// `int (*handler)(int);` inside a struct is how a program builds a dispatch table.
				for (;;)
				{
					Declarator fieldDeclarator = parseDeclarator(/*allowAbstract=*/false);
					if (!fieldDeclarator.ok)
					{
						synchronizeStatement();
						break;
					}
					const Type* fieldType = applyDeclarator(fieldBase, fieldDeclarator, /*isParameter=*/false);
					if (!fieldType)
					{
						synchronizeStatement();
						break;
					}
					if (fieldType->isFunction())
					{
						// A struct holds objects, and a function is not one. The pointer is what a
						// program means here, and saying so is more useful than "has no size".
						_diagnostics.error(DiagId::FieldCannotBeFunction, fieldLoc,
							"a field cannot have function type - did you mean a pointer to one?");
						synchronizeStatement();
						break;
					}
					if (fieldLeadingRestrict)
					{
						if (!fieldType->isPointer())
							_diagnostics.error(DiagId::RestrictRequiresPointer, fieldSpecifierLocation, "'restrict' requires a pointer type");
						else
							fieldType = Type::withRestrict(_arena, fieldType);
					}
					parseAttributes(nullptr);   // `int x __attribute__((aligned(4)));`
					fields.push_back(FieldDecl{ fieldType, fieldDeclarator.name, fieldLoc });
					if (!match(TokenKind::Comma))
						break;
				}
				if (!expect(TokenKind::Semicolon, "';'"))
					synchronizeStatement();
			}
			expect(TokenKind::RBrace, "'}'");
			parseAttributes(nullptr);   // `struct S { ... } __attribute__((...))`
			decl->setFields(copyFieldsToArena(fields));
			_definedTags.push_back(decl);
			for (Decl* assertion : assertions)
				_definedTags.push_back(assertion);
		}

		return isUnion ? Type::makeUnion(_arena, decl) : Type::makeStruct(_arena, decl);
	}

	std::vector<Decl*> Parser::takeDefinedTags()
	{
		std::vector<Decl*> tags = std::move(_definedTags);
		_definedTags.clear();
		return tags;
	}

	std::string_view Parser::makeAnonymousTag(std::string_view kind)
	{
		// Not a valid C identifier on purpose: no program can write a tag that collides with it.
		std::string text = std::format("<anonymous {} {}>", kind, _anonymousTagCount++);
		char* memory = static_cast<char*>(_arena.allocate(text.size() + 1, 1));
		std::memcpy(memory, text.c_str(), text.size() + 1);
		return std::string_view(memory, text.size());
	}

	const Type* Parser::parseEnumTypeSpec()
	{
		SourceLocation location = _current.location();
		advance(); // 'enum'

		std::string_view tagName;
		if (check(TokenKind::Identifier))
		{
			tagName = _current.lexeme();
			advance();
		}
		else if (check(TokenKind::LBrace))
		{
			tagName = makeAnonymousTag("enum"); // `enum { A, B };` - the usual way to name a few constants
		}
		else
		{
			_diagnostics.error(DiagId::ExpectedEnumTagName, _current.location(), "expected an enum tag name after 'enum'");
			advance();
			return nullptr;
		}

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
				_diagnostics.error(DiagId::RedefinitionOfTag, location, "redefinition of 'enum {}'", tagName);

			std::vector<EnumeratorDecl> enumerators;
			while (!check(TokenKind::RBrace) && !isAtEnd())
			{
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(DiagId::ExpectedEnumeratorName, _current.location(), "expected an enumerator name");
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
			_definedTags.push_back(decl);
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
				std::vector<Decl*> decls = parseTypedefDecl();
				if (decls.empty())
					return nullptr;
				return _arena.create<ast::DeclStmt>(location, copyDeclsToArena(decls));
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
				if (isAttributeStart())
				{
					// `__attribute__((fallthrough));` is a statement of its own; in front of anything else the
					// attributes are read and dropped, since none of them has an effect on a local
					SourceLocation attributeLocation = _current.location();
					parseAttributes(nullptr);
					if (match(TokenKind::Semicolon))
						return _arena.create<ast::EmptyStmt>(attributeLocation);
					return parseStatement();
				}
				if (isStaticAssertStart())
				{
					SourceLocation location = _current.location();
					Decl* decl = parseStaticAssert();
					if (!decl)
						return nullptr;
					Decl* decls[1] = { decl };
					return _arena.create<ast::DeclStmt>(location, copyDeclsToArena(std::vector<Decl*>(decls, decls + 1)));
				}
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
			_diagnostics.error(DiagId::ExpectedLabelName, _current.location(), "expected a label name after 'goto'");
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
		bool leadingRestrict = false;
		SourceLocation specifierLocation{};
		_definedTags.clear();
		const Type* base = parseBaseType(specifiers.isConst, specifiers.isVolatile, leadingRestrict, specifierLocation);
		if (!base)
			return nullptr;
		std::vector<Decl*> tags = takeDefinedTags(); // before a declarator or a body can define more

		// `struct Foo { ... };` / `enum Bar { ... };` with no variable declarator: parseBaseType()
		// already registered/completed the tag (see parseStructTypeSpec()/parseEnumTypeSpec()), so
		// there is nothing left to declare - just wrap the tag itself in the DeclStmt.
		if (check(TokenKind::Semicolon) && (base->isAggregate() || base->isEnum()))
		{
			advance();
			if (tags.empty())
				tags.push_back(base->isAggregate() ? static_cast<Decl*>(base->structDecl()) : static_cast<Decl*>(base->enumDecl()));
			return _arena.create<ast::DeclStmt>(location, copyDeclsToArena(tags));
		}

		std::vector<Decl*> decls = std::move(tags); // the tags this declaration defined come first
		if (!parseInitDeclaratorList(location, base, specifiers, leadingRestrict, specifierLocation, decls))
			return nullptr;
		return _arena.create<ast::DeclStmt>(location, copyDeclsToArena(decls));
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
			std::vector<Decl*> parsed = parseExternalDecl();
			// The static variables that compound literals in this declaration stand for come first
			decls.insert(decls.end(), _hoistedDecls.begin(), _hoistedDecls.end());
			_hoistedDecls.clear();
			if (parsed.empty())
			{
				synchronizeDeclaration();
				continue;
			}
			decls.insert(decls.end(), parsed.begin(), parsed.end());
		}
		return _arena.create<TranslationUnit>(location, copyDeclsToArena(decls));
	}

	std::vector<Decl*> Parser::parseExternalDecl()
	{
		SourceLocation location = _current.location();

		if (check(TokenKind::KwTypedef))
			return parseTypedefDecl();

		if (check(TokenKind::KwInterruptVector))
		{
			Decl* decl = parseInterruptVectorDecl();
			return decl ? std::vector<Decl*>{ decl } : std::vector<Decl*>{};
		}

		if (isStaticAssertStart())
		{
			Decl* decl = parseStaticAssert();
			return decl ? std::vector<Decl*>{ decl } : std::vector<Decl*>{};
		}

		// Storage classes and `const` come first and belong to the DECLARATION, so they are read
		// here rather than inside parseTypeName() - which would have no one to hand a storage class
		// to, and rejects one for that reason. The `const` half is put back on the type afterwards.
		DeclSpecifiers specifiers = parseDeclSpecifiers();

		if (!isTypeSpecStart(_current))
		{
			_diagnostics.error(DiagId::ExpectedDeclaration, _current.location(), "expected a declaration but found '{}'",
				_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
			return {};
		}

		bool leadingRestrict = false;
		SourceLocation specifierLocation{};
		_definedTags.clear();
		const Type* base = parseBaseType(specifiers.isConst, specifiers.isVolatile, leadingRestrict, specifierLocation);
		if (!base)
			return {};
		std::vector<Decl*> tags = takeDefinedTags(); // before a declarator or a body can define more

		// `struct Foo { ... };` / `enum Bar { ... };` with no variable declarator - see
		// parseDeclStatement()'s identical check for the local-statement equivalent.
		if (check(TokenKind::Semicolon) && (base->isAggregate() || base->isEnum()))
		{
			advance();
			if (tags.empty())
				tags.push_back(base->isAggregate() ? static_cast<Decl*>(base->structDecl()) : static_cast<Decl*>(base->enumDecl()));
			return tags;
		}

		std::vector<Decl*> decls = std::move(tags); // the tags this declaration defined come first
		if (!parseInitDeclaratorList(location, base, specifiers, leadingRestrict, specifierLocation, decls))
			return {};
		return decls;
	}

	bool Parser::parseInitDeclaratorList(SourceLocation location, const Type* base, const DeclSpecifiers& specifiers,
		bool leadingRestrict, SourceLocation specifierLocation, std::vector<Decl*>& outDecls)
	{
		for (;;)
		{
			bool isFunctionDefinition = false;
			Decl* decl = finishDeclarator(location, base, specifiers, leadingRestrict, specifierLocation, &isFunctionDefinition);
			if (!decl)
				return false;
			outDecls.push_back(decl);

			// A function DEFINITION closes the declaration itself - a body, not a ';' - and C allows
			// no further declarator after one.
			if (isFunctionDefinition)
				return true;

			if (!match(TokenKind::Comma))
				break;
		}
		return expect(TokenKind::Semicolon, "';'");
	}

	// One declarator plus whatever follows it, shared by the file-scope and block-scope forms - they
	// differ only in which storage classes make sense, which is sema's call, not this one's.
	//
	// Whether this is a function declaration is decided by the DECLARATOR, exactly as in C: it is
	// one when the derived type is a function type. That is the whole disambiguation between
	// `int f(int)` and `int (*f)(int)`, and it falls out of applying the declarator rather than
	// needing a rule of its own.
	Decl* Parser::finishDeclarator(SourceLocation location, const Type* base, const DeclSpecifiers& specifiers,
		bool leadingRestrict, SourceLocation specifierLocation, bool* outIsFunctionDefinition)
	{
		AttributeList attributes = specifiers.attributes;
		parseAttributes(&attributes);   // `static int __attribute__((unused)) x;`
		Declarator declarator = parseDeclarator(/*allowAbstract=*/false);
		if (!declarator.ok)
			return nullptr;

		const DeclaratorSuffix* signature = nullptr;
		_allowUnsizedArray = true;
		_unsizedArrayPending = false;
		const Type* type = applyDeclarator(base, declarator, /*isParameter=*/false, &signature);
		_allowUnsizedArray = false;
		bool unsizedArray = _unsizedArrayPending;
		_unsizedArrayPending = false;
		if (!type)
			return nullptr;

		if (specifiers.isRestrict || leadingRestrict)
		{
			SourceLocation where = specifiers.isRestrict ? specifiers.location : specifierLocation;
			if (!type->isPointer())
				_diagnostics.error(DiagId::RestrictRequiresPointer, where, "'restrict' requires a pointer type");
			else
				type = Type::withRestrict(_arena, type);
		}

		support::PooledString asmLabel;
		for (;;)   // `__asm__("x")` and `__attribute__((...))` after a declarator, in either order and any number
		{
			if (isAsmLabelStart())
			{
				if (!parseAsmLabel(asmLabel))
					return nullptr;
			}
			else if (isAttributeStart())
				parseAttributes(&attributes);
			else
				break;
		}

		Decl* decl = nullptr;
		if (type->isFunction() && signature)
		{
			// The parameter NAMES the declaration keeps come from the suffix that made it a function -
			// the type itself holds only their types (type.h).
			decl = finishFunctionDecl(location, declarator.name, type, signature->params, signature->isVariadic,
				specifiers, outIsFunctionDefinition);
		}
		else
		{
			if (outIsFunctionDefinition)
				*outIsFunctionDefinition = false;
			decl = finishVarDecl(location, declarator.name, type, specifiers, unsizedArray);
		}
		if (decl && asmLabel)
			decl->setAsmLabel(asmLabel);
		if (decl && attributes.noReturn)
			decl->setNoReturn(true);
		return decl;
	}

	bool Parser::isAsmLabelStart() const noexcept
	{
		return _current.kind() == TokenKind::Identifier && (_current.lexeme() == "__asm__" || _current.lexeme() == "__asm") &&
			_next.is(TokenKind::LParen);
	}

	bool Parser::parseAsmLabel(support::PooledString& out)
	{
		advance(); // '__asm__'
		if (!expect(TokenKind::LParen, "'(' after '__asm__'"))
			return false;
		if (!check(TokenKind::LiteralString))
		{
			_diagnostics.error(DiagId::ExpectedAsmLabel, _current.location(), "expected a string literal: the name the assembler is to use");
			return false;
		}
		out = _current.stringValue();
		advance();
		return expect(TokenKind::RParen, "')'");
	}

	bool Parser::isStaticAssertStart() const noexcept
	{
		return _current.kind() == TokenKind::Identifier && _current.lexeme() == "_Static_assert" && _next.is(TokenKind::LParen);
	}

	Decl* Parser::parseStaticAssert()
	{
		SourceLocation location = _current.location();
		advance(); // '_Static_assert'
		if (!expect(TokenKind::LParen, "'(' after '_Static_assert'"))
			return nullptr;

		Expr* condition = parseTernary();
		if (!condition)
			return nullptr;

		support::PooledString message;
		bool hasMessage = false;
		if (match(TokenKind::Comma))
		{
			if (!check(TokenKind::LiteralString))
			{
				_diagnostics.error(DiagId::ExpectedStaticAssertMessage, _current.location(), "expected a string literal as the message of the static assertion");
				return nullptr;
			}
			message = _current.stringValue();
			hasMessage = true;
			advance();
		}

		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		return _arena.create<ast::StaticAssertDecl>(location, condition, message, hasMessage);
	}

	Decl* Parser::parseInterruptVectorDecl()
	{
		SourceLocation location = _current.location();
		advance(); // '__interrupt_vector'

		if (!expect(TokenKind::LParen, "'(' after '__interrupt_vector'"))
			return nullptr;

		// Any constant expression: a literal, an enum constant, a macro. sema folds and
		// range-checks it - the parser has no business knowing which numbers the machine has.
		SourceLocation numberLocation = _current.location();
		Expr* number = parseAssignment();
		if (!number)
			return nullptr;

		if (!expect(TokenKind::Comma, "',' between the vector number and its handler"))
			return nullptr;

		if (!check(TokenKind::Identifier))
		{
			_diagnostics.error(DiagId::ExpectedInterruptHandlerName, _current.location(), "expected the name of an '__interrupt' handler");
			return nullptr;
		}
		std::string_view handlerName = _current.lexeme();
		advance();

		if (!expect(TokenKind::RParen, "')'"))
			return nullptr;
		if (!expect(TokenKind::Semicolon, "';'"))
			return nullptr;

		return _arena.create<ast::InterruptVectorDecl>(location, handlerName, number, numberLocation);
	}

	std::vector<Decl*> Parser::parseTypedefDecl()
	{
		SourceLocation location = _current.location();
		advance(); // 'typedef'

		bool leadingRestrict = false;
		SourceLocation specifierLocation{};
		_definedTags.clear();
		const Type* base = parseBaseType(false, false, leadingRestrict, specifierLocation);
		if (!base)
			return {};
		std::vector<Decl*> result = takeDefinedTags(); // `typedef enum { A, B } T;` defines A and B too

		// A typedef is an ordinary declarator that names a TYPE instead of an object, which is what
		// makes `typedef int Handler(int);` name a function type and `typedef int (*Fn)(int);` name
		// a pointer to one - the same two declarators that would declare a function and a variable.
		Declarator declarator = parseDeclarator(/*allowAbstract=*/false);
		if (!declarator.ok)
			return {};
		const Type* underlyingType = applyDeclarator(base, declarator, /*isParameter=*/false);
		if (!underlyingType)
			return {};
		if (leadingRestrict)
		{
			if (!underlyingType->isPointer())
				_diagnostics.error(DiagId::RestrictRequiresPointer, specifierLocation, "'restrict' requires a pointer type");
			else
				underlyingType = Type::withRestrict(_arena, underlyingType);
		}
		std::string_view name = declarator.name;
		parseAttributes(nullptr);

		if (!expect(TokenKind::Semicolon, "';'"))
			return {};

		_typedefTable[name] = underlyingType; // makes `name` usable as a type-spec from here on - see isTypeSpecStart(const Token&)/parseTypeSpec()
		result.push_back(_arena.create<ast::TypedefDecl>(location, name, underlyingType));
		return result;
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
			_diagnostics.error(DiagId::EmptyInitializerList, location, "an initializer list needs at least one value");
			advance(); // '}'
			return nullptr;
		}

		std::vector<Expr*> elements;
		while (true)
		{
			// an element is itself an `initializer` - nesting - possibly with a designation in front of it
			Expr* element = (check(TokenKind::Dot) || check(TokenKind::LBracket)) ? parseDesignatedInit() : parseInitializer();
			if (!element)
				return nullptr;
			elements.push_back(element);

			if (!check(TokenKind::Comma))
				break;
			advance(); // ','
			if (check(TokenKind::RBrace))
				break; // `{ 1, 2, }`
		}

		if (!expect(TokenKind::RBrace, "'}'"))
			return nullptr;

		return _arena.create<ast::InitListExpr>(location, copyArgsToArena(elements));
	}

	namespace
	{
		bool foldArraySizeExpr(const Expr* expr, i64& out);   // defined below, with the array declarators
	}

	bool Parser::isAttributeStart() const noexcept
	{
		return _current.kind() == TokenKind::Identifier && (_current.lexeme() == "__attribute__" || _current.lexeme() == "__attribute") &&
			_next.is(TokenKind::LParen);
	}

	namespace
	{
		// Attributes that only tell a compiler how to check or optimize, and that this one has no use for: accepted
		// without a word, since headers written for GCC are full of them.
		bool isHarmlessAttribute(std::string_view name)
		{
			static constexpr std::string_view kKnown[] = {
				"unused", "used", "fallthrough", "deprecated", "noinline", "always_inline", "cold", "hot", "pure", "const",
				"nonnull", "warn_unused_result", "format", "malloc", "visibility", "returns_nonnull", "nothrow", "leaf",
				"artificial", "gnu_inline", "may_alias", "flatten", "optimize", "no_instrument_function",
			};
			for (std::string_view known : kKnown)
				if (name == known)
					return true;
			return false;
		}
	}

	void Parser::parseAttributes(AttributeList* sink)
	{
		while (isAttributeStart())
		{
			advance(); // '__attribute__'
			if (!expect(TokenKind::LParen, "'(' after '__attribute__'") || !expect(TokenKind::LParen, "'((' after '__attribute__'"))
				return;

			while (!check(TokenKind::RParen) && !isAtEnd())
			{
				SourceLocation where = _current.location();
				std::string_view name = _current.lexeme();
				const bool looksLikeName = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_');
				if (!looksLikeName)
				{
					_diagnostics.error(DiagId::ExpectedAttributeName, where, "expected an attribute name");
					return;
				}
				advance();
				// `__noreturn__` and `noreturn` are the same attribute
				if (name.size() > 4 && name.starts_with("__") && name.ends_with("__"))
					name = name.substr(2, name.size() - 4);

				i64 argument = 0;
				bool hasArgument = false;
				bool argumentOk = false;
				if (match(TokenKind::LParen))
				{
					hasArgument = true;
					if (name == "aligned")
					{
						Expr* expression = parseTernary();
						argumentOk = expression && foldArraySizeExpr(expression, argument);
					}
					else
					{
						// Arguments this compiler has no use for: read past them, brackets balanced
						int depth = 1;
						while (!isAtEnd() && depth > 0)
						{
							if (check(TokenKind::LParen))
								++depth;
							else if (check(TokenKind::RParen) && --depth == 0)
								break;
							advance();
						}
					}
					if (!expect(TokenKind::RParen, "')'"))
						return;
				}

				if (name == "noreturn")
				{
					if (sink)
						sink->noReturn = true;
					else
						_diagnostics.warning(DiagId::AttributeIgnored, where, "attribute 'noreturn' ignored: it applies to a function");
				}
				else if (name == "aligned")
				{
					if (hasArgument && (!argumentOk || argument < 1 || argument > 65536 || (argument & (argument - 1)) != 0))
					{
						_diagnostics.error(DiagId::InvalidAttributeArgument, where, "the alignment of an 'aligned' attribute must be a power of two, from 1 to 65536");
					}
					else if (!hasArgument || argument > 4)
					{
						// A section starts on a 4-byte boundary wherever the linker puts it, so a larger alignment could only be
						// kept relative to the section's own start - not an address anyone can rely on. Said, not pretended.
						_diagnostics.warning(DiagId::AttributeIgnored, where,
							"attribute 'aligned' ignored above 4 bytes: the linker places every section on a 4-byte boundary, so a larger alignment cannot be kept");
					}
					// up to 4 is what every scalar already has
				}
				else if (name == "packed")
				{
					_diagnostics.error(DiagId::PackedNotSupported, where,
						"attribute 'packed' is not supported: the machine faults on a 16- or 32-bit access that is not aligned, so a member cannot sit off its natural boundary");
				}
				else if (!isHarmlessAttribute(name))
				{
					_diagnostics.warning(DiagId::AttributeIgnored, where, "attribute '{}' ignored", name);
				}

				if (!match(TokenKind::Comma))
					break;
			}
			if (!expect(TokenKind::RParen, "')'") || !expect(TokenKind::RParen, "'))'"))
				return;
		}
	}

	// `.name` and `[index]` in any run, then `=`, then the value: `.pos.x = 1`, `[2] = 5`, `[1].name = "a"`.
	Expr* Parser::parseDesignatedInit()
	{
		SourceLocation location = _current.location();
		std::vector<ast::Designator> designators;
		while (check(TokenKind::Dot) || check(TokenKind::LBracket))
		{
			ast::Designator designator;
			designator.location = _current.location();
			if (match(TokenKind::Dot))
			{
				if (!check(TokenKind::Identifier))
				{
					_diagnostics.error(DiagId::ExpectedDesignator, _current.location(), "expected a member name after '.' in a designator");
					return nullptr;
				}
				designator.isField = true;
				designator.field = _current.lexeme();
				advance();
			}
			else
			{
				advance(); // '['
				designator.index = parseTernary();
				if (!designator.index || !expect(TokenKind::RBracket, "']'"))
					return nullptr;
			}
			designators.push_back(designator);
		}
		if (!expect(TokenKind::Equal, "'=' after a designator"))
			return nullptr;

		Expr* value = parseInitializer();
		if (!value)
			return nullptr;

		void* memory = _arena.allocate(sizeof(ast::Designator) * designators.size(), alignof(ast::Designator));
		if (!memory)
		{
			_diagnostics.error(DiagId::OutOfMemory, location, "out of memory allocating a designation");
			return nullptr;
		}
		ast::Designator* stored = static_cast<ast::Designator*>(memory);
		for (usize i = 0; i < designators.size(); ++i)
			stored[i] = designators[i];
		return _arena.create<ast::DesignatedInitExpr>(location, std::span<const ast::Designator>(stored, designators.size()), value);
	}

	i64 Parser::inferArrayLength(const Type* element, const Expr* initializer)
	{
		if (!element || !initializer)
			return -1;
		// `char s[] = "abc"` - the characters and the terminating NUL
		if (const auto* text = dynamic_cast<const ast::StringLiteralExpr*>(initializer))
		{
			bool isCharElement = element->isChar() || element->isSChar() || element->isUChar();
			return isCharElement ? static_cast<i64>(text->value().view().size()) + 1 : -1;
		}
		if (const auto* list = dynamic_cast<const ast::InitListExpr*>(initializer))
		{
			// An array of arrays takes one brace group per row (`int m[][2] = { {1,2}, {3,4} }`); a flat
			// list would need the row width to be divided by, which this subset's initializers do not do.
			//
			// A designator can move the position: `{ [5] = 1 }` holds six, and the count is the highest position
			// reached plus one. Only an index that folds to a constant here can be counted.
			i64 position = 0;
			i64 length = 0;
			for (const Expr* item : list->elements())
			{
				const Expr* value = item;
				if (const auto* designated = dynamic_cast<const ast::DesignatedInitExpr*>(item))
				{
					const ast::Designator& first = designated->designators().front();
					i64 index = 0;
					if (first.isField || !foldArraySizeExpr(first.index, index) || index < 0)
						return -1;
					position = index;
					value = designated->designators().size() == 1 ? designated->value() : nullptr;
				}
				if (element->isArray() && value && !dynamic_cast<const ast::InitListExpr*>(value))
					return -1;
				++position;
				length = position > length ? position : length;
			}
			return length;
		}
		return -1;
	}

	Decl* Parser::finishVarDecl(SourceLocation location, std::string_view name, const Type* type,
		const DeclSpecifiers& specifiers, bool unsizedArray)
	{
		if (specifiers.isInterrupt)
		{
			// `__interrupt` describes how a function is ENTERED AND LEFT - no arguments, every
			// register restored, `iret` at the end. A variable has none of that to describe.
			_diagnostics.error(DiagId::InterruptOnNonFunction, specifiers.location, "'__interrupt' is only allowed on a function");
		}
		Expr* initializer = nullptr;
		if (match(TokenKind::Equal))
		{
			// parseInitializer(), not parseAssignment(): a declarator is the one position §3's
			// grammar allows a brace initializer-list in.
			initializer = parseInitializer();
		}

		if (unsizedArray && type && type->isArray())
		{
			const Type* element = type->arrayElementType();
			i64 length = inferArrayLength(element, initializer);
			if (length > 0)
			{
				type = Type::makeArray(_arena, element, static_cast<u32>(length), type->isConst(), type->isVolatile());
			}
			else if (!initializer && specifiers.storageClass == ast::StorageClass::Extern)
			{
				// `extern int table[];` names an array some other unit defines. This one needs no size to
				// index it, and it must not invent one, so the type keeps the size 0 that means "not known here":
				// it decays to a pointer and can be subscripted, sizeof refuses it, and a definition of the
				// same name later in the unit (or an earlier one) gives it its size.
			}
			else
			{
				if (!initializer || !dynamic_cast<const ast::InitListExpr*>(initializer))
					_diagnostics.error(DiagId::ArraySizeRequired, location,
						"array size is required here: give it a size, or an initializer to take the size from");
				else
					_diagnostics.error(DiagId::ArraySizeRequired, location,
						"cannot infer the size of this array from its initializer: give it a size");
				type = Type::makePointer(_arena, element); // recovers as a pointer, as the other size errors do
			}
		}

		if (specifiers.isInline)
			_diagnostics.error(DiagId::InlineOnNonFunction, specifiers.location, "'inline' is only allowed on a function");

		return _arena.create<ast::VarDecl>(location, name, type, initializer, specifiers.storageClass);
	}

	Decl* Parser::finishFunctionDecl(SourceLocation location, std::string_view name, const Type* functionType,
		std::span<const Param> params, bool isVariadic, const DeclSpecifiers& specifiers, bool* outIsDefinition)
	{
		// The parameter list has already been read, as part of the declarator that made this a
		// function declaration in the first place - `int f(int)` and `int (*f)(int)` differ only in
		// the declarator, and only it can tell them apart. What is left is the body or the `;`.
		const ast::FunctionTypeInfo* info = functionType ? functionType->functionInfo() : nullptr;
		const Type* returnType = info ? info->returnType : nullptr;

		CompoundStmt* body = nullptr;
		if (check(TokenKind::LBrace))
		{
			_currentFunctionName = name;
			Stmt* bodyStmt = parseCompoundStatement();
			_currentFunctionName = {};
			if (!bodyStmt)
				return nullptr;
			body = static_cast<CompoundStmt*>(bodyStmt); // parseCompoundStatement() only ever returns a CompoundStmt* (or nullptr)
		}
		if (outIsDefinition)
			*outIsDefinition = body != nullptr;

		if (specifiers.storageClass == ast::StorageClass::Auto)
		{
			// `auto` means automatic STORAGE, which a function does not have. Rejected here rather
			// than in sema because there is nothing type-dependent about it.
			_diagnostics.error(DiagId::AutoOnFunction, specifiers.location, "'auto' is not allowed on a function");
		}
		if (specifiers.storageClass == ast::StorageClass::Register)
		{
			// Same: `register` asks for a kind of storage a function does not have either. Only
			// `static`, `extern` and `inline` say anything about one.
			_diagnostics.error(DiagId::RegisterOnFunction, specifiers.location, "'register' is not allowed on a function");
		}
		if (specifiers.isInline && !body)
			_diagnostics.error(DiagId::InlineOnPrototype, specifiers.location, "'inline' is only meaningful on a function definition, not on a prototype");
		if (specifiers.isInterrupt && specifiers.isInline)
		{
			// Nothing calls an interrupt handler, so there is no call site to inline it into.
			_diagnostics.error(DiagId::InterruptWithInline, specifiers.location, "'__interrupt' cannot be combined with 'inline'");
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
			// reason is not merely conformance: __builtin_va_start() names the last fixed parameter
			// to find where the variadic arguments begin (docs/09-Variadic-Convention.md), so a
			// list with no fixed parameter has nothing such a call could ever name.
			_diagnostics.error(DiagId::EllipsisNeedsNamedParameter, _current.location(), "'...' requires at least one named parameter before it");
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
					_diagnostics.error(DiagId::EllipsisMustBeLast, _current.location(), "'...' must be the last entry in a parameter list");
					return false;
				}
				return true;
			}

			parseAttributes(nullptr);
			SourceLocation location = _current.location();
			bool leadingRestrict = false;
			bool isRegister = false;
			SourceLocation specifierLocation{};
			const Type* base = parseBaseType(false, false, leadingRestrict, specifierLocation, &isRegister);
			if (!base)
				return false;

			// A parameter's declarator is an ordinary one, so `void apply(int (*f)(int))` works for
			// the same reason `int (*f)(int);` does. `isParameter` is what applies C's decay rules:
			// an array parameter is a pointer to its first element and a function parameter is a
			// pointer to the function, because there is nothing else either could be passed as.
			//
			// Abstract is allowed here because a prototype may leave a parameter unnamed, and a
			// type-name must: the `(int)` in `int (*)(int)` has nowhere to put a name. sema is what
			// requires one in a DEFINITION, where the body would have no way to refer to it.
			parseAttributes(nullptr);   // `int __attribute__((unused)) x`
			Declarator declarator = parseDeclarator(/*allowAbstract=*/true);
			if (!declarator.ok)
				return false;
			const Type* type = applyDeclarator(base, declarator, /*isParameter=*/true);
			if (!type)
				return false;
			if (leadingRestrict)
			{
				if (!type->isPointer())
					_diagnostics.error(DiagId::RestrictRequiresPointer, specifierLocation, "'restrict' requires a pointer type");
				else
					type = Type::withRestrict(_arena, type);
			}

			parseAttributes(nullptr);   // `int x __attribute__((unused))`
			outParams.push_back(Param{ type, declarator.name, location, isRegister });
		} while (match(TokenKind::Comma));

		return true;
	}

	// ---- array declarator suffix (see parser.h's header comment on this method for the parameter-
	// decay rule and why every dimension outside a parameter declarator must be an INT_LITERAL) ------

	// ---- declarators ------------------------------------------------------------------------------

	bool Parser::nestedDeclaratorFollows() const noexcept
	{
		// Called with `_current` on the '('. What follows decides:
		//   `(` `*` ...      a pointer declarator - `int (*f)(void)`
		//   `(` `(` ...      another group - `int ((*f))(void)`
		//   `(` IDENT ...    a name, unless that identifier is a typedef - `int (f)(void)`
		//   `(` `)` ...      an EMPTY PARAMETER LIST - `int f()`
		//   `(` type ...     a parameter list - `int f(int)`
		// The identifier case is why this consults the typedef table rather than the token alone:
		// `int (T)` is a function taking a T, while `int (f)` declares f.
		if (_next.is(TokenKind::Star) || _next.is(TokenKind::LParen))
			return true;
		if (_next.is(TokenKind::Identifier))
			return !_typedefTable.contains(_next.lexeme());
		return false;
	}

	namespace
	{
		// The value of an integer constant expression made of literals - what an array size may be
		// (`[64]`, `[BUF + 8]`, `[4 * 512]`, `[1 << 6]`, `[(2 + 3) * 4]` - macros have already expanded to
		// literals by now). Anything that needs a symbol (sizeof, an enumerator, a variable) is not
		// folded here: the parser has none of that information yet, so it is reported, not guessed.
		bool foldArraySizeExpr(const Expr* expr, i64& out)
		{
			if (const auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(expr))
			{
				if (literal->value() > 0x7FFFFFFFFFFFFFFFull)
					return false;
				out = static_cast<i64>(literal->value());
				return true;
			}
			if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(expr))
			{
				i64 operand = 0;
				if (!foldArraySizeExpr(unary->operand(), operand))
					return false;
				switch (unary->op())
				{
					case UnaryOp::Negate: out = -operand; return true;
					case UnaryOp::BitwiseNot: out = ~operand; return true;
					case UnaryOp::LogicalNot: out = operand == 0 ? 1 : 0; return true;
					default: return false;
				}
			}
			if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(expr))
			{
				i64 left = 0, right = 0;
				if (!foldArraySizeExpr(binary->lhs(), left) || !foldArraySizeExpr(binary->rhs(), right))
					return false;
				// Anything past ~2^40 is not an array size and would only risk overflowing the i64 below.
				if (left > (1ll << 40) || left < -(1ll << 40) || right > (1ll << 40) || right < -(1ll << 40))
					return false;
				switch (binary->op())
				{
					case BinaryOp::Add: out = left + right; return true;
					case BinaryOp::Sub: out = left - right; return true;
					case BinaryOp::Mul: out = left * right; return true;
					case BinaryOp::Div: if (right == 0) return false; out = left / right; return true;
					case BinaryOp::Mod: if (right == 0) return false; out = left % right; return true;
					case BinaryOp::Shl: if (right < 0 || right > 40) return false; out = left << right; return true;
					case BinaryOp::Shr: if (right < 0 || right > 40) return false; out = left >> right; return true;
					case BinaryOp::BitAnd: out = left & right; return true;
					case BinaryOp::BitOr: out = left | right; return true;
					case BinaryOp::BitXor: out = left ^ right; return true;
					default: return false;
				}
			}
			return false;
		}
	}

	void Parser::parseDeclaratorSuffixes(Declarator& declarator)
	{
		for (;;)
		{
			if (check(TokenKind::LBracket))
			{
				DeclaratorSuffix suffix;
				suffix.location = _current.location();
				advance(); // '['
				if (check(TokenKind::RBracket))
				{
					// Left unsized. Legal only as a parameter's outermost dimension, which decays to
					// a pointer anyway - applyDeclarator() is what knows whether this is one.
					suffix.hasArraySize = false;
				}
				else
				{
					SourceLocation sizeLocation = _current.location();
					std::string_view firstToken = _current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme();
					Expr* sizeExpr = parseAssignment();
					i64 size = 0;
					if (!sizeExpr || !foldArraySizeExpr(sizeExpr, size))
					{
						_diagnostics.error(DiagId::ExpectedArraySize, sizeLocation,
							"expected an integer constant for the array size (a literal, or arithmetic on literals) but found '{}'", firstToken);
						// Resync on the ']' so one bad dimension does not cost the declarator the rest.
						while (!check(TokenKind::RBracket) && !check(TokenKind::Semicolon) && !check(TokenKind::Comma) && !isAtEnd())
							advance();
					}
					else if (size <= 0 || size > 0xFFFFFFFFll)
						_diagnostics.error(DiagId::InvalidArraySize, sizeLocation, "array size must be a positive integer that fits in 32 bits");
					else
					{
						suffix.arraySize = static_cast<u32>(size);
						suffix.hasArraySize = true;
					}
				}
				expect(TokenKind::RBracket, "']'");
				declarator.suffixes.push_back(std::move(suffix));
				continue;
			}

			if (check(TokenKind::LParen))
			{
				DeclaratorSuffix suffix;
				suffix.isFunction = true;
				suffix.location = _current.location();
				advance(); // '('
				if (!parseParamList(suffix.params, suffix.isVariadic))
				{
					declarator.ok = false;
					return;
				}
				if (!expect(TokenKind::RParen, "')'"))
				{
					declarator.ok = false;
					return;
				}
				declarator.suffixes.push_back(std::move(suffix));
				continue;
			}

			return;
		}
	}

	Parser::Declarator Parser::parseDeclarator(bool allowAbstract)
	{
		Declarator declarator;

		// The leading `*`s, left to right, each taking whatever qualifiers follow IT - `int * const *`
		// is a pointer to a const pointer to int, and which `*` the `const` belongs to is decided by
		// position alone.
		while (match(TokenKind::Star))
		{
			PointerLevel level;
			parseQualifierRun(level.isConst, level.isVolatile);
			while (check(TokenKind::KwRestrict))
			{
				if (level.isRestrict)
					_diagnostics.error(DiagId::DuplicateQualifier, _current.location(), "duplicate 'restrict'");
				level.isRestrict = true;
				advance();
				parseQualifierRun(level.isConst, level.isVolatile); // `* restrict const` is one run
			}
			declarator.pointers.push_back(level);
		}

		if (check(TokenKind::LParen) && nestedDeclaratorFollows())
		{
			advance(); // '('
			declarator.nested = std::make_unique<Declarator>(parseDeclarator(allowAbstract));
			if (!declarator.nested->ok)
			{
				declarator.ok = false;
				return declarator;
			}
			if (!expect(TokenKind::RParen, "')'"))
			{
				declarator.ok = false;
				return declarator;
			}
			// The name lives at the innermost level - in `int (*f)(void)` it is inside the
			// parentheses - so carry it up. Every consumer then reads one field rather than knowing
			// how deep the parentheses went.
			declarator.name = declarator.nested->name;
			declarator.nameLocation = declarator.nested->nameLocation;
		}
		else if (check(TokenKind::Identifier))
		{
			declarator.name = _current.lexeme();
			declarator.nameLocation = _current.location();
			advance();
		}
		else if (!allowAbstract)
		{
			_diagnostics.error(DiagId::ExpectedIdentifier, _current.location(), "expected an identifier in declaration");
			declarator.ok = false;
			return declarator;
		}

		parseDeclaratorSuffixes(declarator);
		return declarator;
	}

	const Type* Parser::applyDeclarator(const Type* base, const Declarator& declarator, bool isParameter,
		const DeclaratorSuffix** outSignature)
	{
		if (!base)
			return nullptr;

		// Set when a function derivation is applied, cleared by anything applied after it - so when
		// the recursion ends it holds the suffix that produced the final type, and only then.
		auto note = [&](const DeclaratorSuffix* suffix) { if (outSignature) *outSignature = suffix; };
		note(nullptr);

		// 1. The leading `*`s. They are the outermost construct whenever no parentheses separate
		//    them from the name, which is why they go first: in `int *f[3]`, `f` is an array of
		//    pointers, so the pointer has to exist before the array wraps it.
		for (const PointerLevel& level : declarator.pointers)
		{
			base = Type::makePointer(_arena, base);
			if (level.isConst)
				base = Type::withConst(_arena, base);
			if (level.isVolatile)
				base = Type::withVolatile(_arena, base);
			if (level.isRestrict)
				base = Type::withRestrict(_arena, base);
			note(nullptr);
		}

		// 2. The suffixes, RIGHT to left. `int f[2][3]` groups as `(f[2])[3]`, so `[3]` is the
		//    outermost and `f` ends up an array of 2 arrays of 3.
		for (usize i = declarator.suffixes.size(); i-- > 0;)
		{
			const DeclaratorSuffix& suffix = declarator.suffixes[i];
			// Only the OUTERMOST dimension of a parameter decays, and only when nothing in this
			// declarator wraps it - `int a[3]` is a parameter that decays, `int (*a)[3]` is not.
			bool outermost = (i == 0) && declarator.pointers.empty() && !declarator.nested;

			if (suffix.isFunction)
			{
				if (base->isFunction())
					_diagnostics.error(DiagId::FunctionReturnsFunction, suffix.location, "a function cannot return a function");
				else if (base->isArray())
					_diagnostics.error(DiagId::FunctionReturnsArray, suffix.location, "a function cannot return an array");

				std::vector<const Type*> paramTypes;
				paramTypes.reserve(suffix.params.size());
				for (const Param& param : suffix.params)
					paramTypes.push_back(param.type);
				base = Type::makeFunction(_arena, base, paramTypes, suffix.isVariadic);
				note(&suffix);
				continue;
			}

			if (base->isVoid())
				_diagnostics.error(DiagId::InvalidArrayElementType, suffix.location, "array has invalid element type 'void'");
			else if (base->isFunction())
				_diagnostics.error(DiagId::InvalidArrayElementType, suffix.location, "array has invalid element type: a function");

			if (!suffix.hasArraySize)
			{
				// `int a[]` is legal for a parameter's outermost dimension and nowhere else, because
				// that dimension is not part of the type in the first place - see the decay below.
				if (_allowUnsizedArray && !isParameter && i == 0 && !declarator.nested)
				{
					// `int a[] = ...`: the size comes from the initializer, which finishVarDecl() reads next
					_unsizedArrayPending = true;
					base = Type::makeArray(_arena, base, 0);
					note(nullptr);
					continue;
				}
				if (!(isParameter && outermost))
				{
					_diagnostics.error(DiagId::ArraySizeRequired, suffix.location,
						"array size is required here (only a variable's initializer can give it)");
				}
				base = Type::makePointer(_arena, base); // decays, or recovers as a pointer
				note(nullptr);
				continue;
			}
			base = Type::makeArray(_arena, base, suffix.arraySize);
			note(nullptr);
		}

		// 3. Whatever the parentheses enclosed, applied to everything built so far. This is the step
		//    that makes `int (*f)(void)` a pointer to a function rather than a function returning a
		//    pointer: the `(void)` suffix above has already turned `int` into `int(void)`, and only
		//    now does the nested `*` see it.
		if (declarator.nested)
			return applyDeclarator(base, *declarator.nested, isParameter, outSignature);

		// C's parameter decay, applied once here rather than inside the suffix loop so that it also
		// catches a type that arrived already an array or a function - `typedef int A[3]; void f(A a)`
		// declares a pointer just as surely as `void f(int a[3])` does, and no suffix was written.
		// An array parameter is a pointer to its first element and a function parameter is a pointer
		// to the function, because there is nothing else either could be passed as.
		if (isParameter && base->isArray())
		{
			note(nullptr);
			return Type::makePointer(_arena, base->arrayElementType());
		}
		if (isParameter && base->isFunction())
		{
			note(nullptr);
			return Type::makePointer(_arena, base);
		}
		return base;
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
