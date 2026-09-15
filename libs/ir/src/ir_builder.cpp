#include <ceresc/ir/ir_builder.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/decl.h>

#include <cstring>
#include <format>

namespace ceresc::ir
{
	using ast::Type;
	using ast::TypeKind;
	using ast::Expr;
	using ast::Stmt;
	using ast::UnaryOp;
	using ast::BinaryOp;
	using ast::AssignOp;
	using ast::Param;
	using ast::FieldDecl;
	using ast::EnumeratorDecl;

	namespace
	{
		IrCmpPredicate cmpPredicateFor(BinaryOp op) noexcept
		{
			switch (op)
			{
				case BinaryOp::Eq: return IrCmpPredicate::Eq;
				case BinaryOp::Ne: return IrCmpPredicate::Ne;
				case BinaryOp::Lt: return IrCmpPredicate::Lt;
				case BinaryOp::Le: return IrCmpPredicate::Le;
				case BinaryOp::Gt: return IrCmpPredicate::Gt;
				default: return IrCmpPredicate::Ge; // BinaryOp::Ge - the only case left among the comparison ops
			}
		}

		// A pointer operand always compares unsigned (addresses have no sign); otherwise unsigned
		// if either side's own resolved type is unsigned - matches §10's "ifbl/ifbe/ifab/ifae
		// (unsigned, punteros, tamaños)" note. Bool/Float report isSigned()==false too (type.cpp) -
		// a float comparison being marked "unsigned" here is a known v1 simplification: codegen does
		// not exist yet (Fase 6), and this phase's own exit criterion only checks control-flow
		// shape, not signed/unsigned dispatch - see the header comment on IrBuilder's contract.
		bool isUnsignedComparison(const Type* lhs, const Type* rhs) noexcept
		{
			if ((lhs && lhs->isPointer()) || (rhs && rhs->isPointer()))
				return true;
			if (lhs && !lhs->isSigned())
				return true;
			if (rhs && !rhs->isSigned())
				return true;
			return false;
		}

		BinaryOp binaryOpForCompoundAssign(AssignOp op) noexcept
		{
			switch (op)
			{
				case AssignOp::AddAssign: return BinaryOp::Add;
				case AssignOp::SubAssign: return BinaryOp::Sub;
				case AssignOp::MulAssign: return BinaryOp::Mul;
				case AssignOp::DivAssign: return BinaryOp::Div;
				case AssignOp::ModAssign: return BinaryOp::Mod;
				case AssignOp::AndAssign: return BinaryOp::BitAnd;
				case AssignOp::OrAssign: return BinaryOp::BitOr;
				case AssignOp::XorAssign: return BinaryOp::BitXor;
				case AssignOp::ShlAssign: return BinaryOp::Shl;
				case AssignOp::ShrAssign: return BinaryOp::Shr;
				default: return BinaryOp::Add; // AssignOp::Assign never reaches here - see visit(AssignExpr&)
			}
		}

		// Duplicates Sema::integerPromote()/commonArithmeticType() (sema.cpp, both private members -
		// not reusable from here, same reason foldConstant() duplicates evalConstantExpr()): `x op=
		// y` means `x = x op y` by definition, so it must pick the same result type the equivalent
		// BinaryExpr would - lowerArithmetic()'s plain-arithmetic fallback reads its own isUnsigned
		// off of *this* promoted type, not off of `x`'s raw, unpromoted declared type, or a narrow
		// unsigned target (e.g. `unsigned char`) would disagree with `x = x op y` on whether the
		// operation itself is signed or unsigned.
		const Type* integerPromote(const Type* type) noexcept
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

		const Type* commonArithmeticType(const Type* lhs, const Type* rhs) noexcept
		{
			const Type* promotedLhs = integerPromote(lhs);
			const Type* promotedRhs = integerPromote(rhs);
			return arithmeticRank(promotedLhs) >= arithmeticRank(promotedRhs) ? promotedLhs : promotedRhs;
		}
	}

	// ---- construction / entry point -----------------------------------------------------------

	IrBuilder::IrBuilder(support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept :
		_arena(arena), _diagnostics(diagnostics)
	{}

	IrModule IrBuilder::build(ast::TranslationUnit& unit)
	{
		// Resets every piece of state build() itself owns, mirroring Sema::check()'s own identical
		// reset (sema.cpp) - _caseBlocks/_labelBlocks in particular key off of raw ast::Stmt*
		// identity, so a second build() call on this same instance (whether over a different
		// TranslationUnit, or one backed by an Arena that happens to reuse a freed address) could
		// otherwise alias a fresh CaseStmt/LabelStmt onto a stale entry from the previous call and
		// jump into the wrong block.
		_module = IrModule{};
		_currentFunction = nullptr;
		_currentBlock = nullptr;
		_lastValue = IrValue{};
		_scopes.clear();
		_globalSymbols.clear();
		_breakTargets.clear();
		_continueTargets.clear();
		_labelBlocks.clear();
		_caseBlocks.clear();
		_nextStringLiteralId = 0;

		unit.accept(*this);
		return std::move(_module);
	}

	// ---- scope / symbol helpers -----------------------------------------------------------------

	void IrBuilder::pushScope() { _scopes.emplace_back(); }
	void IrBuilder::popScope() { if (!_scopes.empty()) _scopes.pop_back(); }

	void IrBuilder::declareSymbol(std::string_view name, const LocalSymbol& symbol)
	{
		if (_currentFunction && !_scopes.empty())
			_scopes.back()[name] = symbol;
		else
			_globalSymbols[name] = symbol;
	}

	IrBuilder::LocalSymbol* IrBuilder::lookupSymbol(std::string_view name) noexcept
	{
		for (auto it = _scopes.rbegin(); it != _scopes.rend(); ++it)
		{
			auto found = it->find(name);
			if (found != it->end())
				return &found->second;
		}
		auto found = _globalSymbols.find(name);
		return found != _globalSymbols.end() ? &found->second : nullptr;
	}

	// ---- dispatch helpers -------------------------------------------------------------------------

	IrValue IrBuilder::lowerExpr(Expr* expr)
	{
		if (!expr)
			return IrValue{};
		expr->accept(*this);
		return _lastValue;
	}

	void IrBuilder::lowerStmt(Stmt* stmt)
	{
		if (stmt)
			stmt->accept(*this);
	}

	// ---- block helpers ------------------------------------------------------------------------------

	void IrBuilder::switchToBlock(BasicBlock& block, support::SourceLocation loc)
	{
		if (_currentBlock && !_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &block });
		_currentBlock = &block;
	}

