#include <ceresc/sema/sema.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/ast_printer.h>

#include <algorithm>
#include <span>

namespace ceresc::sema
{
	using ast::Type;
	using ast::TypeKind;
	using ast::Expr;
	using ast::Stmt;
	using ast::Decl;
	using ast::UnaryOp;
	using ast::BinaryOp;
	using ast::Param;
	using ast::FieldDecl;
	using ast::EnumeratorDecl;
	using ast::AstPrinter;

	namespace
	{
		std::string typeName(const Type* type) { return AstPrinter::typeName(type); }
	}

	// ---- construction / entry point -----------------------------------------------------------

	Sema::Sema(support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept :
		_arena(arena), _diagnostics(diagnostics)
	{}

	bool Sema::check(ast::TranslationUnit& unit)
	{
		_scopes.clear();
		_globalScope = nullptr;
		_currentFunctionReturnType = nullptr;
		_currentFunctionLabels.clear();
		_loopDepth = 0;
		_switchStack.clear();

		usize errorsBefore = _diagnostics.errorCount();

		pushScope();
		_globalScope = _scopes.back().get();
		unit.accept(*this);
		popScope();

		return _diagnostics.errorCount() == errorsBefore;
	}

	// ---- scope helpers --------------------------------------------------------------------------

	Scope& Sema::pushScope()
	{
		Scope* parent = _scopes.empty() ? nullptr : _scopes.back().get();
		_scopes.push_back(std::make_unique<Scope>(parent));
		return *_scopes.back();
	}

	void Sema::popScope()
	{
		if (!_scopes.empty())
			_scopes.pop_back();
	}

	Scope& Sema::currentScope() noexcept
	{
		return *_scopes.back();
	}

	bool Sema::declareSymbol(const Symbol& symbol)
	{
		if (!currentScope().declare(symbol))
		{
			_diagnostics.error(symbol.location, "redefinition of '{}'", symbol.name);
			return false;
		}
		return true;
	}

	// ---- expression / statement dispatch helpers -------------------------------------------------

	const Type* Sema::checkExpr(Expr* expr)
	{
		if (!expr)
			return nullptr;
		expr->accept(*this);
		return _lastExprType;
	}

	void Sema::checkStmt(Stmt* stmt)
	{
		if (stmt)
			stmt->accept(*this);
	}

	// ---- small type-system helpers ---------------------------------------------------------------

	const Type* Sema::errorRecoveryType() noexcept { return &Type::Int; }

	bool Sema::isArithmeticType(const Type* type) noexcept
	{
		if (!type)
			return false;
		switch (type->kind())
		{
			case TypeKind::Bool: case TypeKind::Char: case TypeKind::UChar: case TypeKind::SChar:
			case TypeKind::Short: case TypeKind::UShort: case TypeKind::Int: case TypeKind::UInt:
			case TypeKind::Long: case TypeKind::ULong: case TypeKind::Float:
			case TypeKind::Enum: // an enum is an integer type in real C - see integerPromote()
				return true;
			default:
				return false;
		}
	}

	bool Sema::isIntegerType(const Type* type) noexcept
	{
		return isArithmeticType(type) && type->kind() != TypeKind::Float;
	}

	bool Sema::isScalarType(const Type* type) noexcept
	{
		return isArithmeticType(type) || (type && type->isPointer());
	}

	bool Sema::isLValue(const Expr* expr)
	{
		if (!expr)
			return false;
		if (const auto* name = dynamic_cast<const ast::NameExpr*>(expr))
		{
			// A name is only an lvalue if it names an object (a variable or parameter) - an enum
			// constant or a bare function name resolves to a value/designator, not something with
			// storage, so `enumConst = 1` and `funcName = 1` must not type-check as assignments.
			Symbol* symbol = currentScope().lookup(name->name());
			return symbol && (symbol->kind == SymbolKind::Variable || symbol->kind == SymbolKind::Parameter);
		}
		if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(expr))
			return unary->op() == UnaryOp::Deref;
		if (dynamic_cast<const ast::IndexExpr*>(expr))
			return true;
		if (const auto* member = dynamic_cast<const ast::MemberExpr*>(expr))
			// `E1->E2` is `(*E1).E2` - dereferencing any pointer value yields an lvalue regardless
			// of whether E1 itself was one. `E1.E2` has no such dereference, so it's only an
			// lvalue when E1 is (real C's rule - see the header note on why `f().x = 1` must not
			// type-check when f() returns a struct by value).
			return member->isArrow() || isLValue(member->object());
		return false;
	}

	bool Sema::isAssignable(const Type* target, const Type* source) noexcept
	{
		if (!target || !source)
			return true; // one side already errored - do not cascade a second diagnostic
		if (*target == *source)
			return true;
		if (isArithmeticType(target) && isArithmeticType(source))
			return true;
		if (target->isPointer() && source->isPointer())
			return true;
		if (target->isPointer() && isArithmeticType(source))
			return true; // permissive: this subset does not track "null pointer constant" specially
		return false;
	}

	bool Sema::isCharType(const Type* type) noexcept
	{
		return type && (type->isChar() || type->isUChar() || type->isSChar());
	}

