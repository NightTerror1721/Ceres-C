#include <ceresc/sema/sema.h>
#include <format>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/ast_printer.h>

#include <algorithm>
#include <span>

namespace ceresc::sema
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

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
		_interruptVectors.clear();

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
			_diagnostics.error(DiagId::Redefinition, symbol.location, "redefinition of '{}'", symbol.name);
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
		if (dynamic_cast<const ast::CompoundLiteralExpr*>(expr))
			return true;      // an unnamed object, with an address of its own
		if (const auto* member = dynamic_cast<const ast::MemberExpr*>(expr))
			// `E1->E2` is `(*E1).E2` - dereferencing any pointer value yields an lvalue regardless
			// of whether E1 itself was one. `E1.E2` has no such dereference, so it's only an
			// lvalue when E1 is (real C's rule - see the header note on why `f().x = 1` must not
			// type-check when f() returns a struct by value).
			return member->isArrow() || isLValue(member->object());
		return false;
	}

	namespace
	{
		// C's null pointer constant: the literal 0, or 0 cast to `void*` (NULL is `((void*)0)`).
		bool isNullPointerConstant(const Expr* expr)
		{
			if (const auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(expr))
				return literal->value() == 0;
			if (const auto* cast = dynamic_cast<const ast::CastExpr*>(expr))
			{
				const Type* to = cast->targetType();
				const Type* pointee = to && to->isPointer() ? to->arrayElementType() : nullptr;
				return pointee && pointee->isVoid() && isNullPointerConstant(cast->operand());
			}
			return false;
		}
	}

	void Sema::recordConstant(Expr* expr)
	{
		if (!expr || expr->constantValue())
			return;
		// A literal is already its own value. What is worth folding is what is built from them.
		if (dynamic_cast<ast::BinaryExpr*>(expr) || dynamic_cast<ast::TernaryExpr*>(expr) ||
			dynamic_cast<ast::SizeofExpr*>(expr) || dynamic_cast<ast::NameExpr*>(expr) || dynamic_cast<ast::UnaryExpr*>(expr))
		{
			if (std::optional<i64> value = evalConstantExpr(expr))
				expr->setConstantValue(*value);
		}
	}

	bool Sema::isAssignable(const Type* target, const Type* source, const Expr* sourceExpr) noexcept
	{
		if (target && target->isPointer() && isNullPointerConstant(sourceExpr))
			return true;
		return isAssignable(target, source);
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
		{
			// The one thing a pointer conversion may not do: forget a qualifier on what it points
			// at. `const int*` -> `int*` would hand out a writable alias to a read-only object,
			// which for a global living in @rodata is not a formality - the store faults.
			// `volatile int*` -> `int*` is the same shape of mistake with a different consequence:
			// the alias loses the guarantee, and the optimizer is then free over accesses the
			// program needed kept. The other direction is always fine for both: promising more
			// about an object than you have to is never wrong.
			const Type* targetPointee = target->arrayElementType();
			const Type* sourcePointee = source->arrayElementType();

			// A pointer to a function converts to a pointer to a function of the SAME signature and
			// to nothing else. The qualifier walk below would wave anything through, because a
			// function type is never const or volatile - and calling through a mismatched signature
			// is not a portability nicety here, it is the wrong arguments in the wrong registers.
			bool targetIsFunction = targetPointee && targetPointee->isFunction();
			bool sourceIsFunction = sourcePointee && sourcePointee->isFunction();
			if (targetIsFunction || sourceIsFunction)
				return targetIsFunction && sourceIsFunction && *targetPointee == *sourcePointee;

			while (sourcePointee && targetPointee)
			{
				if (sourcePointee->isConst() && !targetPointee->isConst())
					return false;
				if (sourcePointee->isVolatile() && !targetPointee->isVolatile())
					return false;
				sourcePointee = sourcePointee->arrayElementType();
				targetPointee = targetPointee->arrayElementType();
			}
			return true;
		}
		if (target->isPointer() && isArithmeticType(source))
			return true; // permissive: this subset does not track "null pointer constant" specially
		// A struct or union converts to one of the same tag whatever qualifiers either side carries:
		// `struct T copy = *constPtr;` makes a NEW object, it does not hand out an alias to the
		// read-only one (that is what the pointer rule above guards). Different tags stay incompatible.
		if (target->isAggregate() && target->kind() == source->kind() && target->structDecl() &&
			target->structDecl() == source->structDecl())
			return true;
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
				_diagnostics.error(DiagId::StringInitializerNeedsCharArray, init->location(), "initializing '{}' with a string literal requires an array of 'char'", typeName(type));
				return;
			}
			auto* literal = static_cast<ast::StringLiteralExpr*>(init);
			usize needed = literal->value().view().size() + 1; // + the terminating zero
			if (needed > type->arraySize())
			{
				_diagnostics.error(DiagId::StringInitializerTooLong, init->location(), "string literal needs {} byte(s) including its terminating zero, but '{}' holds {}",
					needed, typeName(type), type->arraySize());
			}
			return;
		}

		const Type* initType = decayArray(checkExpr(init));
		recordConstant(init);

		if (type && type->isArray())
		{
			// An array is never assignable from a plain expression in C - it has no assignment at
			// all - so this cannot fall through to isAssignable() and say "incompatible type", which
			// would suggest the right-hand side is the problem rather than the form.
			_diagnostics.error(DiagId::ArrayNeedsListInitializer, init->location(), "an array like '{}' must be initialized with an initializer list or a string literal", typeName(type));
			return;
		}

		if (!isAssignable(type, initType, init))
		{
			_diagnostics.error(DiagId::IncompatibleInitializer, init->location(), "initializing '{}' with an expression of incompatible type '{}'",
				typeName(type), typeName(initType));
		}
	}

	// ---- designated initializers ---------------------------------------------------------------------------------

	namespace
	{
		bool containsDesignator(const ast::InitListExpr& list)
		{
			for (const Expr* element : list.elements())
			{
				if (dynamic_cast<const ast::DesignatedInitExpr*>(element))
					return true;
				if (const auto* nested = dynamic_cast<const ast::InitListExpr*>(element); nested && containsDesignator(*nested))
					return true;
			}
			return false;
		}

		// The type of the element a position stands for, or null past the end (which sema reports anyway).
		const Type* elementTypeAt(const Type* type, usize index)
		{
			if (type->isArray())
				return type->arrayElementType();
			if (type->isAggregate() && type->structDecl() && index < type->structDecl()->fields().size())
				return type->structDecl()->fields()[index].type;
			return nullptr;
		}
	}

	ast::InitListExpr* Sema::makeInitList(support::SourceLocation location, const std::vector<ast::Expr*>& elements)
	{
		std::span<Expr* const> stored;
		if (!elements.empty())
		{
			Expr** memory = static_cast<Expr**>(_arena.allocate(sizeof(Expr*) * elements.size(), alignof(Expr*)));
			for (usize i = 0; i < elements.size(); ++i)
				memory[i] = elements[i];
			stored = std::span<Expr* const>(memory, elements.size());
		}
		return _arena.create<ast::InitListExpr>(location, stored);
	}

	// What stands for "all zero" where a list skipped an element: a scalar's own zero, and for an aggregate a list
	// that sets its first scalar to zero, which is what leaves the rest zero as well.
	ast::Expr* Sema::zeroInitializerFor(const Type* type, support::SourceLocation location)
	{
		if (type && (type->isArray() || type->isAggregate()))
		{
			const Type* first = nullptr;
			if (type->isArray())
				first = type->arrayElementType();
			else if (type->structDecl() && type->structDecl()->isComplete() && !type->structDecl()->fields().empty())
				first = type->structDecl()->fields().front().type;
			Expr* inner = first ? zeroInitializerFor(first, location) : static_cast<Expr*>(_arena.create<ast::IntLiteralExpr>(location, u64{ 0 }));
			return makeInitList(location, { inner });
		}
		if (type && type->isFloat())
			return _arena.create<ast::FloatLiteralExpr>(location, 0.0);
		return _arena.create<ast::IntLiteralExpr>(location, u64{ 0 });
	}

	// Which element a designator names in `type`: an index in an array, a field's position in a struct. Reports what
	// is wrong with it and returns nothing when it cannot be used.
	std::optional<usize> Sema::designatedIndex(const Type* type, const ast::Designator& designator)
	{
		if (type->isArray())
		{
			if (designator.isField)
			{
				_diagnostics.error(DiagId::InvalidDesignator, designator.location, "a member designator '.{}' cannot initialize the array '{}'", designator.field, typeName(type));
				return std::nullopt;
			}
			checkExpr(designator.index);
			std::optional<i64> index = evalConstantExpr(designator.index);
			if (!index)
			{
				_diagnostics.error(DiagId::DesignatorIndexNotConstant, designator.index->location(), "the index of an array designator must be an integer constant expression");
				return std::nullopt;
			}
			if (*index < 0 || (type->arraySize() != 0 && *index >= static_cast<i64>(type->arraySize())))
			{
				_diagnostics.error(DiagId::DesignatorIndexOutOfRange, designator.index->location(), "the designator index {} is outside '{}', which holds {}",
					*index, typeName(type), type->arraySize());
				return std::nullopt;
			}
			return static_cast<usize>(*index);
		}

		if (!designator.isField)
		{
			_diagnostics.error(DiagId::InvalidDesignator, designator.location, "an index designator cannot initialize '{}'", typeName(type));
			return std::nullopt;
		}
		ast::StructDecl* decl = type->structDecl();
		if (!decl || !decl->isComplete())
		{
			_diagnostics.error(DiagId::IncompleteTypeInitializer, designator.location, "cannot initialize an incomplete type '{}'", typeName(type));
			return std::nullopt;
		}
		std::span<const FieldDecl> fields = decl->fields();
		for (usize i = 0; i < fields.size(); ++i)
		{
			if (fields[i].name != designator.field)
				continue;
			if (type->isUnion() && i != 0)
			{
				_diagnostics.error(DiagId::UnionMemberDesignator, designator.location,
					"'.{}' is not the first member of '{}': only the first member of a union can be initialized in this version", designator.field, typeName(type));
				return std::nullopt;
			}
			return i;
		}
		_diagnostics.error(DiagId::NoSuchMember, designator.location, "no member named '{}' in '{}'", designator.field, typeName(type));
		return std::nullopt;
	}

	void Sema::normalizeInitList(const Type* type, ast::InitListExpr& list)
	{
		if (!type || !containsDesignator(list))
			return;

		std::vector<Expr*> slots;               // by position; null is a place nothing named
		usize position = 0;
		const bool isArrayOrAggregate = type->isArray() || type->isAggregate();

		for (Expr* element : list.elements())
		{
			auto* designated = dynamic_cast<ast::DesignatedInitExpr*>(element);
			if (!designated)
			{
				if (slots.size() <= position)
					slots.resize(position + 1, nullptr);
				slots[position++] = element;
				continue;
			}
			if (!isArrayOrAggregate)
			{
				_diagnostics.error(DiagId::InvalidDesignator, designated->location(), "a designated initializer needs an array, struct or union, not '{}'", typeName(type));
				if (slots.size() <= position)
					slots.resize(position + 1, nullptr);
				slots[position++] = designated->value();   // no more messages about it than this one
				continue;
			}

			std::span<const ast::Designator> designators = designated->designators();
			std::optional<usize> index = designatedIndex(type, designators.front());
			if (!index)
				continue;
			if (slots.size() <= *index)
				slots.resize(*index + 1, nullptr);

			if (designators.size() == 1)
			{
				slots[*index] = designated->value();
			}
			else
			{
				// `.a.b = v`: the rest of the path goes to the sub-object, as an element of a list of its own.
				// Several designations of one sub-object accumulate in that list, and it is normalized in turn.
				Expr* rest = _arena.create<ast::DesignatedInitExpr>(designated->location(), designators.subspan(1), designated->value());
				auto* sub = dynamic_cast<ast::InitListExpr*>(slots[*index]);
				if (!sub)
				{
					sub = makeInitList(designated->location(), {});
					slots[*index] = sub;
				}
				std::vector<Expr*> parts(sub->elements().begin(), sub->elements().end());
				parts.push_back(rest);
				sub->setElements(makeInitList(designated->location(), parts)->elements());
			}
			position = *index + 1;
		}

		for (usize i = 0; i < slots.size(); ++i)
		{
			if (!slots[i])
			{
				const Type* elementType = elementTypeAt(type, i);
				slots[i] = elementType ? zeroInitializerFor(elementType, list.location())
					: static_cast<Expr*>(_arena.create<ast::IntLiteralExpr>(list.location(), u64{ 0 }));
			}
			if (auto* nested = dynamic_cast<ast::InitListExpr*>(slots[i]))
			{
				if (const Type* elementType = elementTypeAt(type, i))
					normalizeInitList(elementType, *nested);
			}
		}
		list.setElements(makeInitList(list.location(), slots)->elements());
	}

	void Sema::visit(ast::AsmStmt&)
	{
		// The text is the assembler's to judge; nothing here can say more about it than that it is a string.
	}

	void Sema::visit(ast::CompoundLiteralExpr& node)
	{
		const Type* type = node.literalType();
		if (!type || type->isVoid() || type->isFunction())
		{
			_diagnostics.error(DiagId::InvalidCompoundLiteralType, node.location(), "a compound literal cannot have the type '{}'", typeName(type));
			node.setType(errorRecoveryType());
			_lastExprType = errorRecoveryType();
			return;
		}
		normalizeInitList(type, *node.list());
		checkInitList(type, *node.list());
		node.setType(type);
		_lastExprType = type;
	}

	void Sema::visit(ast::DesignatedInitExpr& node)
	{
		// Only a designation that is not inside a list being initialized gets here (the rest were rewritten away).
		_diagnostics.error(DiagId::InvalidDesignator, node.location(), "a designator can only appear in a brace initializer list");
		node.setType(errorRecoveryType());
		_lastExprType = errorRecoveryType();
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
				_diagnostics.error(DiagId::InitializerListSize, list.location(), "{} value(s) in an initializer list for '{}', which holds {}",
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
					_diagnostics.error(DiagId::MissingInnerBraces, element->location(), "expected '{{' to initialize a '{}' here - omitting the inner braces is not accepted in this version",
						typeName(elementType));
					continue;
				}
				checkInitializer(elementType, element);
			}
			return;
		}

		if (type && type->isAggregate())
		{
			ast::StructDecl* decl = type->structDecl();
			if (!decl || !decl->isComplete())
			{
				_diagnostics.error(DiagId::IncompleteTypeInitializer, list.location(), "cannot initialize an incomplete type '{}'", typeName(type));
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
				_diagnostics.error(DiagId::InitializerListSize, list.location(), "{} value(s) in an initializer list for '{}', which has {} field(s)",
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
					_diagnostics.error(DiagId::MissingInnerBraces, elements[i]->location(), "expected '{{' to initialize field '{}' of type '{}' here - omitting the inner braces is not accepted in this version",
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
			_diagnostics.error(DiagId::InitializerListSize, list.location(), "an initializer list for the scalar type '{}' takes exactly one value, not {}",
				typeName(type), elements.size());
		}
		for (Expr* element : elements)
			checkInitializer(type, element);
	}

	void Sema::checkVariadicArgument(ast::Expr* arg, bool calleeIsVariadic)
	{
		const Type* argType = decayArray(checkExpr(arg));
		if (!calleeIsVariadic || !argType)
			return; // a surplus argument to a non-variadic callee is already diagnosed as an arity error

		// A struct or union argument travels as a hidden pointer to a caller-owned copy
		// (ir_builder.h's struct convention). Nothing in that convention tells the callee how big
		// the copy is, and __builtin_va_arg() has no way to ask - so rather than pass one and let
		// the callee read whatever it guesses, this is refused outright.
		if (argType->isAggregate())
		{
			_diagnostics.error(DiagId::AggregateThroughEllipsis, arg->location(),
				"cannot pass '{}' through '...': struct and union arguments have no variadic representation",
				typeName(argType));
			return;
		}
		if (argType->isVoid())
			_diagnostics.error(DiagId::VoidThroughEllipsis, arg->location(), "cannot pass a void value through '...'");

		// The default argument promotions are not applied by rewriting the type here: a narrow
		// value is ALREADY in its promoted representation by the time it becomes a value at all
		// (ir_instr.h's IrUnOp::Narrow invariant), so the word the caller stores is exactly the
		// `int` a matching __builtin_va_arg(ap, int) reads back. `float` is the one place this
		// deviates from C on purpose: C promotes it to `double`, the machine has no f64 at all, so
		// a float travels as the f32 it already is - see docs/09-Variadic-Convention.md.
	}

	// The type a FunctionDecl declares: `int f(int)` has type `int(int)`. Built on demand rather
	// than stored on the node, because only the handful of places that use a function as a value
	// ever need it and the declaration already holds every piece.
	const Type* Sema::functionTypeOf(const ast::FunctionDecl* decl)
	{
		if (!decl)
			return errorRecoveryType();

		std::vector<const Type*> paramTypes;
		paramTypes.reserve(decl->params().size());
		for (const Param& param : decl->params())
			paramTypes.push_back(param.type);
		return Type::makeFunction(_arena, decl->returnType(), paramTypes, decl->isVariadic());
	}

	const Type* Sema::decayArray(const Type* type) noexcept
	{
		if (!type)
			return type;
		if (type->isArray())
			return Type::makePointer(_arena, type->arrayElementType());
		// A function decays to a pointer to itself in every position but `sizeof` and `&`, and for
		// the same reason an array does: a function type names no object, so there is nothing a
		// value of that type could be. `&f` and `f` therefore mean the same thing, as in C.
		if (type->isFunction())
			return Type::makePointer(_arena, type);
		return type;
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
				case BinaryOp::Comma: break; // never a constant expression, even when both sides are
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
			// A constant asked for before the enclosing declaration's initializer was checked: the operand has
			// no type yet, and its type is all sizeof needs.
			if (!sizeofExpr->isTypeArgument() && sizeofExpr->argumentExpr() && !sizeofExpr->argumentExpr()->type() && !_scopes.empty())
				checkExpr(sizeofExpr->argumentExpr());
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
				_diagnostics.error(DiagId::RedefinitionOfLabel, label->location(), "redefinition of label '{}'", label->label());
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
		// A `u`/`U` suffix selects the unsigned type; an unsuffixed literal is `int` (no widening to
		// `long` exists in this subset - see type.h).
		const Type* type = node.isUnsigned() ? &Type::UInt : &Type::Int;
		node.setType(type);
		_lastExprType = type;
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
			_diagnostics.error(DiagId::UndeclaredIdentifier, node.location(), "use of undeclared identifier '{}'", node.name());
		}
		else if (symbol->kind == SymbolKind::Function)
		{
			if (symbol->funcDecl && symbol->funcDecl->isInterruptHandler())
			{
				// Rejected here as well as in call position, because a pointer would be a way round
				// that check: the vector is the only entry an `iret` can return from.
				_diagnostics.error(DiagId::InterruptHandlerAddressTaken, node.location(),
					"'{}' is an '__interrupt' handler, so its address cannot be taken: it is reached through its vector",
					node.name());
			}
			// The function's own type, which decayArray() turns into a pointer wherever a value is
			// wanted. Left undecayed here so that `&f` and `sizeof f` - the two positions where C
			// does not decay - can still see what it really is.
			resultType = functionTypeOf(symbol->funcDecl);
		}
		else
		{
			resultType = symbol->type ? symbol->type : errorRecoveryType();
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	// Argument checking for one call, written against a SIGNATURE rather than against a
	// FunctionDecl. A call knows what it may pass from the callee's return type, parameter list and
	// `...` - never from which declaration those came from - so this works the same whether the
	// callee was named or computed. `calleeName` is only ever quoted in a diagnostic.
	void Sema::checkCallArguments(ast::CallExpr& node, const CallSignature& signature, std::string_view calleeName)
	{
		std::span<Expr* const> args = node.args();
		std::span<const Param> params = signature.params;

		// A `...` turns the declared arity into a MINIMUM: the fixed parameters must all be there
		// and are type-checked as usual, and anything past them is the variadic tail, which by
		// construction has no declared type to check against.
		if (signature.isVariadic)
		{
			if (args.size() < params.size())
				_diagnostics.error(DiagId::ArgumentCount, node.location(), "{} expects at least {} argument(s), got {}",
					calleeName, params.size(), args.size());
		}
		else if (args.size() != params.size())
		{
			_diagnostics.error(DiagId::ArgumentCount, node.location(), "{} expects {} argument(s), got {}",
				calleeName, params.size(), args.size());
		}

		usize checkCount = std::min(args.size(), params.size());
		for (usize i = 0; i < checkCount; ++i)
		{
			const Type* argType = decayArray(checkExpr(args[i]));
			if (!isAssignable(params[i].type, argType, args[i]))
			{
				_diagnostics.error(DiagId::IncompatibleArgument, args[i]->location(), "passing '{}' to parameter of incompatible type '{}'",
					typeName(argType), typeName(params[i].type));
			}
			else if (isArithmeticType(argType) && isArithmeticType(params[i].type) &&
				(argType->isFloat() != params[i].type->isFloat()))
			{
				_diagnostics.error(DiagId::IntegerFloatArgument, args[i]->location(), "implicit conversion between integer and float call arguments is not supported");
			}
		}
		for (usize i = checkCount; i < args.size(); ++i)
			checkVariadicArgument(args[i], signature.isVariadic);
	}

	void Sema::visit(ast::CallExpr& node)
	{
		const Type* resultType = errorRecoveryType();

		// Two kinds of callee, and only one of them has a declaration behind it. A name that
		// resolves to a function keeps its FunctionDecl, because the parameter NAMES in the
		// diagnostics come from there and nothing else has them; everything else is checked against
		// the signature its type carries, which is all a call ever needed (see checkCallArguments).
		ast::FunctionDecl* funcDecl = nullptr;
		std::string calleeName = "the called expression";
		const ast::FunctionTypeInfo* info = nullptr;

		if (auto* nameExpr = dynamic_cast<ast::NameExpr*>(node.callee()))
		{
			Symbol* symbol = currentScope().lookup(nameExpr->name());
			if (!symbol)
				_diagnostics.error(DiagId::UndeclaredIdentifier, node.location(), "use of undeclared identifier '{}'", nameExpr->name());
			else if (symbol->kind == SymbolKind::Function)
			{
				if (symbol->funcDecl && symbol->funcDecl->isInterruptHandler())
				{
					// A handler ends in `iret`, which pops a PC and flags the machine pushed on
					// dispatch. Reached by `call`, it would pop the return address as a PC and
					// whatever sat below it as flags. The vector is the only way in.
					_diagnostics.error(DiagId::InterruptHandlerCalled, node.location(),
						"'{}' is an '__interrupt' handler and cannot be called: it is reached through its vector",
						nameExpr->name());
				}
				else
					funcDecl = symbol->funcDecl;
			}
			else
			{
				// A variable can be the callee too, when what it holds is a pointer to a function.
				const Type* symbolType = symbol->type;
				info = symbolType ? symbolType->calleeSignature() : nullptr;
				if (!info)
					_diagnostics.error(DiagId::CalledObjectNotAFunction, node.location(), "called object '{}' is not a function", nameExpr->name());
				else
					calleeName = std::format("'{}'", nameExpr->name());
			}

			// The callee's own expression type: the function itself for a name that resolves to one
			// (decayArray() is what turns it into a pointer where a value is wanted), or whatever
			// the variable holds.
			if (funcDecl)
				nameExpr->setType(functionTypeOf(funcDecl));
			else if (!info)
				nameExpr->setType(errorRecoveryType());
		}
		else
		{
			const Type* calleeType = decayArray(checkExpr(node.callee()));
			info = calleeType ? calleeType->calleeSignature() : nullptr;
			if (!info)
			{
				_diagnostics.error(DiagId::CalledObjectNotAFunction, node.location(), "called object of type '{}' is not a function or a pointer to one",
					typeName(calleeType));
			}
		}

		if (funcDecl)
		{
			CallSignature signature{ funcDecl->returnType(), funcDecl->params(), funcDecl->isVariadic() };
			checkCallArguments(node, signature, std::format("'{}'", funcDecl->name()));
			resultType = funcDecl->returnType();
			// Two attributes that only mean anything at a call site: deprecated warns wherever the
			// name is called, and warn_unused_result is carried on the call so the enclosing
			// expression statement can warn when its value is dropped.
			if (funcDecl->isDeprecated())
				_diagnostics.warning(DiagId::DeprecatedFunctionUse, node.location(), "call to deprecated function '{}'", funcDecl->name());
			// Only meaningful when the call HAS a result: `void f(void) __attribute__((warn_unused_result))`
			// must not warn that a result nobody could use was ignored.
			if (funcDecl->isWarnUnusedResult() && funcDecl->returnType() && !funcDecl->returnType()->isVoid())
				node.setWarnUnusedResult(true);
		}
		else if (info)
		{
			// A signature carries parameter TYPES and no names (type.h), so the Params handed to
			// checkCallArguments() are built here with the names left empty - nothing reads them.
			std::vector<Param> params;
			params.reserve(info->paramCount);
			for (const Type* paramType : info->params())
				params.push_back(Param{ paramType, {}, node.location() });

			CallSignature signature{ info->returnType, params, info->isVariadic };
			checkCallArguments(node, signature, calleeName);
			resultType = info->returnType;
		}
		else
		{
			// Nothing to check them against, but they are still expressions and their own errors
			// are worth reporting rather than swallowing behind the one above.
			for (Expr* arg : node.args())
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
				if (operandType && operandType->isFunction())
				{
					// `&f` and `f` are the same value in C, and a function is not an lvalue, so this
					// has to come before the lvalue check rather than after it.
					resultType = Type::makePointer(_arena, operandType);
					break;
				}
				if (!isLValue(node.operand()))
					_diagnostics.error(DiagId::AddressOfNonLValue, node.location(), "cannot take the address of a non-lvalue expression");
				else if (auto* name = dynamic_cast<ast::NameExpr*>(node.operand()))
				{
					Symbol* symbol = currentScope().lookup(name->name());
					bool isRegister = symbol && ((symbol->varDecl && symbol->varDecl->storageClass() == ast::StorageClass::Register)
						|| symbol->isRegister);
					if (isRegister)
						_diagnostics.error(DiagId::AddressOfRegisterVariable, node.location(), "cannot take the address of register variable '{}'", name->name());
				}
				resultType = Type::makePointer(_arena, operandType ? operandType : errorRecoveryType());
				break;

			case UnaryOp::Deref:
				if (operandType && (operandType->isPointer() || operandType->isArray()))
					resultType = operandType->arrayElementType();
				else
					_diagnostics.error(DiagId::IndirectionRequiresPointer, node.location(), "indirection requires a pointer operand ('{}' invalid)", typeName(operandType));
				break;

			case UnaryOp::Negate:
				if (isArithmeticType(operandType))
					resultType = integerPromote(operandType);
				else
					_diagnostics.error(DiagId::InvalidUnaryOperand, node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				break;

			case UnaryOp::LogicalNot:
				if (!isScalarType(decayedOperandType))
					_diagnostics.error(DiagId::InvalidUnaryOperand, node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				resultType = &Type::Bool;
				break;

			case UnaryOp::BitwiseNot:
				if (isIntegerType(operandType))
					resultType = integerPromote(operandType);
				else
					_diagnostics.error(DiagId::InvalidUnaryOperand, node.location(), "invalid argument type '{}' to unary expression", typeName(operandType));
				break;

			case UnaryOp::PreIncrement:
			case UnaryOp::PreDecrement:
			case UnaryOp::PostIncrement:
			case UnaryOp::PostDecrement:
				if (!isLValue(node.operand()))
					_diagnostics.error(DiagId::NotAssignable, node.location(), "expression is not assignable");
				else if (operandType && operandType->isConst())
					_diagnostics.error(DiagId::AssignToConst, node.location(), "cannot modify '{}': it is const", typeName(operandType));
				if (!isScalarType(operandType))
					_diagnostics.error(DiagId::IncrementInvalidType, node.location(), "cannot increment/decrement a value of type '{}'", typeName(operandType));
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
			case BinaryOp::Comma:
				// Any operands at all: the left is evaluated for what it does, the right is the value.
				// The result is an rvalue of the right side's (decayed) type.
				resultType = rhsType ? rhsType : errorRecoveryType();
				break;

			case BinaryOp::LogicalOr:
			case BinaryOp::LogicalAnd:
				if (!isScalarType(lhsType) || !isScalarType(rhsType))
					_diagnostics.error(DiagId::InvalidLogicalOperands, node.location(), "invalid operands to logical expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
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
					_diagnostics.error(DiagId::IncompatibleComparison, node.location(), "comparison of incompatible operand types ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				}
				resultType = &Type::Bool;
				break;
			}

			case BinaryOp::BitOr: case BinaryOp::BitXor: case BinaryOp::BitAnd:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(DiagId::InvalidBinaryOperands, node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Shl: case BinaryOp::Shr:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = integerPromote(lhsType); // the rhs never affects the shift's result type
				else
					_diagnostics.error(DiagId::InvalidShiftOperands, node.location(), "invalid operands to shift expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
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
					_diagnostics.error(DiagId::InvalidBinaryOperands, node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Mul: case BinaryOp::Div:
				if (isArithmeticType(lhsType) && isArithmeticType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(DiagId::InvalidBinaryOperands, node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
				break;

			case BinaryOp::Mod:
				if (isIntegerType(lhsType) && isIntegerType(rhsType))
					resultType = commonArithmeticType(lhsType, rhsType);
				else
					_diagnostics.error(DiagId::InvalidBinaryOperands, node.location(), "invalid operands to binary expression ('{}' and '{}')", typeName(lhsType), typeName(rhsType));
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
			_diagnostics.error(DiagId::NotAssignable, node.location(), "expression is not assignable");
		else if (targetType && targetType->isConst())
			_diagnostics.error(DiagId::AssignToConst, node.location(), "cannot assign to '{}': it is const", typeName(targetType));
		else if (targetType && targetType->isAggregate() && node.op() != ast::AssignOp::Assign)
			_diagnostics.error(DiagId::CompoundAssignToStructType, node.location(), "compound assignment is not valid for struct type '{}'", typeName(targetType));
		else if (!isAssignable(targetType, valueType, node.value()))
			_diagnostics.error(DiagId::IncompatibleAssignment, node.location(), "assigning to '{}' from incompatible type '{}'", typeName(targetType), typeName(valueType));

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
			_diagnostics.error(DiagId::SubscriptOnNonPointer, node.location(), "subscripted value ('{}') is not a pointer or array", typeName(arrayType));

		if (!isIntegerType(indexType))
			_diagnostics.error(DiagId::SubscriptNotInteger, node.location(), "array subscript ('{}') is not an integer", typeName(indexType));

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
			if (objectType && objectType->isPointer() && objectType->arrayElementType() && objectType->arrayElementType()->isAggregate())
				structType = objectType->arrayElementType();
			else
				_diagnostics.error(DiagId::MemberBaseNotPointerToStruct, node.location(), "member reference type '{}' is not a pointer to struct", typeName(objectType));
		}
		else
		{
			if (objectType && objectType->isAggregate())
				structType = objectType;
			else
				_diagnostics.error(DiagId::MemberBaseNotStruct, node.location(), "member reference base type '{}' is not a struct", typeName(objectType));
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
						// A member of a qualified struct is qualified too - both ways, and
						// independently, so a `const volatile` object hands each of its fields
						// both. Only `const` used to travel, which left every access to a field of
						// a `volatile struct` unmarked: exactly the memory-mapped register block a
						// program declares `volatile` in the first place.
						//
						// The array case needs nothing here, because there the qualifier already
						// sits on the ELEMENT type and arrayElementType() hands it back as it is;
						// a struct has a field list instead, and nobody was propagating to it.
						resultType = field.type;
						if (structType->isConst())
							resultType = Type::withConst(_arena, resultType);
						if (structType->isVolatile())
							resultType = Type::withVolatile(_arena, resultType);
						found = true;
						break;
					}
				}
			}
			if (!found)
			{
				_diagnostics.error(DiagId::NoSuchMember, node.location(), "no member named '{}' in '{}'", node.memberName(), typeName(structType));
			}
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::CastExpr& node)
	{
		const Type* operandType = checkExpr(node.operand());
		const Type* targetType = node.type(); // set by the parser at construction time - see expr.h

		bool operandIsStruct = operandType && operandType->isAggregate();
		bool targetIsStruct = targetType && targetType->isAggregate();
		if (operandIsStruct != targetIsStruct || (operandIsStruct && targetIsStruct && !(*operandType == *targetType)))
			_diagnostics.error(DiagId::InvalidCast, node.location(), "cannot cast from '{}' to '{}'", typeName(operandType), typeName(targetType));

		_lastExprType = targetType;
	}

	void Sema::visit(ast::SizeofExpr& node)
	{
		if (!node.isTypeArgument())
			checkExpr(node.argumentExpr()); // non-evaluated context, but still checked for errors, same as real C

		const Type* measured = node.isTypeArgument() ? node.argumentType() : node.argumentExpr()->type();
		if (measured && measured->isArray() && measured->arraySize() == 0)
		{
			_diagnostics.error(DiagId::SizeofIncompleteArray, node.location(),
				"invalid application of 'sizeof' to '{}': the array's size is not known here", typeName(measured));
		}

		node.setType(&Type::UInt);
		_lastExprType = &Type::UInt;
	}

	void Sema::visit(ast::AlignofExpr& node)
	{
		node.setType(&Type::UInt);
		_lastExprType = &Type::UInt;
	}

	void Sema::visit(ast::MachineOpExpr& node)
	{
		// Nothing to check. The parser only builds one of these for a name it recognizes, and the
		// grammar already required the empty argument list - none of the three takes an operand, and
		// none produces a value.
		node.setType(&Type::Void);
		_lastExprType = &Type::Void;
	}

	void Sema::visit(ast::BuiltinExpr& node)
	{
		for (ast::Expr* argument : node.args())
			checkExpr(argument);

		// The one-instruction machine builtins have a fixed argument list (the parser enforces its
		// length), so the only thing left is the argument's TYPE and the result's. `abs` and the
		// signed multiply-high take integers and yield an int; the rest of the integer set yields an
		// unsigned int. FloatFromBits reads an integer bit pattern and yields a float; FloatBits and
		// Fclass read a float and yield an integer; everything else float yields a float.
		using ast::Builtin;
		auto requireInteger = [&](const ast::Type* type)
		{
			if (!isIntegerType(type))
				_diagnostics.error(DiagId::InvalidBuiltinOperand, node.location(),
					"'{}' requires an integer argument, not '{}'", ast::builtinName(node.builtin()), typeName(type));
		};
		auto requireFloat = [&](const ast::Type* type)
		{
			if (!type || !type->isFloat())
				_diagnostics.error(DiagId::InvalidBuiltinOperand, node.location(),
					"'{}' requires a float argument, not '{}'", ast::builtinName(node.builtin()), typeName(type));
		};

		const Type* result = &Type::UInt;
		switch (node.builtin())
		{
			case Builtin::FloatBits:
				for (ast::Expr* argument : node.args())
					requireFloat(argument ? argument->type() : nullptr);
				result = &Type::UInt;
				break;
			case Builtin::FloatFromBits:
				for (ast::Expr* argument : node.args())
					requireInteger(argument ? argument->type() : nullptr);
				result = &Type::Float;
				break;
			case Builtin::Fclass:
				for (ast::Expr* argument : node.args())
					requireFloat(argument ? argument->type() : nullptr);
				result = &Type::Int;
				break;
			case Builtin::Abs:
			case Builtin::MulhSigned:
				for (ast::Expr* argument : node.args())
					requireInteger(argument ? argument->type() : nullptr);
				result = &Type::Int;
				break;
			case Builtin::Clz: case Builtin::Ctz: case Builtin::Popcount: case Builtin::Bswap:
			case Builtin::Rotl: case Builtin::Rotr: case Builtin::MulhUnsigned:
				for (ast::Expr* argument : node.args())
					requireInteger(argument ? argument->type() : nullptr);
				result = &Type::UInt;
				break;
			case Builtin::Expect:
				// Its value is the first operand's; the second is a hint. No bank is required.
				result = (!node.args().empty() && node.args()[0]) ? decayArray(node.args()[0]->type()) : &Type::Int;
				if (!result)
					result = &Type::Int;
				break;
			case Builtin::ConstantP:
				result = &Type::Int; // a compile-time 1 or 0; the operand is not evaluated
				break;
			case Builtin::AddOverflow:
			case Builtin::SubOverflow:
			case Builtin::MulOverflow:
			{
				// (a, b, &result): a and b are 4-byte integers and the third operand is a pointer
				// to a 4-byte integer, which is where the machine's word arithmetic lands.
				std::span<ast::Expr* const> args = node.args();
				auto fourByteInteger = [&](usize index) -> bool
				{
					const Type* type = index < args.size() && args[index] ? args[index]->type() : nullptr;
					return type && isIntegerType(type) && type->sizeInBytes() == 4;
				};
				if (!fourByteInteger(0) || !fourByteInteger(1))
					_diagnostics.error(DiagId::InvalidBuiltinOperand, node.location(),
						"'{}' requires two 4-byte integer operands", ast::builtinName(node.builtin()));

				const Type* pointer = args.size() > 2 && args[2] ? args[2]->type() : nullptr;
				const Type* pointee = pointer && pointer->isPointer() ? pointer->arrayElementType() : nullptr;
				if (!pointee || !isIntegerType(pointee) || pointee->sizeInBytes() != 4)
					_diagnostics.error(DiagId::InvalidBuiltinOperand, node.location(),
						"'{}' expects a pointer to a 4-byte integer as its third argument", ast::builtinName(node.builtin()));

				result = &Type::Bool;
				break;
			}
			default: // every float operation
				for (ast::Expr* argument : node.args())
					requireFloat(argument ? argument->type() : nullptr);
				result = &Type::Float;
				break;
		}

		node.setType(result);
		_lastExprType = result;
	}

	void Sema::visit(ast::VaExpr& node)
	{
		using ast::VaOp;

		// Every form writes through its first operand except __builtin_va_end, and C requires an
		// lvalue there in all four cases anyway.
		const Type* listType = checkExpr(node.list());
		const Type* vaListType = Type::makePointer(_arena, &Type::Char); // what `__builtin_va_list` is - see the parser
		if (!isLValue(node.list()))
			_diagnostics.error(DiagId::VaListOperandType, node.list()->location(), "the first argument to '{}' must be an lvalue of type '__builtin_va_list'", ast::vaOpName(node.op()));
		else if (!listType || !(*listType == *vaListType))
			_diagnostics.error(DiagId::VaListOperandType, node.list()->location(), "the first argument to '{}' must have type '__builtin_va_list', not '{}'",
				ast::vaOpName(node.op()), typeName(listType));

		const Type* resultType = &Type::Void;
		switch (node.op())
		{
			case VaOp::Start:
			{
				std::span<const Param> params = _currentFunction ? _currentFunction->params() : std::span<const Param>{};
				if (!_currentFunction || !_currentFunction->isVariadic())
				{
					_diagnostics.error(DiagId::VaStartOutsideVariadic, node.location(), "'__builtin_va_start' is only allowed inside a function declared with '...'");
					break;
				}
				// The second operand must name the LAST fixed parameter. That is not a formality
				// here: the tail begins at the first incoming stack word the fixed parameters did
				// not take (docs/09-Variadic-Convention.md), so naming any other parameter would
				// describe a different starting point than the one __builtin_va_start actually produces.
				auto* name = dynamic_cast<ast::NameExpr*>(node.second());
				if (!name)
					_diagnostics.error(DiagId::VaStartLastParameter, node.second() ? node.second()->location() : node.location(),
						"the second argument to '__builtin_va_start' must name the last named parameter");
				else if (params.empty() || name->name() != params.back().name)
					_diagnostics.error(DiagId::VaStartLastParameter, name->location(),
						"'__builtin_va_start' must name the last named parameter ('{}'), not '{}'",
						params.empty() ? std::string_view("<none>") : params.back().name, name->name());
				if (node.second())
					checkExpr(node.second());
				break;
			}

			case VaOp::Arg:
			{
				const Type* argumentType = node.argumentType();
				// One incoming word per variadic argument, so the type read back has to be exactly
				// one word wide and scalar. A narrower type is C's own undefined behaviour (the
				// default argument promotions mean no `char` was ever passed - an `int` was), and
				// an aggregate has no variadic representation at all, so both are refused here
				// rather than decoded out of a word that does not hold what was asked for.
				if (!argumentType)
					break;
				if (!isScalarType(argumentType) || argumentType->isVoid())
					_diagnostics.error(DiagId::VaArgType, node.location(), "'__builtin_va_arg' cannot read type '{}': only scalar types are passed through '...'",
						typeName(argumentType));
				else if (argumentType->sizeInBytes() != 4)
					_diagnostics.error(DiagId::VaArgType, node.location(),
						"'__builtin_va_arg' cannot read type '{}': a variadic argument arrives promoted to a 4-byte type, so read it as 'int' and convert",
						typeName(argumentType));
				else
					resultType = argumentType;
				break;
			}

			case VaOp::Copy:
			{
				const Type* sourceType = node.second() ? checkExpr(node.second()) : nullptr;
				if (node.second() && (!sourceType || !(*sourceType == *vaListType)))
					_diagnostics.error(DiagId::VaListOperandType, node.second()->location(), "the second argument to '__builtin_va_copy' must have type '__builtin_va_list', not '{}'",
						typeName(sourceType));
				break;
			}

			case VaOp::End:
				break;
		}

		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::GenericSelectionExpr& node)
	{
		// The controlling expression is a non-evaluated context at RUN time - node.controlling() is
		// never lowered to IR except through whichever association's expr gets selected below - but
		// it is still type-checked here, the same way sizeof(x++) still type-checks x++. Its own
		// value is never used for anything past this point; only its type is.
		const Type* controllingType = checkExpr(node.controlling());

		i32 matchIndex = -1;
		i32 defaultIndex = -1;
		std::span<const ast::GenericAssoc> assocs = node.associations();
		for (usize i = 0; i < assocs.size(); ++i)
		{
			const ast::GenericAssoc& assoc = assocs[i];
			if (!assoc.type) // `default:`
			{
				if (defaultIndex != -1)
					_diagnostics.error(DiagId::MultipleGenericDefaults, node.location(), "'_Generic' selection has more than one 'default' association");
				defaultIndex = static_cast<i32>(i);
			}
			else
			{
				for (usize j = 0; j < i; ++j)
				{
					if (assocs[j].type && *assocs[j].type == *assoc.type)
					{
						_diagnostics.error(DiagId::DuplicateGenericAssociation, node.location(),
							"'_Generic' selection has type '{}' in more than one association", typeName(assoc.type));
						break;
					}
				}
				if (matchIndex == -1 && controllingType && *controllingType == *assoc.type)
					matchIndex = static_cast<i32>(i);
			}
			// Every association's expr is checked, selected or not - it has to at least parse and
			// type-check as ordinary C, exactly like the non-taken arm of `if (0) { ... }` does.
			checkExpr(assoc.expr);
		}

		i32 selected = matchIndex != -1 ? matchIndex : defaultIndex;
		if (selected == -1)
		{
			_diagnostics.error(DiagId::NoMatchingGenericAssociation, node.location(),
				"'_Generic' selection has no association for type '{}', and no 'default'", typeName(controllingType));
			node.setType(errorRecoveryType());
			_lastExprType = errorRecoveryType();
			return;
		}

		node.setSelectedIndex(selected);
		const Type* resultType = assocs[static_cast<usize>(selected)].expr->type();
		node.setType(resultType);
		_lastExprType = resultType;
	}

	void Sema::visit(ast::TernaryExpr& node)
	{
		const Type* condType = decayArray(checkExpr(node.cond()));
		const Type* thenType = decayArray(checkExpr(node.thenExpr()));
		const Type* elseType = decayArray(checkExpr(node.elseExpr()));

		if (!isScalarType(condType))
			_diagnostics.error(DiagId::ConditionNotScalar, node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

		const Type* resultType = errorRecoveryType();
		if (thenType && elseType && *thenType == *elseType)
			resultType = thenType;
		else if (isArithmeticType(thenType) && isArithmeticType(elseType))
			resultType = commonArithmeticType(thenType, elseType);
		else if (thenType && elseType && thenType->isPointer() && elseType->isPointer() && isNullPointerConstant(node.elseExpr()))
			resultType = thenType; // `c ? handler : NULL`: a null pointer takes the type of the other arm
		else if (thenType && elseType && thenType->isPointer() && elseType->isPointer() && isNullPointerConstant(node.thenExpr()))
			resultType = elseType;
		else if (thenType && elseType && thenType->isPointer() && elseType->isPointer() &&
			(isAssignable(thenType, elseType) || isAssignable(elseType, thenType)))
		{
			// `c ? p : q` with two pointers: the result is the one that can hold the other - the
			// pointer to const over the plain one, `void*` over a typed one - and it is an error
			// only when neither direction is a conversion this subset allows.
			resultType = isAssignable(thenType, elseType) ? thenType : elseType;
		}
		else if (thenType && elseType && thenType->isPointer() && isIntegerType(elseType) && isNullPointerConstant(node.elseExpr()))
			resultType = thenType; // `c ? p : 0`
		else if (thenType && elseType && elseType->isPointer() && isIntegerType(thenType) && isNullPointerConstant(node.thenExpr()))
			resultType = elseType; // `c ? 0 : p`
		else
			_diagnostics.error(DiagId::IncompatibleTernaryOperands, node.location(), "incompatible operand types ('{}' and '{}') in ternary expression", typeName(thenType), typeName(elseType));

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
		_diagnostics.error(DiagId::InitializerListOutsideVariable, node.location(), "an initializer list is only valid as a variable's initializer");
		for (Expr* element : node.elements())
			checkExpr(element);
		node.setType(errorRecoveryType());
		_lastExprType = node.type();
	}

	void Sema::visit(ast::EmptyStmt&) {}

	void Sema::visit(ast::ExprStmt& node)
	{
		checkExpr(node.expr());
		// `f(x);` throws the result away. When f asked not to have that done, say so - at the
		// statement, which is the only place a discarded result exists.
		if (auto* call = dynamic_cast<ast::CallExpr*>(node.expr());
			call && call->warnUnusedResult() && call->type() && !call->type()->isVoid())
			_diagnostics.warning(DiagId::UnusedResult, call->location(), "the result of this call is ignored, but the function is declared 'warn_unused_result'");
	}

	void Sema::visit(ast::DeclStmt& node)
	{
		for (ast::Decl* decl : node.decls())
			decl->accept(*this);
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
			_diagnostics.error(DiagId::ConditionNotScalar, node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

		checkStmt(node.thenStmt());
		checkStmt(node.elseStmt());
	}

	void Sema::visit(ast::WhileStmt& node)
	{
		const Type* condType = decayArray(checkExpr(node.cond()));
		if (!isScalarType(condType))
			_diagnostics.error(DiagId::ConditionNotScalar, node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));

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
			_diagnostics.error(DiagId::ConditionNotScalar, node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));
	}

	void Sema::visit(ast::ForStmt& node)
	{
		pushScope(); // `for (int i = 0; ...)` scopes `i` to the loop, not the enclosing block
		checkStmt(node.init());

		if (node.cond())
		{
			const Type* condType = decayArray(checkExpr(node.cond()));
			if (!isScalarType(condType))
				_diagnostics.error(DiagId::ConditionNotScalar, node.location(), "used type '{}' where arithmetic or pointer type is required", typeName(condType));
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
		if (_currentFunction && _currentFunction->isNoReturn())
		{
			_diagnostics.warning(DiagId::NoReturnFunctionReturns, node.location(),
				"function '{}' is declared 'noreturn' but contains a 'return'", _currentFunction->name());
		}

		if (node.value())
		{
			const Type* valueType = decayArray(checkExpr(node.value()));
			if (returnType->isVoid())
				_diagnostics.error(DiagId::ReturnValueFromVoid, node.location(), "void function should not return a value");
			else if (!isAssignable(returnType, valueType, node.value()))
				_diagnostics.error(DiagId::IncompatibleReturnType, node.location(), "returning '{}' from a function with incompatible result type '{}'", typeName(valueType), typeName(returnType));
		}
		else if (!returnType->isVoid())
		{
			_diagnostics.error(DiagId::MissingReturnValue, node.location(), "non-void function should return a value");
		}
	}

	void Sema::visit(ast::BreakStmt& node)
	{
		if (_loopDepth == 0 && _switchStack.empty())
			_diagnostics.error(DiagId::BreakOutsideLoopOrSwitch, node.location(), "'break' statement not in a loop or switch statement");
	}

	void Sema::visit(ast::ContinueStmt& node)
	{
		if (_loopDepth == 0)
			_diagnostics.error(DiagId::ContinueOutsideLoop, node.location(), "'continue' statement not in a loop");
	}

	void Sema::visit(ast::SwitchStmt& node)
	{
		const Type* condType = checkExpr(node.cond());
		if (!isIntegerType(condType))
			_diagnostics.error(DiagId::SwitchConditionNotInteger, node.location(), "switch condition type '{}' is not an integer", typeName(condType));

		_switchStack.emplace_back();
		checkStmt(node.body());
		_switchStack.pop_back();
	}

	void Sema::visit(ast::CaseStmt& node)
	{
		if (_switchStack.empty())
			_diagnostics.error(DiagId::CaseOutsideSwitch, node.location(), "'case' statement not in a switch statement");

		checkExpr(node.value());
		std::optional<i64> value = evalConstantExpr(node.value());

		// `case low ... high:` - the high bound is a second constant expression, and the range must
		// be non-empty and small enough to expand (the dispatch enumerates every value in it).
		std::optional<i64> upper;
		if (node.upper())
		{
			checkExpr(node.upper());
			upper = evalConstantExpr(node.upper());
		}

		constexpr i64 kMaxCaseRangeSpan = 65536;
		if (!value || (node.upper() && !upper))
		{
			_diagnostics.error(DiagId::CaseNotConstant, node.location(), "case label does not reduce to an integer constant");
		}
		else if (node.upper() && *upper < *value)
		{
			_diagnostics.error(DiagId::CaseRangeEmpty, node.location(), "empty case range: the low bound is greater than the high bound");
		}
		else if (node.upper() && static_cast<u64>(*upper) - static_cast<u64>(*value) > static_cast<u64>(kMaxCaseRangeSpan))
		{
			_diagnostics.error(DiagId::CaseRangeTooLarge, node.location(), "case range spans more than {} values", kMaxCaseRangeSpan);
		}
		else if (!_switchStack.empty())
		{
			SwitchContext& context = _switchStack.back();
			i64 high = node.upper() ? *upper : *value;
			// Iterate by offset, not by value: `high` can be INT64_MAX (a legal one-value range),
			// and `++value` there would overflow a signed integer. The span is bounded by sema's
			// own cap above.
			u64 span = static_cast<u64>(high) - static_cast<u64>(*value);
			i64 firstDuplicate = 0;
			bool foundDuplicate = false;
			for (u64 offset = 0; offset <= span; ++offset)
			{
				i64 candidate = *value + static_cast<i64>(offset);
				if (!context.seenCaseValues.insert(candidate).second && !foundDuplicate)
				{
					foundDuplicate = true;
					firstDuplicate = candidate;
				}
			}
			if (foundDuplicate)
				_diagnostics.error(DiagId::DuplicateCaseValue, node.location(), "duplicate case value '{}'", firstDuplicate);
		}

		checkStmt(node.body());
	}

	void Sema::visit(ast::DefaultStmt& node)
	{
		if (_switchStack.empty())
		{
			_diagnostics.error(DiagId::DefaultOutsideSwitch, node.location(), "'default' statement not in a switch statement");
		}
		else
		{
			SwitchContext& context = _switchStack.back();
			if (context.defaultSeen)
				_diagnostics.error(DiagId::MultipleDefaultLabels, node.location(), "multiple default labels in one switch statement");
			else
				context.defaultSeen = true;
		}

		checkStmt(node.body());
	}

	void Sema::visit(ast::GotoStmt& node)
	{
		if (std::find(_currentFunctionLabels.begin(), _currentFunctionLabels.end(), node.label()) == _currentFunctionLabels.end())
			_diagnostics.error(DiagId::UndeclaredLabel, node.location(), "use of undeclared label '{}'", node.label());
	}

	void Sema::visit(ast::LabelStmt& node)
	{
		checkStmt(node.body());
	}

	// ---- declarations -----------------------------------------------------------------------------

	// ---- storage classes --------------------------------------------------------------------------

	void Sema::checkStorageClass(ast::VarDecl& node)
	{
		bool atFileScope = (&currentScope() == _globalScope);
		ast::StorageClass storageClass = node.storageClass();

		if (storageClass == ast::StorageClass::Auto && atFileScope)
		{
			// `auto` means automatic storage, which is what a block gives a variable and a file
			// scope cannot. It is also the default in a block, so it never says anything new there -
			// but it is legal C, and rejecting it at file scope is the only rule it carries.
			_diagnostics.error(DiagId::AutoOutsideBlock, node.location(), "'auto' is only allowed on a variable declared inside a block");
		}

		if (storageClass == ast::StorageClass::Register && atFileScope)
		{
			// Same reason as `auto` above: `register` asks for automatic storage that lives in a
			// register, and a file-scope object has static storage duration whatever it is asked
			// for. Its one real consequence - that the address cannot be taken - would also be a
			// promise this compiler could not keep for something a whole other unit may refer to.
			_diagnostics.error(DiagId::RegisterOutsideBlock, node.location(), "'register' is only allowed on a variable declared inside a block");
		}

		if (storageClass == ast::StorageClass::Register && node.type() && node.type()->isArray())
		{
			// An array decays to a pointer to its first element the moment it is used for anything
			// but `sizeof`, and that decay IS taking its address - so every use of a `register`
			// array breaks the one promise the keyword makes. The `&` check in visit(UnaryExpr&)
			// cannot see it, because there is no `&` written anywhere; rejecting the declaration is
			// the whole rule rather than chasing each use.
			_diagnostics.error(DiagId::RegisterOnArray, node.location(),
				"'register' is not allowed on array '{}': using an array takes its address", node.name());
		}

		if (storageClass == ast::StorageClass::Extern && node.initializer() && !atFileScope)
		{
			// At file scope `extern int x = 1;` is a definition with external linkage, which is
			// legal and means exactly what `int x = 1;` means. Inside a block there is nothing for
			// it to define - the object belongs to whatever unit declares it at file scope.
			_diagnostics.error(DiagId::ExternInitializerInBlock, node.location(), "'extern' variable '{}' cannot have an initializer inside a block", node.name());
		}

		if (storageClass == ast::StorageClass::Static && node.initializer() && !isConstantInitializer(node.initializer()))
		{
			// A static's initializer runs once, at load time, so it becomes bytes in the image -
			// there is no moment at which a run-time expression could be evaluated for it. Reported
			// here rather than by codegen so the message names the variable and its declaration.
			_diagnostics.error(DiagId::StaticInitializerNotConstant, node.initializer()->location(),
				"the initializer of '{}' must be a compile-time constant, because it has static storage", node.name());
		}

		if (node.type() && node.type()->isConst() && !node.initializer() &&
			storageClass != ast::StorageClass::Extern)
		{
			// Nothing may ever write it, so a const object with no initializer can only ever hold
			// whatever it was loaded with - zero, in practice. An `extern` one is exempt: the
			// initializer is somewhere else by definition.
			_diagnostics.warning(DiagId::ConstWithoutInitializer, node.location(), "const variable '{}' has no initializer, so it can only ever be zero", node.name());
		}
	}

	namespace
	{
		// Has a name, in an initializer, a fixed address? A global does (`static` or not), and so does a
		// `static` or `extern` local and any function; an automatic local or a parameter lives in a
		// frame and has none.
		bool hasStaticStorage(const Symbol* symbol)
		{
			if (!symbol)
				return false;
			if (symbol->kind == SymbolKind::Function)
				return true;
			if (symbol->kind != SymbolKind::Variable)
				return false;
			if (symbol->isGlobal)
				return true;
			if (!symbol->varDecl)
				return false;
			const ast::StorageClass storage = symbol->varDecl->storageClass();
			return storage == ast::StorageClass::Static || storage == ast::StorageClass::Extern;
		}

		// Do two declarations of one name mean the same type? An array whose size is not known (`extern int t[];`,
		// size 0) agrees with the same array of any size: they are one object, and the known size is what it has.
		bool sameOrCompletes(const Type* a, const Type* b)
		{
			if (*a == *b)
				return true;
			if (!a->isArray() || !b->isArray())
				return false;
			if (a->arraySize() != b->arraySize() && a->arraySize() != 0 && b->arraySize() != 0)
				return false;
			return a->isConst() == b->isConst() && a->isVolatile() == b->isVolatile() &&
				*a->arrayElementType() == *b->arrayElementType();
		}
	}

	bool Sema::isConstantInitializer(const ast::Expr* expr)
	{
		if (!expr)
			return true;
		if (dynamic_cast<const ast::IntLiteralExpr*>(expr) || dynamic_cast<const ast::FloatLiteralExpr*>(expr) ||
			dynamic_cast<const ast::CharLiteralExpr*>(expr) || dynamic_cast<const ast::BoolLiteralExpr*>(expr) ||
			dynamic_cast<const ast::StringLiteralExpr*>(expr))
		{
			return true;
		}
		// Arithmetic on constants - `2 + 3`, `sizeof(a) / sizeof(a[0])`, `FLAG_A | FLAG_B`, `n ? 1 : 2` - is a
		// constant, and its value is what goes in the image.
		if (dynamic_cast<const ast::BinaryExpr*>(expr) || dynamic_cast<const ast::TernaryExpr*>(expr) ||
			dynamic_cast<const ast::SizeofExpr*>(expr))
		{
			recordConstant(const_cast<Expr*>(expr));
			return expr->constantValue().has_value();
		}
		if (auto* list = dynamic_cast<const ast::InitListExpr*>(expr))
		{
			for (const ast::Expr* element : list->elements())
			{
				if (!isConstantInitializer(element))
					return false;
			}
			return true;
		}
		if (auto* unary = dynamic_cast<const ast::UnaryExpr*>(expr))
		{
			// `-1` and `!0` are constants; `*p` is not, and neither is `++x`. `&x` is, when x has
			// static storage: the address is fixed once the program is linked.
			switch (unary->op())
			{
				case ast::UnaryOp::Negate:
				case ast::UnaryOp::LogicalNot:
				case ast::UnaryOp::BitwiseNot:
					return isConstantInitializer(unary->operand());
				case ast::UnaryOp::AddressOf:
					if (const auto* name = dynamic_cast<const ast::NameExpr*>(unary->operand()))
						return hasStaticStorage(currentScope().lookup(name->name()));
					return false;
				default:
					return false;
			}
		}
		// `(void*)0` - what NULL is - and `(char*)table`: a cast of a constant is a constant.
		if (const auto* cast = dynamic_cast<const ast::CastExpr*>(expr))
			return isConstantInitializer(cast->operand());
		if (const auto* name = dynamic_cast<const ast::NameExpr*>(expr))
		{
			// An array or a function name decays to its own address. (A plain variable is a value, and
			// its value is not known until the program runs.)
			const Symbol* symbol = currentScope().lookup(name->name());
			if (symbol && symbol->kind == SymbolKind::EnumConstant)
			{
				recordConstant(const_cast<Expr*>(expr));      // an enumerator is a constant, and a number
				return true;
			}
			if (!hasStaticStorage(symbol))
				return false;
			return symbol->kind == SymbolKind::Function || (symbol->type && symbol->type->isArray());
		}
		return false;
	}

	void Sema::checkAsmLabel(ast::Decl& node, bool atFileScope)
	{
		const std::string_view label = node.asmLabel().view();
		if (!node.asmLabel())
			return;

		if (!atFileScope)
		{
			_diagnostics.error(DiagId::AsmLabelNotAtFileScope, node.location(),
				"an asm label is only allowed on a declaration at file scope, not on '{}' here", node.name());
			return;
		}

		// What CeresASM reads as a plain symbol: letters, digits and underscores, not starting with a digit.
		bool valid = !label.empty() && !(label.front() >= '0' && label.front() <= '9');
		for (char c : label)
			valid = valid && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
		if (!valid)
		{
			_diagnostics.error(DiagId::InvalidAsmLabel, node.location(),
				"'{}' cannot be the asm label of '{}': it has to be a plain identifier (letters, digits and '_')", label, node.name());
			return;
		}

		auto [it, inserted] = _asmLabels.emplace(node.name(), label);
		if (!inserted && it->second != label)
		{
			_diagnostics.error(DiagId::ConflictingAsmLabel, node.location(),
				"'{}' is declared with the asm label '{}' here and '{}' before", node.name(), label, it->second);
		}
	}

	void Sema::visit(ast::VarDecl& node)
	{
		if (node.type() && node.type()->isVoid())
			_diagnostics.error(DiagId::VoidVariable, node.location(), "variable '{}' declared with type 'void'", node.name());

		// Before anything looks at the initializer: a list with designators is rewritten into the positional one it
		// means, which is the only kind the checks below (and code generation after them) understand.
		if (auto* list = dynamic_cast<ast::InitListExpr*>(node.initializer()))
			normalizeInitList(node.type(), *list);

		checkStorageClass(node);
		checkAsmLabel(node, &currentScope() == _globalScope);

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

		// At file scope a name may be declared more than once and still mean one object: that is
		// what an `extern` declaration in a header followed by the definition in a source file IS,
		// and it is the whole reason headers work. Redeclaring a LOCAL is still an error - a block
		// has no such notion.
		Symbol* previous = symbol.isGlobal ? _globalScope->lookupInThisScope(node.name()) : nullptr;
		if (previous && previous->kind == SymbolKind::Variable)
		{
			bool sameType = previous->type && node.type() && sameOrCompletes(previous->type, node.type());
			if (!sameType)
			{
				_diagnostics.error(DiagId::ConflictingTypes, node.location(), "redeclaration of '{}' with a different type ('{}' after '{}')",
					node.name(), typeName(node.type()), typeName(previous->type));
			}
			else if (previous->varDecl && previous->varDecl->initializer() && node.initializer())
			{
				// Two initializers is the one case that really is a redefinition: there is no way to
				// tell which value the object should start with.
				_diagnostics.error(DiagId::Redefinition, node.location(), "redefinition of '{}'", node.name());
			}
			else if (previous->varDecl &&
				((previous->varDecl->storageClass() == ast::StorageClass::Static) != (node.storageClass() == ast::StorageClass::Static)))
			{
				_diagnostics.error(DiagId::ConflictingLinkage, node.location(), "redeclaration of '{}' has conflicting linkage", node.name());
			}
			else if (node.initializer())
			{
				// The definition wins the slot, so a later reference resolves to the declaration that
				// actually has the value - which is what checkStorageClass() and codegen both read.
				previous->varDecl = &node;
			}

			// `extern int t[];` and then `int t[4] = ...;`: from here on the array has its size.
			if (sameType && previous->type->isArray() && previous->type->arraySize() == 0 && node.type()->arraySize() != 0)
				previous->type = node.type();
		}
		else
		{
			declareSymbol(symbol);
		}

		if (node.initializer())
			checkInitializer(node.type(), node.initializer());
	}

	// Everything `__interrupt` promises, checked in one place. A handler is not called - it is
	// dispatched to, by the machine, at a point the surrounding code never chose - so there is no
	// caller to pass it an argument, none to read a result, and none whose registers it may
	// disturb. Each rule below is that one fact seen from a different side.
	void Sema::checkInterruptHandler(ast::FunctionDecl& node)
	{
		if (!node.isInterruptHandler())
			return;

		if (node.returnType() && !node.returnType()->isVoid())
		{
			_diagnostics.error(DiagId::InterruptHandlerReturnType, node.location(),
				"an '__interrupt' handler must return 'void', not '{}': nothing is there to receive a result",
				typeName(node.returnType()));
		}
		if (!node.params().empty() || node.isVariadic())
		{
			_diagnostics.error(DiagId::InterruptHandlerTakesNoParameters, node.location(),
				"an '__interrupt' handler takes no parameters: nothing is there to pass one");
		}
		if (node.name() == "main")
		{
			// The reset vector is the entry point, and the linker finds it by this name. A handler
			// ending in `iret` would return to a PC and flags nothing ever pushed.
			_diagnostics.error(DiagId::MainCannotBeInterrupt, node.location(), "'main' cannot be an '__interrupt' handler");
		}
	}

	void Sema::visit(ast::FunctionDecl& node)
	{
		checkInterruptHandler(node);
		checkAsmLabel(node, true);

		// Every function attribute declared on a prototype holds for the definition that comes
		// after it, exactly as `noreturn` always has. The definition wins where it says otherwise.
		if (Symbol* declared = _globalScope->lookupInThisScope(node.name());
			declared && declared->kind == SymbolKind::Function && declared->funcDecl)
		{
			const ast::FunctionDecl* prototype = declared->funcDecl;
			if (prototype->isNoReturn())
				node.setNoReturn(true);
			// A definition's own attribute wins over a prototype's where the two contradict:
			// `noinline` then `always_inline` must leave the definition inlinable.
			if (prototype->isNoInline() && !node.isAlwaysInline())
				node.setNoInline(true);
			if (prototype->isAlwaysInline() && !node.isNoInline())
				node.setAlwaysInline(true);
			if (prototype->isConstAttr())
				node.setConstAttr(true);
			else if (prototype->isPure())
				node.setPure(true);
			if (prototype->isDeprecated())
				node.setDeprecated(true);
			if (prototype->isWarnUnusedResult())
				node.setWarnUnusedResult(true);
		}

		Symbol* existing = _globalScope->lookupInThisScope(node.name());
		bool kindConflict = existing && existing->kind != SymbolKind::Function;
		if (kindConflict)
			_diagnostics.error(DiagId::RedefinitionAsDifferentKind, node.location(), "redefinition of '{}' as a different kind of symbol", node.name());

		if (existing && !kindConflict)
		{
			ast::FunctionDecl* previous = existing->funcDecl;
			// `...` is part of the signature, not a detail of one declaration: a prototype and a
			// definition that disagree about it disagree about the calling convention every call
			// site was already compiled against (docs/09-Variadic-Convention.md).
			bool signatureMatches = previous && previous->returnType() && node.returnType() &&
				*previous->returnType() == *node.returnType() &&
				previous->params().size() == node.params().size() &&
				previous->isVariadic() == node.isVariadic();
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
				_diagnostics.error(DiagId::ConflictingTypes, node.location(), "conflicting types for '{}'", node.name());
			else if (previous && previous->isDefinition() && node.isDefinition())
				_diagnostics.error(DiagId::RedefinitionOfFunction, node.location(), "redefinition of function '{}'", node.name());

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
			if (param.name.empty())
			{
				// A prototype may leave a parameter unnamed, and a type-name has nowhere to put a
				// name at all - but a definition's body would have no way to refer to it.
				_diagnostics.error(DiagId::UnnamedParameterInDefinition, param.location, "a parameter of a function definition must be named");
				continue;
			}
			Symbol paramSymbol;
			paramSymbol.kind = SymbolKind::Parameter;
			paramSymbol.name = param.name;
			paramSymbol.type = param.type;
			paramSymbol.location = param.location;
			paramSymbol.isRegister = param.isRegister;
			declareSymbol(paramSymbol);
		}

		const Type* previousReturnType = _currentFunctionReturnType;
		const ast::FunctionDecl* previousFunction = _currentFunction;
		_currentFunction = &node;
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
		_currentFunction = previousFunction;
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
					_diagnostics.error(DiagId::EnumeratorNotConstant, enumerator.location, "enumerator value for '{}' is not a constant expression", enumerator.name);
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

	void Sema::visit(ast::StaticAssertDecl& node)
	{
		// The condition is an integer constant expression, folded with the same machinery that gives a
		// static initializer its value: enumerators, sizeof of any complete type, arithmetic, comparisons.
		checkExpr(node.condition());
		std::optional<i64> value = evalConstantExpr(node.condition());
		if (!value)
		{
			_diagnostics.error(DiagId::StaticAssertNotConstant, node.condition()->location(),
				"the condition of a static assertion must be a constant expression");
			return;
		}
		if (*value != 0)
			return;

		if (node.hasMessage())
			_diagnostics.error(DiagId::StaticAssertFailed, node.location(), "static assertion failed: {}", node.message().view());
		else
			_diagnostics.error(DiagId::StaticAssertFailed, node.location(), "static assertion failed");
	}

	void Sema::visit(ast::InterruptVectorDecl& node)
	{
		// Four rules, all of them the linker's own (CeresASM 26-Interrupt-Vector-Binding.md) - caught
		// here so the message names the C declaration instead of generated assembly.

		// 1. The number has to fold. It is written as any constant expression, so an enum constant
		//    or a macro works and a variable does not.
		checkExpr(node.number());
		std::optional<i64> vector = evalConstantExpr(node.number());
		if (!vector)
		{
			_diagnostics.error(DiagId::InterruptNumberNotConstant, node.numberLocation(), "an interrupt number must be a constant expression");
			return;
		}

		// 2. 0 is the reset vector - the entry point, which `main` already is. 64 is the end of the
		//    table.
		if (*vector == 0)
		{
			_diagnostics.error(DiagId::InterruptVectorIsReset, node.numberLocation(),
				"interrupt 0 is the reset vector: it is where the program starts, which is what 'main' already is");
			return;
		}
		if (*vector < 0 || *vector > 63)
		{
			_diagnostics.error(DiagId::InterruptNumberOutOfRange, node.numberLocation(),
				"interrupt number {} is out of range: the vector table holds 1 to 63", *vector);
			return;
		}
		node.setResolvedNumber(*vector);

		// 3. The target must be an `__interrupt` handler. An ordinary function would end in `ret`
		//    and pop the flags the dispatcher pushed as a return address.
		Symbol* symbol = _globalScope->lookupInThisScope(node.name());
		if (!symbol || symbol->kind != SymbolKind::Function)
		{
			_diagnostics.error(DiagId::InterruptVectorTargetNotAFunction, node.location(), "'{}' is not a function", node.name());
			return;
		}
		if (!symbol->funcDecl || !symbol->funcDecl->isInterruptHandler())
		{
			_diagnostics.error(DiagId::InterruptVectorTargetNotInterrupt, node.location(),
				"'{}' is not declared '__interrupt', so it cannot be an interrupt handler", node.name());
			return;
		}

		// 4. One binding per number. Whole-program uniqueness is the linker's to enforce, since it
		//    is the only thing that sees every object; this catches the case inside one file, where
		//    the message can point at both declarations.
		auto [it, inserted] = _interruptVectors.try_emplace(*vector, &node);
		if (!inserted)
		{
			_diagnostics.error(DiagId::InterruptVectorAlreadyBound, node.location(),
				"interrupt {} is already bound to '{}'", *vector, it->second->name());
		}
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