	// ---- emit helpers ---------------------------------------------------------------------------

	void IrBuilder::emitVoid(support::SourceLocation loc, IrInstrPayload payload)
	{
		// Dead code can still textually follow a break/continue/goto/return (`{ break; x = 1; }` -
		// sema does not reject it, see the header comment on IrBuilder's contract): rather than
		// append onto a block that already ended with its own terminator (see BasicBlock::
		// isTerminated()'s own header comment), lazily start a fresh, unreachable block right here,
		// the one place every instruction-emitting helper in this file ultimately goes through. No
		// reachability analysis prunes that block afterward (Fase 9's job, §13) - it simply never
		// gets jumped into, and the common case (nothing dead follows) never pays for it at all.
		if (_currentBlock->isTerminated())
			_currentBlock = &_currentFunction->createBlock();

		IrInstr* instr = _arena.create<IrInstr>(loc, std::move(payload));
		_currentBlock->append(instr);
	}

	void IrBuilder::emitConstInto(support::SourceLocation loc, IrValue result, i64 value)
	{
		IrConstPayload payload;
		payload.result = result;
		payload.intValue = value;
		emitVoid(loc, payload);
	}

	IrValue IrBuilder::emitConstInt(support::SourceLocation loc, i64 value)
	{
		IrValue result = _currentFunction->newTemp();
		emitConstInto(loc, result, value);
		return result;
	}

	IrValue IrBuilder::emitConstFloat(support::SourceLocation loc, f32 value)
	{
		IrConstPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.floatValue = value;
		payload.isFloat = true;
		emitVoid(loc, payload);
		return payload.result;
	}

	void IrBuilder::emitCopyInto(support::SourceLocation loc, IrValue result, IrValue source)
	{
		if (result == source)
			return;
		emitVoid(loc, IrCopyPayload{ result, source });
	}

	IrValue IrBuilder::emitLoad(support::SourceLocation loc, IrValue address, IrMemSize size)
	{
		IrLoadPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.size = size;
		payload.address = address;
		emitVoid(loc, payload);
		return payload.result;
	}

	void IrBuilder::emitStore(support::SourceLocation loc, IrValue address, IrMemSize size, IrValue value)
	{
		emitVoid(loc, IrStorePayload{ size, address, value });
	}