	void Sema::checkInitializer(const Type* type, Expr* init)
	{
		if (!init)
			return;

		if (auto* list = dynamic_cast<ast::InitListExpr*>(init))
		{
			checkInitList(type, *list);
			return;
		}

		// `char s[8] = "hola"` - the one non-brace initializer an ARRAY accepts. Note this is the
		// array itself being filled with the literal's bytes, not a pointer being aimed at the
		// literal's own .rodata copy (which is what the same StringLiteralExpr means anywhere else,
		// see visit(StringLiteralExpr&)), so it deliberately does not go through decayArray().
		if (type && type->isArray() && dynamic_cast<ast::StringLiteralExpr*>(init))
		{
			checkExpr(init); // still annotate the literal - libs/ir reads its PooledString, not its type
			if (!isCharType(type->arrayElementType()))
			{
				_diagnostics.error(init->location(), "initializing '{}' with a string literal requires an array of 'char'", typeName(type));
				return;
			}
			auto* literal = static_cast<ast::StringLiteralExpr*>(init);
			usize needed = literal->value().view().size() + 1; // + the terminating zero
			if (needed > type->arraySize())
			{
				_diagnostics.error(init->location(), "string literal needs {} byte(s) including its terminating zero, but '{}' holds {}",
					needed, typeName(type), type->arraySize());
			}
			return;
		}

		const Type* initType = decayArray(checkExpr(init));

		if (type && type->isArray())
		{
			// An array is never assignable from a plain expression in C - it has no assignment at
			// all - so this cannot fall through to isAssignable() and say "incompatible type", which
			// would suggest the right-hand side is the problem rather than the form.
			_diagnostics.error(init->location(), "an array like '{}' must be initialized with an initializer list or a string literal", typeName(type));
			return;
		}

		if (!isAssignable(type, initType))
		{
			_diagnostics.error(init->location(), "initializing '{}' with an expression of incompatible type '{}'",
				typeName(type), typeName(initType));
		}
	}

	void Sema::checkInitList(const Type* type, ast::InitListExpr& list)
	{
		std::span<Expr* const> elements = list.elements();
		list.setType(type); // what this list was checked against - see expr.h

		if (type && type->isArray())
		{
			const Type* elementType = type->arrayElementType();
			if (elements.size() == 1 && elementType && isCharType(elementType) &&
				dynamic_cast<ast::StringLiteralExpr*>(elements.front()))
			{
				// Braces around a character-array string initializer are transparent.
				checkInitializer(type, elements.front());
				return;
			}
			if (elements.size() > type->arraySize())
			{
				_diagnostics.error(list.location(), "{} value(s) in an initializer list for '{}', which holds {}",
					elements.size(), typeName(type), type->arraySize());
			}
			for (Expr* element : elements)
			{
				if (elementType && (elementType->isArray() || elementType->isStruct()) &&
					!dynamic_cast<ast::InitListExpr*>(element) && !dynamic_cast<ast::StringLiteralExpr*>(element))
				{
					const Type* elementExprType = decayArray(checkExpr(element));
					if (isAssignable(elementType, elementExprType))
						continue; // an aggregate value, not brace elision
					// Brace elision - see sema.h's own note on why this is reported, not guessed at.
					_diagnostics.error(element->location(), "expected '{{' to initialize a '{}' here - omitting the inner braces is not accepted in this version",
						typeName(elementType));
					continue;
				}
				checkInitializer(elementType, element);
			}
			return;
		}

		if (type && type->isStruct())
		{
			ast::StructDecl* decl = type->structDecl();
			if (!decl || !decl->isComplete())
			{
				_diagnostics.error(list.location(), "cannot initialize an incomplete type '{}'", typeName(type));
				for (Expr* element : elements)
				{
					if (auto* nested = dynamic_cast<ast::InitListExpr*>(element))
					{
						for (Expr* inner : nested->elements())
							checkExpr(inner);
						nested->setType(errorRecoveryType());
					}
					else
						checkExpr(element);
				}
				return;
			}

			std::span<const FieldDecl> fields = decl->fields();
			if (elements.size() > fields.size())
			{
				_diagnostics.error(list.location(), "{} value(s) in an initializer list for '{}', which has {} field(s)",
					elements.size(), typeName(type), fields.size());
			}
			for (usize i = 0; i < elements.size(); ++i)
			{
				const Type* fieldType = i < fields.size() ? fields[i].type : nullptr;
				if (!fieldType)
				{
					if (auto* nested = dynamic_cast<ast::InitListExpr*>(elements[i]))
					{
						for (Expr* inner : nested->elements())
							checkExpr(inner);
						nested->setType(errorRecoveryType());
					}
					else
						checkExpr(elements[i]);
					continue;
				}
				if ((fieldType->isArray() || fieldType->isStruct()) &&
					!dynamic_cast<ast::InitListExpr*>(elements[i]) && !dynamic_cast<ast::StringLiteralExpr*>(elements[i]))
				{
					const Type* elementExprType = decayArray(checkExpr(elements[i]));
					if (isAssignable(fieldType, elementExprType))
						continue; // an aggregate value, not brace elision
					_diagnostics.error(elements[i]->location(), "expected '{{' to initialize field '{}' of type '{}' here - omitting the inner braces is not accepted in this version",
						fields[i].name, typeName(fieldType));
					continue;
				}
				checkInitializer(fieldType, elements[i]);
			}
			return;
		}

		// A scalar/pointer with braces - real C's `int x = { 5 }`. One value, no more.
		if (elements.size() != 1)
		{
			_diagnostics.error(list.location(), "an initializer list for the scalar type '{}' takes exactly one value, not {}",
				typeName(type), elements.size());
		}
		for (Expr* element : elements)
			checkInitializer(type, element);
	}

