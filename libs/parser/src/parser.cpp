#include <ceresc/parser/parser.h>

namespace ceresc::parser
{
	using support::SourceLocation;

	Parser::Parser(lexer::Lexer& lexer, support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept :
		_lexer(lexer), _arena(arena), _diagnostics(diagnostics), _current(lexer.next()), _next(lexer.next())
	{}

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
			_diagnostics.error(_current.location(), "out of memory allocating call arguments");
			return {};
		}

		Expr** stored = static_cast<Expr**>(memory);
		for (usize i = 0; i < args.size(); ++i)
			stored[i] = args[i];
		return std::span<Expr* const>(stored, args.size());
	}

	// ---- Expressions (§7's precedence table) --------------------------------------------------

	Expr* Parser::parseExpression()
	{
		return parseAssignment();
	}

	Expr* Parser::parseAssignment()
	{
		Expr* lhs = parseBinary(2); // 2 = lowest binary level (||); climbs up through 11

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
		if (check(TokenKind::LParen) && isTypeSpecStart(_next.kind()))
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
			default: return parsePostfix();
		}
	}

	Expr* Parser::parseSizeof(SourceLocation location)
	{
		advance(); // 'sizeof'

		if (check(TokenKind::LParen) && isTypeSpecStart(_next.kind()))
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

	// ---- type-name (Fase 2 subset: primitives + signed/unsigned/short/long, no struct/enum/typedef
	// yet - see type.h) ---------------------------------------------------------------------------

	const Type* Parser::parseTypeName()
	{
		const Type* base = parseTypeSpec();
		if (!base)
			return nullptr;

		while (match(TokenKind::Star))
			base = Type::makePointer(_arena, base);
		return base;
	}

	const Type* Parser::parseTypeSpec()
	{
		SourceLocation location = _current.location();
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
			default:
				_diagnostics.error(location, "expected type name but found '{}'",
					_current.isEndOfFile() ? std::string_view("end of file") : _current.lexeme());
				advance(); // guarantee forward progress, same reasoning as parsePrimary()'s default case
				return nullptr;
		}
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