	IrValue IrBuilder::emitFrameAddr(support::SourceLocation loc, u32 localIndex)
	{
		IrFrameAddrPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.localIndex = localIndex;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitGlobalAddr(support::SourceLocation loc, std::string_view name)
	{
		IrGlobalAddrPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.name = name;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitBinOp(support::SourceLocation loc, IrBinOp op, IrValue lhs, IrValue rhs, bool isUnsigned)
	{
		IrBinOpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.op = op;
		payload.isUnsigned = isUnsigned;
		payload.lhs = lhs;
		payload.rhs = rhs;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitUnOp(support::SourceLocation loc, IrUnOp op, IrValue operand)
	{
		IrUnOpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.op = op;
		payload.operand = operand;
		emitVoid(loc, payload);
		return payload.result;
	}

	std::string_view IrBuilder::internLabel(std::string_view text)
	{
		void* storage = _arena.allocate(text.size(), alignof(char));
		std::memcpy(storage, text.data(), text.size());
		return std::string_view(static_cast<const char*>(storage), text.size());
	}

	IrMemSize IrBuilder::memSizeOf(const Type* type) noexcept
	{
		return irMemSizeForBytes(type ? type->sizeInBytes() : 4u);
	}

	void IrBuilder::requireScalarValue(const Type* type, support::SourceLocation loc)
	{
		if (type && type->isStruct())
			_diagnostics.error(loc, "using a struct by value here is not supported in this version - use a pointer instead");
	}

	// ---- constant folding (mirrors Sema::evalConstantExpr - see the header comment) -------------

	std::optional<i64> IrBuilder::foldConstant(Expr* expr)
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
			LocalSymbol* symbol = lookupSymbol(name->name());
			if (symbol && symbol->kind == LocalSymbolKind::EnumConstant)
				return symbol->enumValue;
			return std::nullopt;
		}

		if (auto* unary = dynamic_cast<ast::UnaryExpr*>(expr))
		{
			std::optional<i64> operand = foldConstant(unary->operand());
			if (!operand)
				return std::nullopt;
			switch (unary->op())
			{
				case UnaryOp::Negate: return -*operand;
				case UnaryOp::LogicalNot: return *operand == 0 ? i64(1) : i64(0);
				case UnaryOp::BitwiseNot: return ~*operand;
				default: return std::nullopt;
			}
		}

		if (auto* binary = dynamic_cast<ast::BinaryExpr*>(expr))
		{
			std::optional<i64> lhs = foldConstant(binary->lhs());
			std::optional<i64> rhs = foldConstant(binary->rhs());
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
			std::optional<i64> cond = foldConstant(ternary->cond());
			if (!cond)
				return std::nullopt;
			return *cond != 0 ? foldConstant(ternary->thenExpr()) : foldConstant(ternary->elseExpr());
		}

		if (auto* cast = dynamic_cast<ast::CastExpr*>(expr))
			return foldConstant(cast->operand());

		if (auto* sizeofExpr = dynamic_cast<ast::SizeofExpr*>(expr))
		{
			const Type* argType = sizeofExpr->isTypeArgument() ? sizeofExpr->argumentType()
				: (sizeofExpr->argumentExpr() ? sizeofExpr->argumentExpr()->type() : nullptr);
			return argType ? std::optional<i64>(static_cast<i64>(argType->sizeInBytes())) : std::nullopt;
		}

		return std::nullopt;
	}

	// ---- lvalue addressing --------------------------------------------------------------------------

	IrValue IrBuilder::lowerAddress(Expr* expr)
	{
		support::SourceLocation loc = expr->location();

		if (auto* name = dynamic_cast<ast::NameExpr*>(expr))
		{
			LocalSymbol* symbol = lookupSymbol(name->name());
			if (symbol && symbol->kind == LocalSymbolKind::Local)
				return emitFrameAddr(loc, symbol->localSlot);
			// Global (or an unresolved symbol - assumed not to happen on sema-checked input, see
			// the header comment on IrBuilder's contract).
			return emitGlobalAddr(loc, name->name());
		}

		if (auto* unary = dynamic_cast<ast::UnaryExpr*>(expr); unary && unary->op() == UnaryOp::Deref)
		{
			// *p's address is just p's own value - except when the operand is itself an array
			// (sema explicitly allows that, see visit(UnaryExpr&)'s own note): an array's address
			// IS its own storage, never a pointer value loaded from somewhere else.
			const Type* operandType = unary->operand()->type();
			return (operandType && operandType->isArray()) ? lowerAddress(unary->operand()) : lowerExpr(unary->operand());
		}

		if (auto* index = dynamic_cast<ast::IndexExpr*>(expr))
		{
			const Type* arrayType = index->array()->type();
			IrValue base = (arrayType && arrayType->isArray()) ? lowerAddress(index->array()) : lowerExpr(index->array());
			IrValue indexValue = lowerExpr(index->index());
			u32 elemSize = (arrayType && arrayType->arrayElementType()) ? arrayType->arrayElementType()->sizeInBytes() : 1u;
			IrValue offset = indexValue;
			if (elemSize != 1)
			{
				IrValue elemSizeValue = emitConstInt(loc, static_cast<i64>(elemSize));
				offset = emitBinOp(loc, IrBinOp::Mul, indexValue, elemSizeValue, true);
			}
			return emitBinOp(loc, IrBinOp::Add, base, offset, true);
		}

		if (auto* member = dynamic_cast<ast::MemberExpr*>(expr))
		{
			IrValue objectAddr = member->isArrow() ? lowerExpr(member->object()) : lowerAddress(member->object());
			const Type* objectType = member->object()->type();
			const Type* structType = member->isArrow()
				? (objectType ? objectType->arrayElementType() : nullptr)
				: objectType;
			ast::StructDecl* decl = structType ? structType->structDecl() : nullptr;
			if (!decl)
				return objectAddr; // shouldn't happen on sema-checked input - see the header comment

			u32 fieldIndex = 0;
			bool found = false;
			for (const FieldDecl& field : decl->fields())
			{
				if (field.name == member->memberName()) { found = true; break; }
				++fieldIndex;
			}
			if (!found)
				return objectAddr;

			u32 offset = sema::fieldOffset(*decl, fieldIndex);
			if (offset == 0)
				return objectAddr;
			IrValue offsetValue = emitConstInt(loc, static_cast<i64>(offset));
			return emitBinOp(loc, IrBinOp::Add, objectAddr, offsetValue, true);
		}

		// Every other Expr kind reaching here is not one of this subset's lvalue forms (see
		// sema::Sema::isLValue(), sema.cpp) - but unlike isLValue(), sema's own visit(MemberExpr&)
		// only requires a `.` base to have struct *type*, not to actually be an lvalue (sema.cpp),
		// so e.g. `make().y` for a struct-returning `make()` reaches here on perfectly valid,
		// sema-checked input: the "object" only ever exists in a temp, never in memory, so there is
		// no real address to compute. Report the same struct-by-value diagnostic requireScalarValue()
		// uses elsewhere, then fall back to the value itself so a caller still gets *something* to
		// build on - the same error-recovery style sema.h's own header comment describes.
		requireScalarValue(expr->type(), expr->location());
		return lowerExpr(expr);
	}

	// ---- condition lowering (jumping code, short-circuit && / ||) --------------------------------

	void IrBuilder::lowerCondition(Expr* cond, BasicBlock* trueBlock, BasicBlock* falseBlock)
	{
		if (auto* binary = dynamic_cast<ast::BinaryExpr*>(cond))
		{
			if (binary->op() == BinaryOp::LogicalAnd)
			{
				BasicBlock& midBlock = _currentFunction->createBlock();
				lowerCondition(binary->lhs(), &midBlock, falseBlock);
				_currentBlock = &midBlock;
				lowerCondition(binary->rhs(), trueBlock, falseBlock);
				return;
			}
			if (binary->op() == BinaryOp::LogicalOr)
			{
				BasicBlock& midBlock = _currentFunction->createBlock();
				lowerCondition(binary->lhs(), trueBlock, &midBlock);
				_currentBlock = &midBlock;
				lowerCondition(binary->rhs(), trueBlock, falseBlock);
				return;
			}
		}
		if (auto* unary = dynamic_cast<ast::UnaryExpr*>(cond); unary && unary->op() == UnaryOp::LogicalNot)
		{
			lowerCondition(unary->operand(), falseBlock, trueBlock); // De Morgan: swap targets
			return;
		}

		support::SourceLocation loc = cond->location();
		IrValue value = lowerExpr(cond);
		IrValue zero = emitConstInt(loc, 0);
		emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Ne, false, value, zero, trueBlock, falseBlock });
	}

	IrValue IrBuilder::materializeBoolean(Expr* cond)
	{
		support::SourceLocation loc = cond->location();
		BasicBlock& trueBlock = _currentFunction->createBlock();
		BasicBlock& falseBlock = _currentFunction->createBlock();
		BasicBlock& mergeBlock = _currentFunction->createBlock();

		lowerCondition(cond, &trueBlock, &falseBlock);

		IrValue result = _currentFunction->newTemp();

		_currentBlock = &trueBlock;
		emitConstInto(loc, result, 1);
		emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &falseBlock;
		emitConstInto(loc, result, 0);
		emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &mergeBlock;
		return result;
	}

	// ---- arithmetic (shared by BinaryExpr and AssignExpr's compound operators) -------------------

	IrValue IrBuilder::lowerArithmetic(support::SourceLocation loc, BinaryOp op, const Type* resultType,
		const Type* lhsType, const Type* rhsType, IrValue lhsVal, IrValue rhsVal)
	{
		bool lhsIsPointer = lhsType && lhsType->isPointer();
		bool rhsIsPointer = rhsType && rhsType->isPointer();

		if (lhsIsPointer && !rhsIsPointer && (op == BinaryOp::Add || op == BinaryOp::Sub))
		{
			u32 step = lhsType->arrayElementType() ? lhsType->arrayElementType()->sizeInBytes() : 1u;
			IrValue scaledRhs = rhsVal;
			if (step != 1)
			{
				IrValue stepValue = emitConstInt(loc, static_cast<i64>(step));
				scaledRhs = emitBinOp(loc, IrBinOp::Mul, rhsVal, stepValue, true);
			}
			return emitBinOp(loc, op == BinaryOp::Add ? IrBinOp::Add : IrBinOp::Sub, lhsVal, scaledRhs, true);
		}
		if (rhsIsPointer && !lhsIsPointer && op == BinaryOp::Add)
		{
			u32 step = rhsType->arrayElementType() ? rhsType->arrayElementType()->sizeInBytes() : 1u;
			IrValue scaledLhs = lhsVal;
			if (step != 1)
			{
				IrValue stepValue = emitConstInt(loc, static_cast<i64>(step));
				scaledLhs = emitBinOp(loc, IrBinOp::Mul, lhsVal, stepValue, true);
			}
			return emitBinOp(loc, IrBinOp::Add, scaledLhs, rhsVal, true);
		}
		if (lhsIsPointer && rhsIsPointer && op == BinaryOp::Sub)
		{
			IrValue diff = emitBinOp(loc, IrBinOp::Sub, lhsVal, rhsVal, true);
			u32 step = lhsType->arrayElementType() ? lhsType->arrayElementType()->sizeInBytes() : 1u;
			if (step == 1)
				return diff;
			IrValue stepValue = emitConstInt(loc, static_cast<i64>(step));
			return emitBinOp(loc, IrBinOp::Div, diff, stepValue, false); // ptrdiff is signed `int` - see sema.cpp
		}

		// Plain arithmetic: both operands already share one common, promoted type by the time sema
		// resolved this node (commonArithmeticType(), sema.cpp) - resultType is that type, so its
		// own signedness is the right one to read, not either raw operand's (e.g. `unsigned char +
		// int` promotes the unsigned char to a signed int before the add - see integerPromote()).
		bool isUnsigned = resultType && !resultType->isSigned();
		IrBinOp irOp;
		switch (op)
		{
			case BinaryOp::Add: irOp = IrBinOp::Add; break;
			case BinaryOp::Sub: irOp = IrBinOp::Sub; break;
			case BinaryOp::Mul: irOp = IrBinOp::Mul; break;
			case BinaryOp::Div: irOp = IrBinOp::Div; break;
			case BinaryOp::Mod: irOp = IrBinOp::Mod; break;
			case BinaryOp::BitAnd: irOp = IrBinOp::And; break;
			case BinaryOp::BitOr: irOp = IrBinOp::Or; break;
			case BinaryOp::BitXor: irOp = IrBinOp::Xor; break;
			case BinaryOp::Shl: irOp = IrBinOp::Shl; break;
			case BinaryOp::Shr: irOp = isUnsigned ? IrBinOp::Shr : IrBinOp::Sar; break;
			default: irOp = IrBinOp::Add; break; // Eq/Ne/Lt/Le/Gt/Ge/LogicalAnd/LogicalOr never reach here
		}
		return emitBinOp(loc, irOp, lhsVal, rhsVal, isUnsigned);
	}

	// ---- expressions --------------------------------------------------------------------------------

	void IrBuilder::visit(ast::IntLiteralExpr& node)
	{
		_lastValue = emitConstInt(node.location(), static_cast<i64>(node.value()));
	}

	void IrBuilder::visit(ast::FloatLiteralExpr& node)
	{
		_lastValue = emitConstFloat(node.location(), static_cast<f32>(node.value()));
	}

	void IrBuilder::visit(ast::CharLiteralExpr& node)
	{
		_lastValue = emitConstInt(node.location(), static_cast<i64>(node.value()));
	}

	void IrBuilder::visit(ast::BoolLiteralExpr& node)
	{
		_lastValue = emitConstInt(node.location(), node.value() ? 1 : 0);
	}

	void IrBuilder::visit(ast::StringLiteralExpr& node)
	{
		std::string label = std::format(".str{}", _nextStringLiteralId++);
		std::string_view name = internLabel(label);
		_module.addStringLiteral(name, node.value());
		_lastValue = emitGlobalAddr(node.location(), name);
	}

	void IrBuilder::visit(ast::NameExpr& node)
	{
		LocalSymbol* symbol = lookupSymbol(node.name());
		if (symbol && symbol->kind == LocalSymbolKind::EnumConstant)
		{
			_lastValue = emitConstInt(node.location(), symbol->enumValue);
			return;
		}
		// A variable (local, parameter or global) - a function name or an undeclared identifier
		// would already have been rejected by sema, see the header comment on IrBuilder's contract.
		requireScalarValue(node.type(), node.location());
		IrValue addr = lowerAddress(&node);
		_lastValue = emitLoad(node.location(), addr, memSizeOf(node.type()));
	}

	void IrBuilder::visit(ast::CallExpr& node)
	{
		std::vector<IrValue> argValues;
		argValues.reserve(node.args().size());
		for (Expr* arg : node.args())
			argValues.push_back(lowerExpr(arg));

		for (IrValue value : argValues)
			emitVoid(node.location(), IrParamPayload{ value });

		auto* callee = dynamic_cast<ast::NameExpr*>(node.callee());
		std::string_view calleeName = callee ? callee->name() : std::string_view{}; // sema guarantees this - see the header comment

		bool hasResult = node.type() && !node.type()->isVoid();
		if (hasResult)
			requireScalarValue(node.type(), node.location()); // a struct-returning call (§10/§14's hidden-pointer ABI is Fase 7's job) is diagnosed here too, not just NameExpr/MemberExpr/`*p`

		IrCallPayload payload;
		payload.hasResult = hasResult;
		payload.callee = calleeName;
		payload.argCount = static_cast<u32>(argValues.size());
		if (hasResult)
			payload.result = _currentFunction->newTemp();

		emitVoid(node.location(), payload);
		_lastValue = hasResult ? payload.result : IrValue{};
	}

	void IrBuilder::visit(ast::UnaryExpr& node)
	{
		support::SourceLocation loc = node.location();
		switch (node.op())
		{
			case UnaryOp::AddressOf:
				_lastValue = lowerAddress(node.operand());
				return;

			case UnaryOp::Deref:
			{
				requireScalarValue(node.type(), loc);
				// sema explicitly allows an array operand here too (sema.cpp's own UnaryOp::Deref
				// case checks `operandType->isPointer() || operandType->isArray()`), matching real
				// C's `*arr == arr[0]`: an array's *address* is what `*` dereferences, never a
				// value loaded through it (an array never sits behind a pointer stored in memory -
				// its own frame/global slot already holds the elements directly). Getting this
				// wrong is exactly the same mistake a raw `lowerExpr()` on a pointer-typed operand
				// correctly avoids: that one loads the pointer's own stored value first, this one
				// must not.
				const Type* operandType = node.operand()->type();
				IrValue ptr = (operandType && operandType->isArray()) ? lowerAddress(node.operand()) : lowerExpr(node.operand());
				_lastValue = emitLoad(loc, ptr, memSizeOf(node.type()));
				return;
			}

			case UnaryOp::Negate:
				_lastValue = emitUnOp(loc, IrUnOp::Neg, lowerExpr(node.operand()));
				return;

			case UnaryOp::BitwiseNot:
				_lastValue = emitUnOp(loc, IrUnOp::Not, lowerExpr(node.operand()));
				return;

			case UnaryOp::LogicalNot:
				_lastValue = materializeBoolean(&node);
				return;

			case UnaryOp::PreIncrement:
			case UnaryOp::PreDecrement:
			case UnaryOp::PostIncrement:
			case UnaryOp::PostDecrement:
			{
				const Type* type = node.operand()->type();
				IrValue addr = lowerAddress(node.operand());
				IrMemSize size = memSizeOf(type);
				IrValue oldValue = emitLoad(loc, addr, size);
				i64 step = (type && type->isPointer() && type->arrayElementType()) ? type->arrayElementType()->sizeInBytes() : 1;
				IrValue stepValue = emitConstInt(loc, step);
				bool isIncrement = (node.op() == UnaryOp::PreIncrement || node.op() == UnaryOp::PostIncrement);
				IrValue newValue = emitBinOp(loc, isIncrement ? IrBinOp::Add : IrBinOp::Sub, oldValue, stepValue, type && !type->isSigned());
				emitStore(loc, addr, size, newValue);
				bool isPre = (node.op() == UnaryOp::PreIncrement || node.op() == UnaryOp::PreDecrement);
				_lastValue = isPre ? newValue : oldValue;
				return;
			}
		}
	}

	void IrBuilder::visit(ast::BinaryExpr& node)
	{
		if (node.op() == BinaryOp::LogicalAnd || node.op() == BinaryOp::LogicalOr)
		{
			_lastValue = materializeBoolean(&node);
			return;
		}

		IrValue lhs = lowerExpr(node.lhs());
		IrValue rhs = lowerExpr(node.rhs());
		support::SourceLocation loc = node.location();

		switch (node.op())
		{
			case BinaryOp::Eq: case BinaryOp::Ne:
			case BinaryOp::Lt: case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
			{
				IrCmpPayload payload;
				payload.result = _currentFunction->newTemp();
				payload.predicate = cmpPredicateFor(node.op());
				payload.isUnsigned = isUnsignedComparison(node.lhs()->type(), node.rhs()->type());
				payload.lhs = lhs;
				payload.rhs = rhs;
				emitVoid(loc, payload);
				_lastValue = payload.result;
				return;
			}
			default:
				_lastValue = lowerArithmetic(loc, node.op(), node.type(), node.lhs()->type(), node.rhs()->type(), lhs, rhs);
				return;
		}
	}

	void IrBuilder::visit(ast::AssignExpr& node)
	{
		support::SourceLocation loc = node.location();
		IrValue addr = lowerAddress(node.target());
		const Type* targetType = node.target()->type();
		IrMemSize size = memSizeOf(targetType);

		IrValue value;
		if (node.op() == AssignOp::Assign)
		{
			value = lowerExpr(node.value());
		}
		else
		{
			IrValue oldValue = emitLoad(loc, addr, size);
			IrValue rhs = lowerExpr(node.value());
			BinaryOp binaryOp = binaryOpForCompoundAssign(node.op());
			// `x op= y` means `x = x op y` by definition, so this must agree with what the
			// equivalent BinaryExpr would compute (its own node.type() is already sema's promoted
			// result type) - not the raw, unpromoted target type, which for a narrow unsigned target
			// (e.g. `unsigned char x; x %= someInt;`) would pick the wrong signedness for the
			// operation (and, for `>>=`, the wrong Shr/Sar) versus `x = x % someInt;`. Harmless to
			// compute even when targetType is a pointer (`p += i`): lowerArithmetic()'s pointer
			// branches never read this parameter, only the plain-arithmetic fallback does.
			const Type* promotedType = commonArithmeticType(targetType, node.value()->type());
			value = lowerArithmetic(loc, binaryOp, promotedType, targetType, node.value()->type(), oldValue, rhs);
		}

		emitStore(loc, addr, size, value);
		_lastValue = value;
	}

	void IrBuilder::visit(ast::IndexExpr& node)
	{
		requireScalarValue(node.type(), node.location());
		IrValue addr = lowerAddress(&node);
		_lastValue = emitLoad(node.location(), addr, memSizeOf(node.type()));
	}

	void IrBuilder::visit(ast::MemberExpr& node)
	{
		requireScalarValue(node.type(), node.location());
		IrValue addr = lowerAddress(&node);
		_lastValue = emitLoad(node.location(), addr, memSizeOf(node.type()));
	}

	void IrBuilder::visit(ast::CastExpr& node)
	{
		// No IR-level conversion opcode exists (§9's opcode table has none): the cast is realized
		// only through the memSize a later Load/Store picks based on the AST's own annotated type.
		// Real float<->int conversion instructions are libs/codegen's job (Fase 6+), not this
		// phase's - see the header comment on IrBuilder's contract.
		_lastValue = lowerExpr(node.operand());
	}

	void IrBuilder::visit(ast::SizeofExpr& node)
	{
		// sizeof's operand is a non-evaluated context in real C (sema.cpp's own note): only its
		// already-annotated type is read here, argumentExpr() is never lowered, so e.g.
		// `sizeof(x++)` never actually increments x.
		const Type* argType = node.isTypeArgument() ? node.argumentType()
			: (node.argumentExpr() ? node.argumentExpr()->type() : nullptr);
		_lastValue = emitConstInt(node.location(), argType ? static_cast<i64>(argType->sizeInBytes()) : 0);
	}

	void IrBuilder::visit(ast::TernaryExpr& node)
	{
		support::SourceLocation loc = node.location();
		BasicBlock& thenBlock = _currentFunction->createBlock();
		BasicBlock& elseBlock = _currentFunction->createBlock();
		BasicBlock& mergeBlock = _currentFunction->createBlock();

		lowerCondition(node.cond(), &thenBlock, &elseBlock);

		IrValue result = _currentFunction->newTemp();

		_currentBlock = &thenBlock;
		emitCopyInto(loc, result, lowerExpr(node.thenExpr()));
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &elseBlock;
		emitCopyInto(loc, result, lowerExpr(node.elseExpr()));
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &mergeBlock;
		_lastValue = result;
	}

	// ---- statements -----------------------------------------------------------------------------

	void IrBuilder::visit(ast::EmptyStmt&) {}

	void IrBuilder::visit(ast::ExprStmt& node)
	{
		lowerExpr(node.expr());
	}

	void IrBuilder::visit(ast::DeclStmt& node)
	{
		if (node.decl())
			node.decl()->accept(*this);
	}

	void IrBuilder::visit(ast::CompoundStmt& node)
	{
		pushScope();
		for (Stmt* stmt : node.stmts())
			lowerStmt(stmt);
		popScope();
	}

	void IrBuilder::visit(ast::IfStmt& node)
	{
		support::SourceLocation loc = node.location();
		BasicBlock& thenBlock = _currentFunction->createBlock();
		BasicBlock* elseBlockPtr = node.elseStmt() ? &_currentFunction->createBlock() : nullptr;
		BasicBlock& mergeBlock = _currentFunction->createBlock();
		BasicBlock& elseBlock = elseBlockPtr ? *elseBlockPtr : mergeBlock;

		lowerCondition(node.cond(), &thenBlock, &elseBlock);

		_currentBlock = &thenBlock;
		lowerStmt(node.thenStmt());
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &mergeBlock });

		if (elseBlockPtr)
		{
			_currentBlock = elseBlockPtr;
			lowerStmt(node.elseStmt());
			if (!_currentBlock->isTerminated())
				emitVoid(loc, IrJumpPayload{ &mergeBlock });
		}

		_currentBlock = &mergeBlock;
	}

	void IrBuilder::visit(ast::WhileStmt& node)
	{
		support::SourceLocation loc = node.location();
		BasicBlock& headerBlock = _currentFunction->createBlock();
		BasicBlock& bodyBlock = _currentFunction->createBlock();
		BasicBlock& exitBlock = _currentFunction->createBlock();

		switchToBlock(headerBlock, loc);
		lowerCondition(node.cond(), &bodyBlock, &exitBlock);

		_currentBlock = &bodyBlock;
		_breakTargets.push_back(&exitBlock);
		_continueTargets.push_back(&headerBlock);
		lowerStmt(node.body());
		_breakTargets.pop_back();
		_continueTargets.pop_back();
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &headerBlock });

		_currentBlock = &exitBlock;
	}

	void IrBuilder::visit(ast::DoWhileStmt& node)
	{
		support::SourceLocation loc = node.location();
		BasicBlock& bodyBlock = _currentFunction->createBlock();
		BasicBlock& condBlock = _currentFunction->createBlock();
		BasicBlock& exitBlock = _currentFunction->createBlock();

		switchToBlock(bodyBlock, loc);
		_breakTargets.push_back(&exitBlock);
		_continueTargets.push_back(&condBlock);
		lowerStmt(node.body());
		_breakTargets.pop_back();
		_continueTargets.pop_back();
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &condBlock });

		_currentBlock = &condBlock;
		lowerCondition(node.cond(), &bodyBlock, &exitBlock);

		_currentBlock = &exitBlock;
	}

	void IrBuilder::visit(ast::ForStmt& node)
	{
		support::SourceLocation loc = node.location();
		pushScope(); // `for (int i = 0; ...)` scopes `i` to the loop - matches sema.cpp
		lowerStmt(node.init());

		BasicBlock& headerBlock = _currentFunction->createBlock();
		BasicBlock& bodyBlock = _currentFunction->createBlock();
		BasicBlock& incBlock = _currentFunction->createBlock();
		BasicBlock& exitBlock = _currentFunction->createBlock();

		switchToBlock(headerBlock, loc);
		if (node.cond())
			lowerCondition(node.cond(), &bodyBlock, &exitBlock);
		else
			emitVoid(loc, IrJumpPayload{ &bodyBlock }); // an absent condition means "always true" - see stmt.h

		_currentBlock = &bodyBlock;
		_breakTargets.push_back(&exitBlock);
		_continueTargets.push_back(&incBlock); // continue re-runs the increment, not the condition directly - real C's rule
		lowerStmt(node.body());
		_breakTargets.pop_back();
		_continueTargets.pop_back();
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &incBlock });

		_currentBlock = &incBlock;
		if (node.increment())
			lowerExpr(node.increment());
		emitVoid(loc, IrJumpPayload{ &headerBlock });

		_currentBlock = &exitBlock;
		popScope();
	}

	void IrBuilder::visit(ast::ReturnStmt& node)
	{
		if (node.value())
			emitVoid(node.location(), IrReturnPayload{ true, lowerExpr(node.value()) });
		else
			emitVoid(node.location(), IrReturnPayload{ false, IrValue{} });
	}

	void IrBuilder::visit(ast::BreakStmt& node)
	{
		if (!_breakTargets.empty())
			emitVoid(node.location(), IrJumpPayload{ _breakTargets.back() });
	}

	void IrBuilder::visit(ast::ContinueStmt& node)
	{
		if (!_continueTargets.empty())
			emitVoid(node.location(), IrJumpPayload{ _continueTargets.back() });
	}

	void IrBuilder::visit(ast::SwitchStmt& node)
	{
		support::SourceLocation loc = node.location();
		IrValue condValue = lowerExpr(node.cond());

		std::vector<std::pair<i64, BasicBlock*>> cases;
		BasicBlock* defaultBlock = nullptr;
		collectSwitchCases(node.body(), cases, defaultBlock);

		BasicBlock& exitBlock = _currentFunction->createBlock();

		// Dispatch chain: one Cmp+CondJump per case value, in source order, falling through to the
		// next comparison on a mismatch - a jump table is deliberately not built here, same §0
		// criterion of not adding a mechanism before it is actually needed.
		for (const auto& [value, block] : cases)
		{
			IrValue constValue = emitConstInt(loc, value);
			BasicBlock& nextTest = _currentFunction->createBlock();
			emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Eq, false, condValue, constValue, block, &nextTest });
			_currentBlock = &nextTest;
		}
		emitVoid(loc, IrJumpPayload{ defaultBlock ? defaultBlock : &exitBlock });

		_breakTargets.push_back(&exitBlock);
		// Deliberately not pushed onto _continueTargets: `continue` inside a switch still targets
		// the nearest enclosing loop, not this switch - real C's rule, matching sema's own separate
		// _loopDepth/_switchStack tracking (sema.h).
		lowerStmt(node.body());
		_breakTargets.pop_back();

		switchToBlock(exitBlock, loc);
	}

	void IrBuilder::visit(ast::CaseStmt& node)
	{
		auto it = _caseBlocks.find(&node);
		if (it != _caseBlocks.end())
			switchToBlock(*it->second, node.location());
		lowerStmt(node.body());
	}

	void IrBuilder::visit(ast::DefaultStmt& node)
	{
		auto it = _caseBlocks.find(&node);
		if (it != _caseBlocks.end())
			switchToBlock(*it->second, node.location());
		lowerStmt(node.body());
	}

	void IrBuilder::visit(ast::GotoStmt& node)
	{
		auto it = _labelBlocks.find(node.label());
		if (it != _labelBlocks.end())
			emitVoid(node.location(), IrJumpPayload{ it->second });
	}

	void IrBuilder::visit(ast::LabelStmt& node)
	{
		auto it = _labelBlocks.find(node.label());
		if (it != _labelBlocks.end())
			switchToBlock(*it->second, node.location());
		lowerStmt(node.body());
	}

	// ---- label / switch-case discovery (mirrors sema.cpp's collectLabels shape) -------------------

	void IrBuilder::collectLabelBlocks(Stmt* stmt)
	{
		if (!stmt)
			return;
		if (auto* label = dynamic_cast<ast::LabelStmt*>(stmt))
		{
			_labelBlocks[label->label()] = &_currentFunction->createBlock();
			collectLabelBlocks(label->body());
			return;
		}
		if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt))
		{
			for (Stmt* child : compound->stmts())
				collectLabelBlocks(child);
			return;
		}
		if (auto* ifStmt = dynamic_cast<ast::IfStmt*>(stmt)) { collectLabelBlocks(ifStmt->thenStmt()); collectLabelBlocks(ifStmt->elseStmt()); return; }
		if (auto* whileStmt = dynamic_cast<ast::WhileStmt*>(stmt)) { collectLabelBlocks(whileStmt->body()); return; }
		if (auto* doWhileStmt = dynamic_cast<ast::DoWhileStmt*>(stmt)) { collectLabelBlocks(doWhileStmt->body()); return; }
		if (auto* forStmt = dynamic_cast<ast::ForStmt*>(stmt)) { collectLabelBlocks(forStmt->body()); return; }
		if (auto* switchStmt = dynamic_cast<ast::SwitchStmt*>(stmt)) { collectLabelBlocks(switchStmt->body()); return; }
		if (auto* caseStmt = dynamic_cast<ast::CaseStmt*>(stmt)) { collectLabelBlocks(caseStmt->body()); return; }
		if (auto* defaultStmt = dynamic_cast<ast::DefaultStmt*>(stmt)) { collectLabelBlocks(defaultStmt->body()); return; }
	}

	void IrBuilder::collectSwitchCases(Stmt* stmt, std::vector<std::pair<i64, BasicBlock*>>& cases, BasicBlock*& defaultBlock)
	{
		if (!stmt)
			return;
		if (auto* caseStmt = dynamic_cast<ast::CaseStmt*>(stmt))
		{
			BasicBlock& block = _currentFunction->createBlock();
			_caseBlocks[caseStmt] = &block;
			std::optional<i64> value = foldConstant(caseStmt->value());
			// sema already guaranteed this folds (see the header comment) - std::nullopt is only
			// reachable on input sema itself would have rejected. Skipping the dispatch entry
			// rather than defaulting to 0 matters even then: 0 is a perfectly legal case value of
			// its own, so substituting it here could alias with (and shadow) a real `case 0:` in
			// the same switch instead of just leaving this one case unreachable.
			if (value)
				cases.emplace_back(*value, &block);
			collectSwitchCases(caseStmt->body(), cases, defaultBlock);
			return;
		}
		if (auto* defaultStmt = dynamic_cast<ast::DefaultStmt*>(stmt))
		{
			BasicBlock& block = _currentFunction->createBlock();
			_caseBlocks[defaultStmt] = &block;
			defaultBlock = &block;
			collectSwitchCases(defaultStmt->body(), cases, defaultBlock);
			return;
		}
		if (dynamic_cast<ast::SwitchStmt*>(stmt))
			return; // a nested switch's own cases belong to IT, not us - see sema.h's _switchStack
		if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt))
		{
			for (Stmt* child : compound->stmts())
				collectSwitchCases(child, cases, defaultBlock);
			return;
		}
		if (auto* ifStmt = dynamic_cast<ast::IfStmt*>(stmt)) { collectSwitchCases(ifStmt->thenStmt(), cases, defaultBlock); collectSwitchCases(ifStmt->elseStmt(), cases, defaultBlock); return; }
		if (auto* whileStmt = dynamic_cast<ast::WhileStmt*>(stmt)) { collectSwitchCases(whileStmt->body(), cases, defaultBlock); return; }
		if (auto* doWhileStmt = dynamic_cast<ast::DoWhileStmt*>(stmt)) { collectSwitchCases(doWhileStmt->body(), cases, defaultBlock); return; }
		if (auto* forStmt = dynamic_cast<ast::ForStmt*>(stmt)) { collectSwitchCases(forStmt->body(), cases, defaultBlock); return; }
		if (auto* labelStmt = dynamic_cast<ast::LabelStmt*>(stmt)) { collectSwitchCases(labelStmt->body(), cases, defaultBlock); return; }
	}

	// ---- declarations -----------------------------------------------------------------------------

	void IrBuilder::visit(ast::VarDecl& node)
	{
		if (_currentFunction == nullptr)
		{
			// File scope - nothing to lower here: codegen (Fase 6/7) reads the AST's VarDecl
			// directly to emit .data/.bss, since no IR instruction models a global's static initial
			// value (§9's opcode table has none) - see IrModule's own header comment.
			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::Global;
			declareSymbol(node.name(), symbol);
			return;
		}

		LocalSymbol symbol;
		symbol.kind = LocalSymbolKind::Local;
		symbol.localSlot = _currentFunction->newLocalSlot();
		declareSymbol(node.name(), symbol);

		if (node.initializer())
		{
			IrValue value = lowerExpr(node.initializer());
			IrValue addr = emitFrameAddr(node.location(), symbol.localSlot);
			emitStore(node.location(), addr, memSizeOf(node.type()), value);
		}
	}

	void IrBuilder::visit(ast::FunctionDecl& node)
	{
		if (!node.isDefinition())
			return; // a prototype has nothing to lower - see the header comment

		IrFunction& function = _module.addFunction(node.name(), node.returnType());
		_currentFunction = &function;
		function.reserveParamSlots(static_cast<u32>(node.params().size()));

		pushScope();
		u32 slot = 0;
		for (const Param& param : node.params())
		{
			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::Local;
			symbol.localSlot = slot++;
			_scopes.back()[param.name] = symbol;
		}

		BasicBlock& entry = function.createBlock();
		_currentBlock = &entry;

		_labelBlocks.clear();
		collectLabelBlocks(node.body());

		for (Stmt* stmt : node.body()->stmts())
			lowerStmt(stmt);

		// A function whose body falls off the end without an explicit return (only valid for
		// `void` - sema's own ReturnStmt check applies per-statement, not as a whole-function
		// reachability analysis this subset does not attempt, see sema.cpp) - close it off with a
		// bare `ret` so every block really is terminated (see BasicBlock::isTerminated()'s own
		// header comment).
		if (!_currentBlock->isTerminated())
			emitVoid(node.location(), IrReturnPayload{ false, IrValue{} });

		popScope();
		_currentFunction = nullptr;
		_currentBlock = nullptr;
	}

	void IrBuilder::visit(ast::StructDecl&)
	{
		// Nothing to lower - a struct's layout is already fully resolved by sema (type_layout.h),
		// and this subset has no IR concept for a struct-typed value (see lowerAddress()'s own note
		// on MemberExpr - member access always goes through an address, never a whole-struct Load).
	}

	void IrBuilder::visit(ast::EnumDecl& node)
	{
		if (!node.isComplete())
			return;

		i64 nextValue = 0;
		for (const EnumeratorDecl& enumerator : node.enumerators())
		{
			if (enumerator.value)
			{
				std::optional<i64> evaluated = foldConstant(enumerator.value);
				if (evaluated)
					nextValue = *evaluated; // sema already validated this folds - see the header comment
			}

			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::EnumConstant;
			symbol.enumValue = nextValue;
			declareSymbol(enumerator.name, symbol);

			++nextValue;
		}
	}

	void IrBuilder::visit(ast::TypedefDecl&)
	{
		// Nothing to lower - see sema.cpp's own identical no-op and decl.h's note on why a typedef
		// introduces no new Type.
	}

	void IrBuilder::visit(ast::TranslationUnit& node)
	{
		for (ast::Decl* decl : node.decls())
			if (decl)
				decl->accept(*this);
	}
}