	const Type* Sema::decayArray(const Type* type) noexcept
	{
		if (!type || !type->isArray())
			return type;
		return Type::makePointer(_arena, type->arrayElementType());
	}

	const Type* Sema::integerPromote(const Type* type) noexcept
	{
		if (!type)
			return &Type::Int;
		switch (type->kind())
		{
			case TypeKind::Bool: case TypeKind::Char: case TypeKind::UChar: case TypeKind::SChar:
			case TypeKind::Short: case TypeKind::UShort: case TypeKind::Enum:
				return &Type::Int;
			default:
				return type;
		}
	}

	namespace
	{
		int arithmeticRank(const Type* type) noexcept
		{
			if (!type)
				return 0;
			switch (type->kind())
			{
				case TypeKind::Float: return 100;
				case TypeKind::ULong: return 50;
				case TypeKind::Long: return 49;
				case TypeKind::UInt: return 40;
				case TypeKind::Int: return 39;
				default: return 10; // never reached once integerPromote() has already widened to at least int
			}
		}
	}

	const Type* Sema::commonArithmeticType(const Type* lhs, const Type* rhs) noexcept
	{
		const Type* promotedLhs = integerPromote(lhs);
		const Type* promotedRhs = integerPromote(rhs);
		return arithmeticRank(promotedLhs) >= arithmeticRank(promotedRhs) ? promotedLhs : promotedRhs;
	}

	// ---- constant-expression evaluator ------------------------------------------------------------

	std::optional<i64> Sema::evalConstantExpr(Expr* expr)
	{
		if (!expr)
			return std::nullopt;

		if (const auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(expr))
			return static_cast<i64>(lit->value());
		if (const auto* lit = dynamic_cast<const ast::CharLiteralExpr*>(expr))
			return static_cast<i64>(lit->value());
		if (const auto* lit = dynamic_cast<const ast::BoolLiteralExpr*>(expr))
			return lit->value() ? i64(1) : i64(0);

		if (const auto* name = dynamic_cast<const ast::NameExpr*>(expr))
		{
			// No scope to resolve against if called outside a check() traversal (e.g. directly,
			// as a standalone constant-expression evaluator) - nothing to look up, not an error.
			if (_scopes.empty())
				return std::nullopt;

			Symbol* symbol = currentScope().lookup(name->name());
			if (symbol && symbol->kind == SymbolKind::EnumConstant)
				return symbol->enumConstantValue;
			return std::nullopt;
		}

		if (auto* unary = dynamic_cast<ast::UnaryExpr*>(expr))
		{
			std::optional<i64> operand = evalConstantExpr(unary->operand());
			if (!operand)
				return std::nullopt;
			switch (unary->op())
			{
				case UnaryOp::Negate: return -*operand;
				case UnaryOp::LogicalNot: return *operand == 0 ? i64(1) : i64(0);
				case UnaryOp::BitwiseNot: return ~*operand;
				default: return std::nullopt; // &/*/++/-- are never constant expressions
			}
		}

		if (auto* binary = dynamic_cast<ast::BinaryExpr*>(expr))
		{
			std::optional<i64> lhs = evalConstantExpr(binary->lhs());
			std::optional<i64> rhs = evalConstantExpr(binary->rhs());
			if (!lhs || !rhs)
				return std::nullopt;
			switch (binary->op())
			{
				case BinaryOp::Add: return *lhs + *rhs;
				case BinaryOp::Sub: return *lhs - *rhs;
				case BinaryOp::Mul: return *lhs * *rhs;
				case BinaryOp::Div: return *rhs != 0 ? std::optional<i64>(*lhs / *rhs) : std::nullopt;
				case BinaryOp::Mod: return *rhs != 0 ? std::optional<i64>(*lhs % *rhs) : std::nullopt;
				case BinaryOp::BitAnd: return *lhs & *rhs;
				case BinaryOp::BitOr: return *lhs | *rhs;
				case BinaryOp::BitXor: return *lhs ^ *rhs;
				// A shift count outside [0, 63] is undefined behavior for a 64-bit operand in C++
				// itself, not just a real-C-semantics nuance - guard it explicitly rather than
				// letting a case label like `1 << 64` fold to a garbage constant.
				case BinaryOp::Shl:
					return (*rhs >= 0 && *rhs < 64) ? std::optional<i64>(static_cast<i64>(static_cast<u64>(*lhs) << *rhs)) : std::nullopt;
				case BinaryOp::Shr:
					return (*rhs >= 0 && *rhs < 64) ? std::optional<i64>(*lhs >> *rhs) : std::nullopt;
				case BinaryOp::Eq: return *lhs == *rhs ? i64(1) : i64(0);
				case BinaryOp::Ne: return *lhs != *rhs ? i64(1) : i64(0);
				case BinaryOp::Lt: return *lhs < *rhs ? i64(1) : i64(0);
				case BinaryOp::Le: return *lhs <= *rhs ? i64(1) : i64(0);
				case BinaryOp::Gt: return *lhs > *rhs ? i64(1) : i64(0);
				case BinaryOp::Ge: return *lhs >= *rhs ? i64(1) : i64(0);
				case BinaryOp::LogicalAnd: return (*lhs != 0 && *rhs != 0) ? i64(1) : i64(0);
				case BinaryOp::LogicalOr: return (*lhs != 0 || *rhs != 0) ? i64(1) : i64(0);
			}
			return std::nullopt;
		}

		if (auto* ternary = dynamic_cast<ast::TernaryExpr*>(expr))
		{
			std::optional<i64> cond = evalConstantExpr(ternary->cond());
			if (!cond)
				return std::nullopt;
			return *cond != 0 ? evalConstantExpr(ternary->thenExpr()) : evalConstantExpr(ternary->elseExpr());
		}

		if (auto* cast = dynamic_cast<ast::CastExpr*>(expr))
			return evalConstantExpr(cast->operand()); // no truncation modeled - see the header comment

		if (auto* sizeofExpr = dynamic_cast<ast::SizeofExpr*>(expr))
		{
			const Type* argType = sizeofExpr->isTypeArgument() ? sizeofExpr->argumentType()
				: (sizeofExpr->argumentExpr() ? sizeofExpr->argumentExpr()->type() : nullptr);
			if (!argType)
				return std::nullopt;
			return static_cast<i64>(argType->sizeInBytes());
		}

		return std::nullopt;
	}

	// ---- label collection (goto/label validity) ---------------------------------------------------

	void Sema::collectLabels(Stmt* stmt, std::vector<std::string_view>& out)
	{
		if (!stmt)
			return;

		if (auto* label = dynamic_cast<ast::LabelStmt*>(stmt))
		{
			if (std::find(out.begin(), out.end(), label->label()) != out.end())
				_diagnostics.error(label->location(), "redefinition of label '{}'", label->label());
			else
				out.push_back(label->label());
			collectLabels(label->body(), out);
			return;
		}
		if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt))
		{
			for (Stmt* child : compound->stmts())
				collectLabels(child, out);
			return;
		}
		if (auto* ifStmt = dynamic_cast<ast::IfStmt*>(stmt))
		{
			collectLabels(ifStmt->thenStmt(), out);
			collectLabels(ifStmt->elseStmt(), out);
			return;
		}
		if (auto* whileStmt = dynamic_cast<ast::WhileStmt*>(stmt)) { collectLabels(whileStmt->body(), out); return; }
		if (auto* doWhileStmt = dynamic_cast<ast::DoWhileStmt*>(stmt)) { collectLabels(doWhileStmt->body(), out); return; }
		if (auto* forStmt = dynamic_cast<ast::ForStmt*>(stmt)) { collectLabels(forStmt->body(), out); return; }
		if (auto* switchStmt = dynamic_cast<ast::SwitchStmt*>(stmt)) { collectLabels(switchStmt->body(), out); return; }
		if (auto* caseStmt = dynamic_cast<ast::CaseStmt*>(stmt)) { collectLabels(caseStmt->body(), out); return; }
		if (auto* defaultStmt = dynamic_cast<ast::DefaultStmt*>(stmt)) { collectLabels(defaultStmt->body(), out); return; }
		// ExprStmt/DeclStmt/ReturnStmt/BreakStmt/ContinueStmt/GotoStmt/EmptyStmt have no nested statement.
	}

	// ---- expressions --------------------------------------------------------------------------------

	void Sema::visit(ast::IntLiteralExpr& node)
	{
		node.setType(&Type::Int);
		_lastExprType = &Type::Int;
	}

	void Sema::visit(ast::FloatLiteralExpr& node)
	{
		// This subset has no `double` (see type.h) - an unsuffixed float literal is type `float`
		// by definition here, not the `double` real C would give it.
		node.setType(&Type::Float);
		_lastExprType = &Type::Float;
	}

	void Sema::visit(ast::CharLiteralExpr& node)
	{
		node.setType(&Type::Char);
		_lastExprType = &Type::Char;
	}

	void Sema::visit(ast::BoolLiteralExpr& node)
	{
		node.setType(&Type::Bool);
		_lastExprType = &Type::Bool;
	}

	void Sema::visit(ast::StringLiteralExpr& node)
	{
		const Type* resultType = Type::makePointer(_arena, &Type::Char);
		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::NameExpr& node)
	{
		const Type* resultType = errorRecoveryType();
		Symbol* symbol = currentScope().lookup(node.name());
		if (!symbol)
		{
			_diagnostics.error(node.location(), "use of undeclared identifier '{}'", node.name());
		}
		else if (symbol->kind == SymbolKind::Function)
		{
			// This subset has no function-pointer type (see type.h's TypeKind), so a bare function
			// name outside call position has nothing correct to be typed as - CallExpr resolves
			// its callee itself and never routes through here (see visit(CallExpr&)), so rejecting
			// this doesn't affect an ordinary `f(...)` call.
			_diagnostics.error(node.location(), "using function '{}' as a value is not supported in this version", node.name());
		}
		else
		{
			resultType = symbol->type ? symbol->type : errorRecoveryType();
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::CallExpr& node)
	{
		const Type* resultType = errorRecoveryType();
		ast::FunctionDecl* funcDecl = nullptr;

		if (auto* nameExpr = dynamic_cast<ast::NameExpr*>(node.callee()))
		{
			Symbol* symbol = currentScope().lookup(nameExpr->name());
			if (!symbol)
				_diagnostics.error(node.location(), "use of undeclared identifier '{}'", nameExpr->name());
			else if (symbol->kind != SymbolKind::Function)
				_diagnostics.error(node.location(), "called object '{}' is not a function", nameExpr->name());
			else
				funcDecl = symbol->funcDecl;

			nameExpr->setType(funcDecl ? funcDecl->returnType() : errorRecoveryType());
		}
		else
		{
			checkExpr(node.callee());
			_diagnostics.error(node.location(), "expression is not callable");
		}

		std::span<Expr* const> args = node.args();
		if (funcDecl)
		{
			std::span<const Param> params = funcDecl->params();
			if (args.size() != params.size())
			{
				_diagnostics.error(node.location(), "'{}' expects {} argument(s), got {}",
					funcDecl->name(), params.size(), args.size());
			}

			usize checkCount = std::min(args.size(), params.size());
			for (usize i = 0; i < checkCount; ++i)
			{
				const Type* argType = decayArray(checkExpr(args[i]));
				if (!isAssignable(params[i].type, argType))
				{
					_diagnostics.error(args[i]->location(), "passing '{}' to parameter of incompatible type '{}'",
						typeName(argType), typeName(params[i].type));
				}
				else if (isArithmeticType(argType) && isArithmeticType(params[i].type) &&
					(argType->isFloat() != params[i].type->isFloat()))
				{
					_diagnostics.error(args[i]->location(), "implicit conversion between integer and float call arguments is not supported");
				}
			}
			for (usize i = checkCount; i < args.size(); ++i)
				checkExpr(args[i]);

			resultType = funcDecl->returnType();
		}
		else
		{
			for (Expr* arg : args)
				checkExpr(arg);
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::UnaryExpr& node)
	{
		const Type* operandType = checkExpr(node.operand());
		const Type* decayedOperandType = decayArray(operandType);
		const Type* resultType = errorRecoveryType();

		switch (node.op())
		{
			case UnaryOp::AddressOf:
				if (!isLValue(node.operand()))
					_diagnostics.error(node.location(), "cannot take the address of a non-lvalue expression");
				resultType = Type::makePointer(_arena, operandType ? operandType : errorRecoveryType());
				break;

			case UnaryOp::Deref:
				if (operandType && (operandType->isPointer() || operandType->isArray()))
					resultType = operandType->arrayElementType();
				else
					_diagnostics.error(node.location(), "indirection requires a pointer operand ('{}' invalid)", typeName(operandType));
				break;

			case UnaryOp::Negate:
				if (isArithmeticType(operandType))
					resultType = integerPromote(operandType);
				else
					_diagnostics.error(node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				break;

			case UnaryOp::LogicalNot:
				if (!isScalarType(decayedOperandType))
					_diagnostics.error(node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				resultType = &Type::Bool;
				break;

			case UnaryOp::BitwiseNot:
				if (isIntegerType(operandType))
					resultType = integerPromote(operandType);
				else
					_diagnostics.error(node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				break;

			case UnaryOp::PreIncrement:
			case UnaryOp::PreDecrement:
			case UnaryOp::PostIncrement:
			case UnaryOp::PostDecrement:
				if (!isLValue(node.operand()))
					_diagnostics.error(node.location(), "expression is not assignable");
				if (!isScalarType(operandType))
					_diagnostics.error(node.location(), "cannot increment/decrement a value of type '{}'", typeName(operandType));
				resultType = operandType ? operandType : errorRecoveryType();
				break;
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::BinaryExpr& node)
	{
		// Decayed once, here, for every operator below: `arr + 1`, `arr == other`, and so on all see
		// arr as a Pointer, matching real C - see decayArray()'s header comment. The node itself
		// keeps annotating node.lhs()/node.rhs() with their own real (undecayed) types; only this
		// local copy used for operator validity/result-type computation is decayed.
		const Type* lhsType = decayArray(checkExpr(node.lhs()));
		const Type* rhsType = decayArray(checkExpr(node.rhs()));
		const Type* resultType = errorRecoveryType();

		// Every branch below only reassigns `resultType` to something derived from lhsType/rhsType
		// once it has confirmed those operands are actually valid for the operator - on failure
		// `resultType` stays at errorRecoveryType() instead of leaking a non-arithmetic operand
		// type (e.g. a struct or an unrelated pointer) onto this node's own annotated type.

		switch (node.op())
		{
			case BinaryOp::LogicalOr:
			case BinaryOp::LogicalAnd:
				if (!isScalarType(lhsType) || !isScalarType(rhsType))
					_diagnostics.error(node.location(), "invalid operands to logical expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				resultType = &Type::Bool;
				break;

			case BinaryOp::Eq: case BinaryOp::Ne:
			case BinaryOp::Lt: case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
			{
				// A pointer compared against an integer constant expression whose value is 0 is
				// the idiomatic null check (`p == 0`, `p != 0` - this subset has no NULL/nullptr
				// literal) and must type-check like real C does, even though a pointer compared
				// against an arbitrary integer still should not.
				bool nullConstant =
					(lhsType && lhsType->isPointer() && isIntegerType(rhsType) && evalConstantExpr(node.rhs()) == std::optional<i64>(0)) ||
					(rhsType && rhsType->isPointer() && isIntegerType(lhsType) && evalConstantExpr(node.lhs()) == std::optional<i64>(0));
				if (!((isArithmeticType(lhsType) && isArithmeticType(rhsType)) ||
					  (lhsType && rhsType && lhsType->isPointer() && rhsType->isPointer()) ||
					  nullConstant))
				{
					_diagnostics.error(node.location(), "comparison of incompatible operand types ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				}
				resultType = &Type::Bool;
				break;
			}

			case BinaryOp::BitOr: case BinaryOp::BitXor: case BinaryOp::BitAnd:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Shl: case BinaryOp::Shr:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = integerPromote(lhsType); // the rhs never affects the shift's result type
				else
					_diagnostics.error(node.location(), "invalid operands to shift expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Add:
			case BinaryOp::Sub:
				if (lhsType && lhsType->isPointer() && isIntegerType(rhsType))
					resultType = lhsType;
				else if (node.op() == BinaryOp::Add && rhsType && rhsType->isPointer() && isIntegerType(lhsType))
					resultType = rhsType;
				else if (node.op() == BinaryOp::Sub && lhsType && rhsType && lhsType->isPointer() && rhsType->isPointer())
					resultType = &Type::Int;
				else if (isArithmeticType(lhsType) && isArithmeticType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Mul: case BinaryOp::Div:
				if (isArithmeticType(lhsType) && isArithmeticType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Mod:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::AssignExpr& node)
	{
		const Type* targetType = checkExpr(node.target());
		const Type* valueType = decayArray(checkExpr(node.value()));

		if (!isLValue(node.target()))
			_diagnostics.error(node.location(), "expression is not assignable");
		else if (targetType && targetType->isStruct() && node.op() != ast::AssignOp::Assign)
			_diagnostics.error(node.location(), "compound assignment is not valid for struct type '{}'", typeName(targetType));
		else if (!isAssignable(targetType, valueType))
			_diagnostics.error(node.location(), "assigning to '{}' from incompatible type '{}'", typeName(targetType), typeName(valueType));

		const Type* resultType = targetType ? targetType : errorRecoveryType();
		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::IndexExpr& node)
	{
		const Type* arrayType = checkExpr(node.array());
		const Type* indexType = checkExpr(node.index());
		const Type* resultType = errorRecoveryType();

		if (arrayType && (arrayType->isPointer() || arrayType->isArray()))
			resultType = arrayType->arrayElementType();
		else
			_diagnostics.error(node.location(), "subscripted value ('{}') is not a pointer or array", typeName(arrayType));

		if (!isIntegerType(indexType))
			_diagnostics.error(node.location(), "array subscript ('{}') is not an integer", typeName(indexType));

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::MemberExpr& node)
	{
		const Type* objectType = checkExpr(node.object());
		const Type* resultType = errorRecoveryType();

		const Type* structType = nullptr;
		if (node.isArrow())
		{
			if (objectType && objectType->isPointer() && objectType->arrayElementType() && objectType->arrayElementType()->isStruct())
				structType = objectType->arrayElementType();
			else
				_diagnostics.error(node.location(), "member reference type '{}' is not a pointer to struct", typeName(objectType));
		}
		else
		{
			if (objectType && objectType->isStruct())
				structType = objectType;
			else
				_diagnostics.error(node.location(), "member reference base type '{}' is not a struct", typeName(objectType));
		}

		if (structType)
		{
			ast::StructDecl* decl = structType->structDecl();
			bool found = false;
			if (decl && decl->isComplete())
			{
				for (const FieldDecl& field : decl->fields())
				{
					if (field.name == node.memberName())
					{
						resultType = field.type;
						found = true;
						break;
					}
				}
			}
			if (!found)
			{
				_diagnostics.error(node.location(), "no member named '{}' in '{}'", node.memberName(), typeName(structType));
			}
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::CastExpr& node)
	{
		const Type* operandType = checkExpr(node.operand());
		const Type* targetType = node.type(); // set by the parser at construction time - see expr.h

		bool operandIsStruct = operandType && operandType->isStruct();
		bool targetIsStruct = targetType && targetType->isStruct();
		if (operandIsStruct != targetIsStruct || (operandIsStruct && targetIsStruct && !(*operandType == *targetType)))
			_diagnostics.error(node.location(), "cannot cast from '{}' to '{}'", typeName(operandType), typeName(targetType));

		_lastExprType = targetType;
	}

	void Sema::visit(ast::SizeofExpr& node)
	{
		if (!node.isTypeArgument())
			checkExpr(node.argumentExpr()); // non-evaluated context, but still checked for errors, same as real C

		node.setType(&Type::UInt);
		_lastExprType = &Type::UInt;
	}

	void Sema::visit(ast::TernaryExpr& node)
	{
		const Type* condType = decayArray(checkExpr(node.cond()));
		const Type* thenType = decayArray(checkExpr(node.thenExpr()));
		const Type* elseType = decayArray(checkExpr(node.elseExpr()));

		if (!isScalarType(condType))
			_diagnostics.error(node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

		const Type* resultType = errorRecoveryType();
		if (thenType && elseType && *thenType == *elseType)
			resultType = thenType;
		else if (isArithmeticType(thenType) && isArithmeticType(elseType))
			resultType = commonArithmeticType(thenType, elseType);
		else
			_diagnostics.error(node.location(), "incompatible operand types ('{}' and '{}') in ternary expression", typeName(thenType), typeName(elseType));

		node.setType(resultType);
		_lastExprType = resultType;
	}

	// ---- statements -----------------------------------------------------------------------------

	void Sema::visit(ast::InitListExpr& node)
	{
		// Only reachable if an InitListExpr ever shows up somewhere checkInitializer() is not what
		// looks at it. libs/parser builds one only from parseInitializer() (expr.h/parser.h), so on
		// well-formed input this never runs - it exists so a future caller that forgets to route an
		// initializer through checkInitializer() gets a real diagnostic instead of an unchecked
		// subtree with null types reaching libs/ir.
		_diagnostics.error(node.location(), "an initializer list is only valid as a variable's initializer");
		for (Expr* element : node.elements())
			checkExpr(element);
		node.setType(errorRecoveryType());
		_lastExprType = node.type();
	}

	void Sema::visit(ast::EmptyStmt&) {}

	void Sema::visit(ast::ExprStmt& node)
	{
		checkExpr(node.expr());
	}

	void Sema::visit(ast::DeclStmt& node)
	{
		if (node.decl())
			node.decl()->accept(*this);
	}

	void Sema::visit(ast::CompoundStmt& node)
	{
		pushScope();
		for (Stmt* stmt : node.stmts())
			checkStmt(stmt);
		popScope();
	}

	void Sema::visit(ast::IfStmt& node)
	{
		const Type* condType = decayArray(checkExpr(node.cond()));
		if (!isScalarType(condType))
			_diagnostics.error(node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

		checkStmt(node.thenStmt());
		checkStmt(node.elseStmt());
	}

	void Sema::visit(ast::WhileStmt& node)
	{
		const Type* condType = decayArray(checkExpr(node.cond()));
		if (!isScalarType(condType))
			_diagnostics.error(node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

		++_loopDepth;
		checkStmt(node.body());
		--_loopDepth;
	}

	void Sema::visit(ast::DoWhileStmt& node)
	{
		++_loopDepth;
		checkStmt(node.body());
		--_loopDepth;

		const Type* condType = decayArray(checkExpr(node.cond()));
		if (!isScalarType(condType))
			_diagnostics.error(node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));
	}

	void Sema::visit(ast::ForStmt& node)
	{
		pushScope(); // `for (int i = 0; ...)` scopes `i` to the loop, not the enclosing block
		checkStmt(node.init());

		if (node.cond())
		{
			const Type* condType = decayArray(checkExpr(node.cond()));
			if (!isScalarType(condType))
				_diagnostics.error(node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));
		}
		checkExpr(node.increment());

		++_loopDepth;
		checkStmt(node.body());
		--_loopDepth;
		popScope();
	}

	void Sema::visit(ast::ReturnStmt& node)
	{
		const Type* returnType = _currentFunctionReturnType ? _currentFunctionReturnType : &Type::Void;

		if (node.value())
		{
			const Type* valueType = decayArray(checkExpr(node.value()));
			if (returnType->isVoid())
				_diagnostics.error(node.location(), "void function should not return a value");
			else if (!isAssignable(returnType, valueType))
				_diagnostics.error(node.location(), "returning '{}' from a function with incompatible result type '{}'", typeName(valueType), typeName(returnType));
		}
		else if (!returnType->isVoid())
		{
			_diagnostics.error(node.location(), "non-void function should return a value");
		}
	}

	void Sema::visit(ast::BreakStmt& node)
	{
		if (_loopDepth == 0 && _switchStack.empty())
			_diagnostics.error(node.location(), "'break' statement not in a loop or switch statement");
	}

	void Sema::visit(ast::ContinueStmt& node)
	{
		if (_loopDepth == 0)
			_diagnostics.error(node.location(), "'continue' statement not in a loop");
	}

	void Sema::visit(ast::SwitchStmt& node)
	{
		const Type* condType = checkExpr(node.cond());
		if (!isIntegerType(condType))
			_diagnostics.error(node.location(), "switch condition type '{}' is not an integer", typeName(condType));

		_switchStack.emplace_back();
		checkStmt(node.body());
		_switchStack.pop_back();
	}

	void Sema::visit(ast::CaseStmt& node)
	{
		if (_switchStack.empty())
			_diagnostics.error(node.location(), "'case' statement not in a switch statement");

		checkExpr(node.value());
		std::optional<i64> value = evalConstantExpr(node.value());
		if (!value)
		{
			_diagnostics.error(node.location(), "case label does not reduce to an integer constant");
		}
		else if (!_switchStack.empty())
		{
			SwitchContext& context = _switchStack.back();
			if (std::find(context.seenCaseValues.begin(), context.seenCaseValues.end(), *value) != context.seenCaseValues.end())
				_diagnostics.error(node.location(), "duplicate case value '{}'", *value);
			else
				context.seenCaseValues.push_back(*value);
		}

		checkStmt(node.body());
	}

	void Sema::visit(ast::DefaultStmt& node)
	{
		if (_switchStack.empty())
		{
			_diagnostics.error(node.location(), "'default' statement not in a switch statement");
		}
		else
		{
			SwitchContext& context = _switchStack.back();
			if (context.defaultSeen)
				_diagnostics.error(node.location(), "multiple default labels in one switch statement");
			else
				context.defaultSeen = true;
		}

		checkStmt(node.body());
	}

	void Sema::visit(ast::GotoStmt& node)
	{
		if (std::find(_currentFunctionLabels.begin(), _currentFunctionLabels.end(), node.label()) == _currentFunctionLabels.end())
			_diagnostics.error(node.location(), "use of undeclared label '{}'", node.label());
	}

	void Sema::visit(ast::LabelStmt& node)
	{
		checkStmt(node.body());
	}

	// ---- declarations -----------------------------------------------------------------------------

	void Sema::visit(ast::VarDecl& node)
	{
		if (node.type() && node.type()->isVoid())
			_diagnostics.error(node.location(), "variable '{}' declared with type 'void'", node.name());

		// Declared before its initializer is checked: in C, a declarator's own scope begins right
		// after the declarator, before the initializer - so `int x = x;` refers to the new
		// (as yet uninitialized) `x`, not any `x` from an enclosing scope. Checking the initializer
		// first would resolve a self-referencing initializer against the wrong `x` (or report it
		// as undeclared) instead of matching real C's scoping rule.
		Symbol symbol;
		symbol.kind = SymbolKind::Variable;
		symbol.name = node.name();
		symbol.type = node.type();
		symbol.location = node.location();
		symbol.isGlobal = (&currentScope() == _globalScope);
		symbol.varDecl = &node;
		declareSymbol(symbol);

		if (node.initializer())
			checkInitializer(node.type(), node.initializer());
	}

	void Sema::visit(ast::FunctionDecl& node)
	{
		Symbol* existing = _globalScope->lookupInThisScope(node.name());
		bool kindConflict = existing && existing->kind != SymbolKind::Function;
		if (kindConflict)
			_diagnostics.error(node.location(), "redefinition of '{}' as a different kind of symbol", node.name());

		if (existing && !kindConflict)
		{
			ast::FunctionDecl* previous = existing->funcDecl;
			bool signatureMatches = previous && previous->returnType() && node.returnType() &&
				*previous->returnType() == *node.returnType() &&
				previous->params().size() == node.params().size();
			if (signatureMatches)
			{
				for (usize i = 0; signatureMatches && i < previous->params().size(); ++i)
				{
					const Type* previousParamType = previous->params()[i].type;
					const Type* newParamType = node.params()[i].type;
					signatureMatches = previousParamType && newParamType && *previousParamType == *newParamType;
				}
			}
			if (!signatureMatches)
				_diagnostics.error(node.location(), "conflicting types for '{}'", node.name());
			else if (previous && previous->isDefinition() && node.isDefinition())
				_diagnostics.error(node.location(), "redefinition of function '{}'", node.name());

			if (node.isDefinition() || !previous || !previous->isDefinition())
				existing->funcDecl = &node;
		}
		else
		{
			Symbol symbol;
			symbol.kind = SymbolKind::Function;
			symbol.name = node.name();
			symbol.type = node.returnType();
			symbol.location = node.location();
			symbol.isGlobal = true;
			symbol.funcDecl = &node;

			// `existing` already names a differently-kinded symbol (kindConflict, error already
			// reported above): overwrite that slot in place rather than calling Scope::declare(),
			// which would silently fail (the name is already taken) and leave this function with
			// no symbol table entry at all - every later call to it would then report a second,
			// more confusing "is not a function" instead of just the one conflict diagnostic above.
			if (kindConflict)
				*existing = symbol;
			else
				_globalScope->declare(symbol);
		}

		if (!node.isDefinition())
			return;

		pushScope();
		for (const Param& param : node.params())
		{
			Symbol paramSymbol;
			paramSymbol.kind = SymbolKind::Parameter;
			paramSymbol.name = param.name;
			paramSymbol.type = param.type;
			paramSymbol.location = param.location;
			declareSymbol(paramSymbol);
		}

		const Type* previousReturnType = _currentFunctionReturnType;
		u32 previousLoopDepth = _loopDepth;
		std::vector<SwitchContext> previousSwitchStack = std::move(_switchStack);
		std::vector<std::string_view> previousLabels = std::move(_currentFunctionLabels);

		_currentFunctionReturnType = node.returnType();
		_loopDepth = 0;
		_switchStack.clear();
		_currentFunctionLabels.clear();
		collectLabels(node.body(), _currentFunctionLabels);

		// Not checkStmt(node.body()): that would dispatch to visit(CompoundStmt&), which pushes
		// its own nested scope, leaving the parameter list and the body's outermost block in two
		// different scopes - a body-level `int x;` would then merely shadow a parameter `x`
		// instead of correctly conflicting with it. In C the two share one scope, so this walks
		// the body's top-level statements directly in the scope already holding the parameters.
		for (Stmt* stmt : node.body()->stmts())
			checkStmt(stmt);

		_currentFunctionReturnType = previousReturnType;
		_loopDepth = previousLoopDepth;
		_switchStack = std::move(previousSwitchStack);
		_currentFunctionLabels = std::move(previousLabels);

		popScope();
	}

	void Sema::visit(ast::StructDecl& node)
	{
		validateStructLayout(_diagnostics, node);
	}

	void Sema::visit(ast::EnumDecl& node)
	{
		if (!node.isComplete())
			return;

		i64 nextValue = 0;
		for (const EnumeratorDecl& enumerator : node.enumerators())
		{
			if (enumerator.value)
			{
				checkExpr(enumerator.value);
				std::optional<i64> evaluated = evalConstantExpr(enumerator.value);
				if (!evaluated)
					_diagnostics.error(enumerator.location, "enumerator value for '{}' is not a constant expression", enumerator.name);
				else
					nextValue = *evaluated;
			}

			Symbol symbol;
			symbol.kind = SymbolKind::EnumConstant;
			symbol.name = enumerator.name;
			symbol.type = &Type::Int;
			symbol.location = enumerator.location;
			symbol.isGlobal = (&currentScope() == _globalScope);
			symbol.enumConstantValue = nextValue;
			declareSymbol(symbol);

			++nextValue;
		}
	}

	void Sema::visit(ast::TypedefDecl&)
	{
		// The parser already fully resolved the underlying type (decl.h) - nothing left to check.
	}

	void Sema::visit(ast::TranslationUnit& node)
	{
		for (Decl* decl : node.decls())
		{
			if (decl)
				decl->accept(*this);
		}
	}
}
