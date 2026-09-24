#include <ceresc/ir/ir_builder.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/decl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_set>

namespace ceresc::ir
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

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
			// A float operand always reads through the unsigned branch family after FCMP, regardless
			// of Type::isSigned() - a hardware quirk of the ISA, not a signedness question: FCMP
			// clears Overflow and puts `fs < ft` directly in Carry (05-Instruction-Set.md), which is
			// exactly what the unsigned comparison jumps read (§10).
			if ((lhs && (lhs->isPointer() || lhs->isArray() || lhs->isFloat())) ||
				(rhs && (rhs->isPointer() || rhs->isArray() || rhs->isFloat())))
				return true;
			if (lhs && !lhs->isSigned())
				return true;
			if (rhs && !rhs->isSigned())
				return true;
			return false;
		}

		// True when two expressions are the same side-effect-free computation: the same name, the
		// same literal, a negation/bitwise-not/logical-not of equivalent operands, or a pure binary
		// operation of equivalent ones. Used only to recognize a select idiom (`a < b ? a : b`),
		// where a false positive would silently change which value is selected - so it is
		// deliberately conservative: no calls, no assignments, no dereference/address-of, no
		// increments. Sema never inserts cast nodes for the usual arithmetic conversions (it records
		// the promotion on the enclosing node instead), so the operands here are the source ones.
		bool exprsEquivalent(const Expr* a, const Expr* b) noexcept
		{
			if (a == b)
				return true;
			if (!a || !b)
				return false;

			if (const auto* x = dynamic_cast<const ast::IntLiteralExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::IntLiteralExpr*>(b);
				return y && x->value() == y->value() && x->isUnsigned() == y->isUnsigned();
			}
			if (const auto* x = dynamic_cast<const ast::CharLiteralExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::CharLiteralExpr*>(b);
				return y && x->value() == y->value() && x->encoding() == y->encoding();
			}
			if (const auto* x = dynamic_cast<const ast::BoolLiteralExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::BoolLiteralExpr*>(b);
				return y && x->value() == y->value();
			}
			if (const auto* x = dynamic_cast<const ast::FloatLiteralExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::FloatLiteralExpr*>(b);
				return y && x->value() == y->value();
			}
			if (const auto* x = dynamic_cast<const ast::NameExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::NameExpr*>(b);
				// A volatile read is observable, and the ternary diamond reads the selected arm a
				// SECOND time (once in the condition, once in the chosen arm) while the one-instruction
				// builtin reads each operand once - so folding here would drop an access that
				// ir_optimizer.h promises is never forwarded or dropped. A NameExpr is the only lvalue
				// this comparator matches, so refusing it here covers every idiom.
				if ((x->type() && x->type()->isVolatile()) || (y && y->type() && y->type()->isVolatile()))
					return false;
				return y && x->name() == y->name();
			}
			if (const auto* x = dynamic_cast<const ast::UnaryExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::UnaryExpr*>(b);
				if (!y || x->op() != y->op())
					return false;
				if (x->op() != UnaryOp::Negate && x->op() != UnaryOp::BitwiseNot && x->op() != UnaryOp::LogicalNot)
					return false;
				return exprsEquivalent(x->operand(), y->operand());
			}
			if (const auto* x = dynamic_cast<const ast::BinaryExpr*>(a))
			{
				const auto* y = dynamic_cast<const ast::BinaryExpr*>(b);
				if (!y || x->op() != y->op())
					return false;
				switch (x->op())
				{
					case BinaryOp::BitOr: case BinaryOp::BitXor: case BinaryOp::BitAnd:
					case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt: case BinaryOp::Le:
					case BinaryOp::Gt: case BinaryOp::Ge: case BinaryOp::Shl: case BinaryOp::Shr:
					case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
					case BinaryOp::Div: case BinaryOp::Mod:
						break;
					default:
						return false; // &&, || and the comma are control flow or sequencing
				}
				return exprsEquivalent(x->lhs(), y->lhs()) && exprsEquivalent(x->rhs(), y->rhs());
			}
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
				case TypeKind::ULongLong: return 62;
				case TypeKind::LongLong: return 61;
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

	IrBuilder::IrBuilder(support::Arena& arena, support::DiagnosticEngine& diagnostics,
		const support::OptimizationOptions& options) noexcept :
		_arena(arena), _diagnostics(diagnostics), _options(options)
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
		_reportedWideInteger = false;
		_scopes.clear();
		_scopeSlots.clear();
		_freeLocalSlots.clear();
		_globalSymbols.clear();
		_breakTargets.clear();
		_continueTargets.clear();
		_labelBlocks.clear();
		_caseBlocks.clear();
		_functionDecls.clear();
		_staticLocalNames.clear();
		_nextStringLiteralId = 0;

		// Collected before lowering anything, not as each definition is reached: a call may appear
		// textually before the function it names (C only requires a declaration to be visible, and
		// sema has already checked that one is), so a map filled on the way past would be missing
		// exactly the entries a forward call needs. Declarations count too - a prototype is enough
		// to know the parameter types, which is all this is for (see convertArgument()).
		for (ast::Decl* decl : unit.decls())
		{
			if (auto* function = dynamic_cast<ast::FunctionDecl*>(decl))
			{
				auto [it, inserted] = _functionDecls.try_emplace(function->name(), function);
				// A definition wins over a prototype for the same name: both carry the parameter
				// types, but keeping the definition means one less thing that can disagree.
				if (!inserted && function->isDefinition())
					it->second = function;
			}
		}

		unit.accept(*this);
		markFunctionsNamedByData(unit);
		return std::move(_module);
	}

	namespace
	{
		// The names an initializer takes the address of: `f`, `&f`, `(void*)f`, inside braces or not. Only
		// what codegen can write into a static image matters here (see addressConstantSymbol()); whatever
		// else the expression holds is not this pass's business.
		void collectAddressNames(const ast::Expr* expr, std::unordered_set<std::string_view>& names)
		{
			if (!expr)
				return;
			if (const auto* list = dynamic_cast<const ast::InitListExpr*>(expr))
			{
				for (const ast::Expr* element : list->elements())
					collectAddressNames(element, names);
			}
			else if (const auto* cast = dynamic_cast<const ast::CastExpr*>(expr))
				collectAddressNames(cast->operand(), names);
			else if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(expr))
			{
				if (unary->op() == ast::UnaryOp::AddressOf)
					collectAddressNames(unary->operand(), names);
			}
			else if (const auto* name = dynamic_cast<const ast::NameExpr*>(expr))
				names.insert(name->name());
		}
	}

	// A function whose address sits in the initializer of a global or of a `static` local is reached by a
	// pointer the data image holds, not by any instruction: unused-function elimination has to be told.
	void IrBuilder::markFunctionsNamedByData(ast::TranslationUnit& unit)
	{
		std::unordered_set<std::string_view> names;
		for (ast::Decl* decl : unit.decls())
			if (const auto* variable = dynamic_cast<const ast::VarDecl*>(decl))
				collectAddressNames(variable->initializer(), names);
		for (const IrStaticLocal& local : _module.staticLocals())
			if (local.decl)
				collectAddressNames(local.decl->initializer(), names);
		if (names.empty())
			return;
		for (const auto& function : _module.functions())
			if (names.contains(function->name()))
				function->setAddressTakenByData(true);
	}

	// ---- scope / symbol helpers -----------------------------------------------------------------

	void IrBuilder::pushScope()
	{
		_scopes.emplace_back();
		_scopeSlots.emplace_back();
	}

	void IrBuilder::popScope()
	{
		if (!_scopes.empty())
			_scopes.pop_back();
		if (!_scopeSlots.empty())
		{
			// Everything this scope declared is out of reach by name from here on, so its slots go
			// back into the pool for the next sibling scope to claim - see _freeLocalSlots.
			for (u32 slot : _scopeSlots.back())
				_freeLocalSlots.push_back(slot);
			_scopeSlots.pop_back();
		}
	}

	u32 IrBuilder::newLocalSlotFor(u32 sizeInBytes, bool isFloat, bool isVolatile, bool preferRegister, bool isRestrict)
	{
		u32 slot;
		if (_options.localSlotReuse && !_freeLocalSlots.empty())
		{
			slot = _freeLocalSlots.back();
			_freeLocalSlots.pop_back();
			// The slot keeps whichever size is larger: reusing a `char`'s slot for an `int` has to
			// grow it, reusing an `int`'s for a `char` must not shrink it (IrFunction::widenLocalSlot).
			_currentFunction->widenLocalSlot(slot, sizeInBytes, isFloat);
			_currentFunction->setLocalSlotRestrict(slot, isRestrict);
		}
		else
		{
			slot = _currentFunction->newLocalSlot(sizeInBytes, isFloat, isVolatile, preferRegister, isRestrict);
		}

		if (!_scopeSlots.empty())
			_scopeSlots.back().push_back(slot);
		return slot;
	}

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

	void IrBuilder::rejectWideFeature(support::SourceLocation loc, std::string_view what)
	{
		if (_reportedWideInteger)
			return;
		_reportedWideInteger = true;
		_diagnostics.error(DiagId::WideIntegerNotSupported, loc,
			"{} is not supported in generated code yet", what);
	}

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

	void IrBuilder::emitCopyInto(support::SourceLocation loc, IrValue result, IrValue source, bool isFloat)
	{
		if (result == source)
			return;
		IrCopyPayload payload;
		payload.result = result;
		payload.isFloat = isFloat;
		payload.source = source;
		emitVoid(loc, payload);
	}

	IrValue IrBuilder::emitLoad(support::SourceLocation loc, IrValue address, IrMemSize size, bool isFloat, bool isSigned,
		bool isVolatile)
	{
		IrLoadPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.size = size;
		payload.isFloat = isFloat;
		payload.isSigned = isSigned;
		payload.isVolatile = isVolatile;
		payload.address = address;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::loadOfType(support::SourceLocation loc, IrValue address, const Type* type)
	{
		// A pointer, an array and a struct are all word-sized addresses here, and Type::isSigned()
		// says false for them - which is the right answer for a load anyway, since a Word load fills
		// the whole register and has no extension to choose.
		return emitLoad(loc, address, memSizeOf(type), type && type->isFloat(), type && type->isSigned(),
			type && type->isVolatile());
	}

	void IrBuilder::emitStore(support::SourceLocation loc, IrValue address, IrMemSize size, IrValue value, bool isFloat,
		bool isVolatile)
	{
		IrStorePayload payload;
		payload.size = size;
		payload.isFloat = isFloat;
		payload.isVolatile = isVolatile;
		payload.address = address;
		payload.value = value;
		emitVoid(loc, payload);
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

	IrValue IrBuilder::emitCmp(support::SourceLocation loc, IrCmpPredicate predicate, IrValue lhs, IrValue rhs, bool isUnsigned, bool isFloat)
	{
		IrCmpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.predicate = predicate;
		payload.isUnsigned = isUnsigned;
		payload.isFloat = isFloat;
		payload.lhs = lhs;
		payload.rhs = rhs;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitBuiltin(support::SourceLocation loc, ast::Builtin builtin, IrValue a, IrValue b)
	{
		IrBuiltinPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.builtin = builtin;
		payload.a = a;
		payload.b = b;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitBinOp(support::SourceLocation loc, IrBinOp op, IrValue lhs, IrValue rhs, bool isUnsigned, bool isFloat)
	{
		IrBinOpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.op = op;
		payload.isUnsigned = isUnsigned;
		payload.isFloat = isFloat;
		payload.lhs = lhs;
		payload.rhs = rhs;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitUnOp(support::SourceLocation loc, IrUnOp op, IrValue operand, bool isFloat, bool isUnsigned)
	{
		IrUnOpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.op = op;
		payload.isFloat = isFloat;
		payload.isUnsigned = isUnsigned;
		payload.operand = operand;
		emitVoid(loc, payload);
		return payload.result;
	}

	IrValue IrBuilder::emitNarrow(support::SourceLocation loc, IrValue operand, IrMemSize size, bool isUnsigned)
	{
		IrUnOpPayload payload;
		payload.result = _currentFunction->newTemp();
		payload.op = IrUnOp::Narrow;
		payload.isUnsigned = isUnsigned;
		payload.narrowSize = size;
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

	// ---- composite memory: structs, aggregates, whole-object copies -----------------------------

	bool IrBuilder::isStructType(const Type* type) noexcept
	{
		return type && type->isAggregate();
	}

	bool IrBuilder::isIndirectStruct(const Type* type) noexcept
	{
		if (!isStructType(type))
			return false;
		u32 size = type->sizeInBytes();
		// 1/2/4 are exactly the sizes one ordinary byte/half/word Load or Store moves whole. A
		// 3-byte struct is deliberately NOT in that set even though it would fit a register: a word
		// store would write a fourth byte that does not belong to the object, and there is no
		// three-byte store to use instead.
		return !(size == 1 || size == 2 || size == 4) || type->alignment() < size;
	}

	IrValue IrBuilder::offsetAddress(support::SourceLocation loc, IrValue base, u32 offset)
	{
		if (offset == 0)
			return base;
		return emitBinOp(loc, IrBinOp::Add, base, emitConstInt(loc, static_cast<i64>(offset)), true);
	}

	void IrBuilder::emitMemoryCopy(support::SourceLocation loc, IrValue destAddr, IrValue sourceAddr,
		u32 sizeInBytes, u32 alignment)
	{
		// The widest piece the alignment allows, then narrower ones for whatever tail is left -
		// a 10-byte, 4-aligned object copies as word, word, half.
		u32 piece = (alignment >= 4) ? 4u : (alignment >= 2 ? 2u : 1u);
		u32 offset = 0;
		while (offset < sizeInBytes)
		{
			while (piece > 1 && offset + piece > sizeInBytes)
				piece /= 2;
			IrMemSize size = irMemSizeForBytes(piece);
			IrValue from = offsetAddress(loc, sourceAddr, offset);
			IrValue word = emitLoad(loc, from, size);
			IrValue to = offsetAddress(loc, destAddr, offset);
			emitStore(loc, to, size, word);
			offset += piece;
		}
	}

	void IrBuilder::emitZeroFill(support::SourceLocation loc, IrValue destAddr, u32 startOffset,
		u32 sizeInBytes, u32 alignment)
	{
		if (sizeInBytes == 0)
			return;

		// One Const feeds every store: a temp read many times is perfectly ordinary in this
		// non-SSA IR (ir_instr.h), and materializing a separate zero per piece would just leave
		// codegen's peepholes more to undo.
		IrValue zero = emitConstInt(loc, 0);
		// The start offset matters as much as the alignment: filling from offset 2 of a 4-aligned
		// object can only use halves, whatever the object's own alignment says.
		u32 piece = (alignment >= 4) ? 4u : (alignment >= 2 ? 2u : 1u);
		while (piece > 1 && (startOffset % piece) != 0)
			piece /= 2;

		u32 offset = startOffset;
		u32 end = startOffset + sizeInBytes;
		while (offset < end)
		{
			while (piece > 1 && offset + piece > end)
				piece /= 2;
			emitStore(loc, offsetAddress(loc, destAddr, offset), irMemSizeForBytes(piece), zero);
			offset += piece;
		}
	}

	u32 IrBuilder::newStructTempSlot(u32 sizeInBytes)
	{
		return _currentFunction->newLocalSlot(sizeInBytes, false);
	}

	// ---- 64-bit integers (F3.1b) -----------------------------------------------------------------
	//
	// A wide value IS the address of its 8-byte storage (see ir_builder.h): low word at +0, high
	// word at +4. Everything below works on that one convention, so no consumer can see a wide
	// value as a scalar by accident - the only thing an address can be is copied or have its two
	// words loaded.

	IrValue IrBuilder::loadWideWord(support::SourceLocation loc, IrValue wideAddr, bool high, bool isVolatile)
	{
		IrValue addr = high ? offsetAddress(loc, wideAddr, 4) : wideAddr;
		return emitLoad(loc, addr, IrMemSize::Word, false, false, isVolatile);
	}

	IrValue IrBuilder::makeWideValue(support::SourceLocation loc, IrValue low, IrValue high)
	{
		IrValue addr = emitFrameAddr(loc, newStructTempSlot(8));
		emitStore(loc, addr, IrMemSize::Word, low);
		emitStore(loc, offsetAddress(loc, addr, 4), IrMemSize::Word, high);
		return addr;
	}

	void IrBuilder::emitWideStore(support::SourceLocation loc, IrValue destAddr, IrValue value,
		const Type* fromType, bool isVolatile)
	{
		// A float source is F3.3's conversion, refused rather than reinterpreted. This is the one
		// chokepoint every wide store flows through (initializer, assignment, ternary, inc-dec), so
		// the guard has to live here and not only in convertForStore - those paths pass the source's
		// own type straight in.
		if (fromType && fromType->isFloat())
		{
			rejectWideFeature(loc, "a float-to-64-bit conversion");
			return;
		}

		if (fromType && fromType->isWideInteger())
		{
			// A wide source is already the address of the eight bytes. Its volatility governs the
			// LOADS (a `volatile long long` read has to stay observable); callers that have already
			// converted the source into a fresh non-volatile temp pass a plain wide type here.
			bool sourceVolatile = fromType->isVolatile();
			IrValue low = loadWideWord(loc, value, false, sourceVolatile);
			IrValue high = loadWideWord(loc, value, true, sourceVolatile);
			emitStore(loc, destAddr, IrMemSize::Word, low, false, isVolatile);
			emitStore(loc, offsetAddress(loc, destAddr, 4), IrMemSize::Word, high, false, isVolatile);
			return;
		}

		// A scalar source: the low word is the value itself (a narrow value is already extended to a
		// word by the narrowing invariant) and the high word is its sign or zero extension.
		emitStore(loc, destAddr, IrMemSize::Word, value, false, isVolatile);
		IrValue high = (fromType && fromType->isSigned())
			? emitBinOp(loc, IrBinOp::Sar, value, emitConstInt(loc, 31), false)
			: emitConstInt(loc, 0);
		emitStore(loc, offsetAddress(loc, destAddr, 4), IrMemSize::Word, high, false, isVolatile);
	}

	IrValue IrBuilder::materializeWide(support::SourceLocation loc, IrValue value, const Type* fromType)
	{
		if (fromType && fromType->isWideInteger())
			return value;
		IrValue addr = emitFrameAddr(loc, newStructTempSlot(8));
		emitWideStore(loc, addr, value, fromType, false);
		return addr;
	}

	IrValue IrBuilder::lowerWideArithmetic(support::SourceLocation loc, BinaryOp op, const Type* resultType,
		const Type* lhsType, const Type* rhsType, IrValue lhsVal, IrValue rhsVal)
	{
		// A 64-bit shift branches on the count; everything else here is a two-word computation.
		if (op == BinaryOp::Shl || op == BinaryOp::Shr)
		{
			IrValue value = materializeWide(loc, lhsVal, lhsType);
			IrValue amount = toWord(loc, rhsVal, rhsType);
			return lowerWideShift(loc, op, resultType, value, amount, lhsType && lhsType->isVolatile());
		}

		IrValue a = materializeWide(loc, lhsVal, lhsType);
		IrValue b = materializeWide(loc, rhsVal, rhsType);

		// Division and remainder are the one operation with no composed-in-IR form worth writing:
		// a restoring shift-subtract loop over 64 bits. It is emitted once, as CASM, at the end of
		// @text (codegen's emitEmittedRoutines), and called with the two operands' addresses and the
		// result's address - a pair of 32-bit pointers each, so no 64-bit calling convention is
		// needed. The routine leaves the quotient at `dest` and the remainder at `dest + 8`.
		if (op == BinaryOp::Div || op == BinaryOp::Mod)
		{
			IrValue dest = emitFrameAddr(loc, newStructTempSlot(16));
			IrValue signedFlag = emitConstInt(loc, (resultType && !resultType->isSigned()) ? 0 : 1);
			// The Params must sit immediately before the Call (codegen reads them back by position),
			// and `dest` is allocated before them so nothing is emitted in between.
			emitVoid(loc, IrParamPayload{ dest, false, false });
			emitVoid(loc, IrParamPayload{ a, false, false });
			emitVoid(loc, IrParamPayload{ b, false, false });
			emitVoid(loc, IrParamPayload{ signedFlag, false, false });
			IrCallPayload payload;
			payload.callee = "__cc_div64";
			payload.hasResult = false;
			payload.argCount = 4;
			emitVoid(loc, payload);
			return op == BinaryOp::Div ? dest : offsetAddress(loc, dest, 8);
		}

		bool aVolatile = lhsType && lhsType->isVolatile();
		bool bVolatile = rhsType && rhsType->isVolatile();
		IrValue aLow = loadWideWord(loc, a, false, aVolatile);
		IrValue aHigh = loadWideWord(loc, a, true, aVolatile);
		IrValue bLow = loadWideWord(loc, b, false, bVolatile);
		IrValue bHigh = loadWideWord(loc, b, true, bVolatile);

		IrValue low = IrValue{};
		IrValue high = IrValue{};
		switch (op)
		{
			case BinaryOp::Add:
			{
				// Carry out of the low word is `sum < a` (unsigned, so a wrap-around is exactly the
				// carry) - there is no ADDC in the IR, but one Cmp is all the flag it would need.
				low = emitBinOp(loc, IrBinOp::Add, aLow, bLow, true);
				IrValue carry = emitCmp(loc, IrCmpPredicate::Lt, low, aLow, true);
				IrValue highSum = emitBinOp(loc, IrBinOp::Add, aHigh, bHigh, true);
				high = emitBinOp(loc, IrBinOp::Add, highSum, carry, true);
				break;
			}
			case BinaryOp::Sub:
			{
				// Borrow out of the low word is `a < b` (unsigned).
				low = emitBinOp(loc, IrBinOp::Sub, aLow, bLow, true);
				IrValue borrow = emitCmp(loc, IrCmpPredicate::Lt, aLow, bLow, true);
				IrValue highDiff = emitBinOp(loc, IrBinOp::Sub, aHigh, bHigh, true);
				high = emitBinOp(loc, IrBinOp::Sub, highDiff, borrow, true);
				break;
			}
			case BinaryOp::Mul:
			{
				// The full 64-bit product from the 32-bit pieces:
				//   lo = low32(al*bl)
				//   hi = high32(al*bl) + low32(ah*bl) + low32(al*bh)   (mod 2^32)
				// The low 64 bits of a signed product are the same bit pattern as the unsigned one
				// (two's complement), so the cross terms use the unsigned multiply-high.
				IrValue lowProduct = emitBinOp(loc, IrBinOp::Mul, aLow, bLow, true);
				IrValue highProduct = emitBuiltin(loc, ast::Builtin::MulhUnsigned, aLow, bLow);
				IrValue crossHighLow = emitBinOp(loc, IrBinOp::Mul, aHigh, bLow, true);
				IrValue crossLowHigh = emitBinOp(loc, IrBinOp::Mul, aLow, bHigh, true);
				IrValue highSum = emitBinOp(loc, IrBinOp::Add, highProduct, crossHighLow, true);
				high = emitBinOp(loc, IrBinOp::Add, highSum, crossLowHigh, true);
				low = lowProduct;
				break;
			}
			case BinaryOp::BitAnd:
				low = emitBinOp(loc, IrBinOp::And, aLow, bLow, true);
				high = emitBinOp(loc, IrBinOp::And, aHigh, bHigh, true);
				break;
			case BinaryOp::BitOr:
				low = emitBinOp(loc, IrBinOp::Or, aLow, bLow, true);
				high = emitBinOp(loc, IrBinOp::Or, aHigh, bHigh, true);
				break;
			case BinaryOp::BitXor:
				low = emitBinOp(loc, IrBinOp::Xor, aLow, bLow, true);
				high = emitBinOp(loc, IrBinOp::Xor, aHigh, bHigh, true);
				break;
			default:
				return lhsVal; // comparisons/logical never reach lowerArithmetic()
		}
		return makeWideValue(loc, low, high);
	}

	IrValue IrBuilder::lowerWideCompare(support::SourceLocation loc, BinaryOp op, IrValue lhsAddr, IrValue rhsAddr, bool isUnsigned)
	{
		IrValue aLow = loadWideWord(loc, lhsAddr, false, false);
		IrValue aHigh = loadWideWord(loc, lhsAddr, true, false);
		IrValue bLow = loadWideWord(loc, rhsAddr, false, false);
		IrValue bHigh = loadWideWord(loc, rhsAddr, true, false);

		// `highCmp` orders the two values (signed when the wide type is); the low words are compared
		// unsigned because the sign lives in the high word. The result is `highCmp || (highEq &&
		// lowCmp)`, or its negation for Eq/Ne - each Cmp already yields a 0/1 word, so the boolean
		// combination is plain and/or, no control flow needed.
		IrValue highEq = emitCmp(loc, IrCmpPredicate::Eq, aHigh, bHigh, false);
		switch (op)
		{
			case BinaryOp::Eq:
			{
				IrValue lowEq = emitCmp(loc, IrCmpPredicate::Eq, aLow, bLow, false);
				return emitBinOp(loc, IrBinOp::And, highEq, lowEq, true);
			}
			case BinaryOp::Ne:
			{
				IrValue lowNe = emitCmp(loc, IrCmpPredicate::Ne, aLow, bLow, false);
				IrValue highNe = emitCmp(loc, IrCmpPredicate::Ne, aHigh, bHigh, false);
				return emitBinOp(loc, IrBinOp::Or, highNe, lowNe, true);
			}
			case BinaryOp::Lt:
			case BinaryOp::Le:
			case BinaryOp::Gt:
			case BinaryOp::Ge:
			{
				IrCmpPredicate highPredicate = (op == BinaryOp::Lt || op == BinaryOp::Le)
					? IrCmpPredicate::Lt : IrCmpPredicate::Gt;
				IrCmpPredicate lowPredicate = cmpPredicateFor(op);
				IrValue highCmp = emitCmp(loc, highPredicate, aHigh, bHigh, isUnsigned);
				IrValue lowCmp = emitCmp(loc, lowPredicate, aLow, bLow, true);
				IrValue sameHighAndLow = emitBinOp(loc, IrBinOp::And, highEq, lowCmp, true);
				return emitBinOp(loc, IrBinOp::Or, highCmp, sameHighAndLow, true);
			}
			default:
				return emitConstInt(loc, 0);
		}
	}

	IrValue IrBuilder::lowerWideShift(support::SourceLocation loc, BinaryOp op, const Type* resultType,
		IrValue value, IrValue amount, bool valueVolatile)
	{
		// C defines a shift by 0..63; the amount is an int (or was truncated to one by the caller).
		// The count decides between three shapes, and each is a block, because a 64-bit shift is two
		// 32-bit shifts whose direction changes at 32 and the ISA masks a shift count to 5 bits:
		//   0       -> unchanged
		//   1..31   -> small: the two words cross-shift
		//   32..63  -> large: the low word comes from the high word (or is zeroed for a left shift)
		bool arithmetic = op == BinaryOp::Shr && resultType && resultType->isSigned();
		IrValue lowWord = loadWideWord(loc, value, false, valueVolatile);
		IrValue highWord = loadWideWord(loc, value, true, valueVolatile);

		BasicBlock& largeBlock = _currentFunction->createBlock();
		BasicBlock& testZeroBlock = _currentFunction->createBlock();
		BasicBlock& zeroBlock = _currentFunction->createBlock();
		BasicBlock& smallBlock = _currentFunction->createBlock();
		BasicBlock& mergeBlock = _currentFunction->createBlock();

		// The result's home and the two constants are materialized before the branch, so they
		// dominate every arm.
		IrValue resultAddr = emitFrameAddr(loc, newStructTempSlot(8));
		IrValue thirtyTwo = emitConstInt(loc, 32);
		IrValue zero = emitConstInt(loc, 0);

		emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Ge, true, false, amount, thirtyTwo, &largeBlock, &testZeroBlock });

		_currentBlock = &testZeroBlock;
		emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Eq, false, false, amount, zero, &zeroBlock, &smallBlock });

		// n == 0: the value is unchanged.
		_currentBlock = &zeroBlock;
		emitStore(loc, resultAddr, IrMemSize::Word, lowWord);
		emitStore(loc, offsetAddress(loc, resultAddr, 4), IrMemSize::Word, highWord);
		emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &smallBlock;
		{
			IrValue complement = emitBinOp(loc, IrBinOp::Sub, thirtyTwo, amount, true); // 32 - n, in 1..31
			IrValue newLow = IrValue{};
			IrValue newHigh = IrValue{};
			if (op == BinaryOp::Shl)
			{
				newLow = emitBinOp(loc, IrBinOp::Shl, lowWord, amount, false);
				IrValue crossed = emitBinOp(loc, IrBinOp::Shr, lowWord, complement, true); // low >>u (32-n)
				IrValue shiftedHigh = emitBinOp(loc, IrBinOp::Shl, highWord, amount, false);
				newHigh = emitBinOp(loc, IrBinOp::Or, shiftedHigh, crossed, true);
			}
			else
			{
				IrValue lowPart = emitBinOp(loc, IrBinOp::Shr, lowWord, amount, true);
				IrValue crossed = emitBinOp(loc, IrBinOp::Shl, highWord, complement, false); // high << (32-n)
				newLow = emitBinOp(loc, IrBinOp::Or, lowPart, crossed, true);
				newHigh = emitBinOp(loc, arithmetic ? IrBinOp::Sar : IrBinOp::Shr, highWord, amount, false);
			}
			emitStore(loc, resultAddr, IrMemSize::Word, newLow);
			emitStore(loc, offsetAddress(loc, resultAddr, 4), IrMemSize::Word, newHigh);
		}
		emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &largeBlock;
		{
			IrValue excess = emitBinOp(loc, IrBinOp::Sub, amount, thirtyTwo, true); // n - 32, in 0..31
			IrValue newLow = IrValue{};
			IrValue newHigh = IrValue{};
			if (op == BinaryOp::Shl)
			{
				newLow = emitConstInt(loc, 0);
				newHigh = emitBinOp(loc, IrBinOp::Shl, lowWord, excess, false);
			}
			else
			{
				newLow = emitBinOp(loc, arithmetic ? IrBinOp::Sar : IrBinOp::Shr, highWord, excess, false);
				newHigh = arithmetic
					? emitBinOp(loc, IrBinOp::Sar, highWord, emitConstInt(loc, 31), false)
					: emitConstInt(loc, 0);
			}
			emitStore(loc, resultAddr, IrMemSize::Word, newLow);
			emitStore(loc, offsetAddress(loc, resultAddr, 4), IrMemSize::Word, newHigh);
		}
		emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &mergeBlock;
		return resultAddr;
	}

	IrValue IrBuilder::lowerWideNegate(support::SourceLocation loc, IrValue address, bool isVolatile)
	{
		// Two's complement on the pair: `low' = -low`, `high' = ~high + (low == 0)` - the carry out of
		// negating the low word.
		IrValue low = loadWideWord(loc, address, false, isVolatile);
		IrValue high = loadWideWord(loc, address, true, isVolatile);
		IrValue zero = emitConstInt(loc, 0);
		IrValue negatedLow = emitBinOp(loc, IrBinOp::Sub, zero, low, true);
		IrValue lowIsZero = emitCmp(loc, IrCmpPredicate::Eq, low, zero, false);
		IrValue notHigh = emitBinOp(loc, IrBinOp::Xor, high, emitConstInt(loc, -1), true);
		IrValue negatedHigh = emitBinOp(loc, IrBinOp::Add, notHigh, lowIsZero, true);
		return makeWideValue(loc, negatedLow, negatedHigh);
	}

	IrValue IrBuilder::lowerWideMagnitudeToFloat(support::SourceLocation loc, IrValue address, bool isVolatile)
	{
		IrValue lowWord = loadWideWord(loc, address, false, isVolatile);
		IrValue highWord = loadWideWord(loc, address, true, isVolatile);
		IrValue scale = emitConstFloat(loc, 65536.0f);
		auto chunk = [&](IrValue word, bool highHalf)
		{
			IrValue part = highHalf
				? emitBinOp(loc, IrBinOp::Shr, word, emitConstInt(loc, 16), true)
				: emitBinOp(loc, IrBinOp::And, word, emitConstInt(loc, 0xFFFF), true);
			return emitUnOp(loc, IrUnOp::IntToFloat, part, false, true); // iitof: a 16-bit chunk is exact
		};
		IrValue result = chunk(highWord, true);
		result = emitBinOp(loc, IrBinOp::Add, emitBinOp(loc, IrBinOp::Mul, result, scale, false, true),
			chunk(highWord, false), false, true);
		result = emitBinOp(loc, IrBinOp::Add, emitBinOp(loc, IrBinOp::Mul, result, scale, false, true),
			chunk(lowWord, true), false, true);
		result = emitBinOp(loc, IrBinOp::Add, emitBinOp(loc, IrBinOp::Mul, result, scale, false, true),
			chunk(lowWord, false), false, true);
		return result;
	}

	IrValue IrBuilder::lowerFloatMagnitudeToWide(support::SourceLocation loc, IrValue value)
	{
		IrValue resultAddr = emitFrameAddr(loc, newStructTempSlot(8));
		IrValue scale48 = emitConstFloat(loc, 281474976710656.0f);  // 2^48
		IrValue scale32 = emitConstFloat(loc, 4294967296.0f);       // 2^32
		IrValue scale16 = emitConstFloat(loc, 65536.0f);            // 2^16
		IrValue inv48 = emitConstFloat(loc, 1.0f / 281474976710656.0f);
		IrValue inv32 = emitConstFloat(loc, 1.0f / 4294967296.0f);
		IrValue inv16 = emitConstFloat(loc, 1.0f / 65536.0f);
		auto chunk = [&](IrValue current, IrValue inverse, IrValue scale) -> std::pair<IrValue, IrValue>
		{
			IrValue scaled = emitBinOp(loc, IrBinOp::Mul, current, inverse, false, true);
			IrValue part = emitUnOp(loc, IrUnOp::FloatToInt, scaled, false, true); // ftoii: a 16-bit value
			IrValue back = emitBinOp(loc, IrBinOp::Mul, emitUnOp(loc, IrUnOp::IntToFloat, part, false, true), scale, false, true);
			IrValue remaining = emitBinOp(loc, IrBinOp::Sub, current, back, false, true);
			return { part, remaining };
		};
		auto [part3, rem3] = chunk(value, inv48, scale48);
		auto [part2, rem2] = chunk(rem3, inv32, scale32);
		auto [part1, rem1] = chunk(rem2, inv16, scale16);
		IrValue part0 = emitUnOp(loc, IrUnOp::FloatToInt, rem1, false, true);
		emitStore(loc, resultAddr, IrMemSize::Half, part0);
		emitStore(loc, offsetAddress(loc, resultAddr, 2), IrMemSize::Half, part1);
		emitStore(loc, offsetAddress(loc, resultAddr, 4), IrMemSize::Half, part2);
		emitStore(loc, offsetAddress(loc, resultAddr, 6), IrMemSize::Half, part3);
		return resultAddr;
	}

	IrValue IrBuilder::toWord(support::SourceLocation loc, IrValue value, const Type* type)
	{
		if (type && type->isWideInteger())
			return emitLoad(loc, value, IrMemSize::Word, false, false);
		return value;
	}
	void IrBuilder::lowerInitializerInto(support::SourceLocation loc, IrValue baseAddr, u32 offset,
		const Type* type, Expr* init)
	{
		if (!init || !type)
			return;

		u32 totalSize = type->sizeInBytes();
		u32 align = type->alignment();

		// `char s[8] = "hola"` - the literal's own bytes go straight into the array, terminating
		// zero and all, and whatever is left over zero-fills. Nothing reaches .rodata for this one:
		// unlike every other use of a string literal, no pointer to a shared copy is taken. A wide
		// literal's value is already its code units' little-endian bytes, so it is stored the same way.
		if (type->isArray())
		{
			// `char s[] = { "abc" }` as well: braces around a string that fills the array are transparent.
			if (const auto* literal = ast::stringFillingArray(type, init))
			{
				std::string_view text = literal->value().view();
				u32 written = 0;
				for (char c : text)
				{
					if (written >= totalSize)
						break; // sema already reported the overflow - just don't write past the object
					emitStore(loc, offsetAddress(loc, baseAddr, offset + written), IrMemSize::Byte,
						emitConstInt(loc, static_cast<i64>(static_cast<unsigned char>(c))));
					++written;
				}
				emitZeroFill(loc, baseAddr, offset + written, totalSize - written, 1); // the terminating zero is part of this
				return;
			}
		}

		auto* list = dynamic_cast<ast::InitListExpr*>(init);
		if (!list)
		{
			// An ordinary expression. A struct-typed one is an address (see ir_builder.h), so it is
			// copied rather than stored; a 64-bit one is the address of its two words; everything
			// else is one scalar store.
			if (isStructType(type))
			{
				emitMemoryCopy(loc, offsetAddress(loc, baseAddr, offset), lowerExpr(init), totalSize, align);
				return;
			}
			if (type->isWideInteger())
			{
				// A wide source is copied as its own address (its volatility rides on the loads); a
				// scalar or float source is converted first. Keeping the two apart is what stops the
				// DESTINATION's qualifier from governing the source's reads.
				if (init->type() && init->type()->isWideInteger())
					emitWideStore(loc, offsetAddress(loc, baseAddr, offset), lowerExpr(init), init->type(), type->isVolatile());
				else
					emitWideStore(loc, offsetAddress(loc, baseAddr, offset),
						convertForStore(loc, lowerExpr(init), init->type(), type), type, type->isVolatile());
				return;
			}
			IrValue value = convertForStore(loc, lowerExpr(init), init->type(), type);
			emitStore(loc, offsetAddress(loc, baseAddr, offset), memSizeOf(type), value, type->isFloat(), type->isVolatile());
			return;
		}

		std::span<Expr* const> elements = list->elements();

		if (type->isArray())
		{
			const Type* elementType = type->arrayElementType();
			u32 elementSize = elementType ? elementType->sizeInBytes() : 1u;
			u32 count = static_cast<u32>(elements.size());
			if (count > type->arraySize())
				count = type->arraySize(); // sema already reported it
			for (u32 i = 0; i < count; ++i)
				lowerInitializerInto(loc, baseAddr, offset + i * elementSize, elementType, elements[i]);
			emitZeroFill(loc, baseAddr, offset + count * elementSize, totalSize - count * elementSize, align);
			return;
		}

		if (type->isStruct())
		{
			ast::StructDecl* decl = type->structDecl();
			if (!decl)
				return; // sema already reported the incomplete type
			std::span<const FieldDecl> fields = decl->fields();
			u32 count = static_cast<u32>(std::min(elements.size(), fields.size()));
			for (u32 i = 0; i < count; ++i)
				lowerInitializerInto(loc, baseAddr, offset + sema::fieldOffset(*decl, i), fields[i].type, elements[i]);

			// Everything from the first field the list did not reach onwards is zeroed, as C
			// requires. Padding BETWEEN the fields the list did reach is left alone - C leaves a
			// struct's padding unspecified, and writing it would cost stores for bytes no correct
			// program can observe.
			u32 initialized = sema::fieldOffset(*decl, count);
			if (initialized < totalSize)
				emitZeroFill(loc, baseAddr, offset + initialized, totalSize - initialized, align);
			return;
		}

		// A scalar with braces - `int x = { 5 }`. Sema already required exactly one value.
		if (!elements.empty())
			lowerInitializerInto(loc, baseAddr, offset, type, elements[0]);
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
				case BinaryOp::Comma: break; // never a constant expression, even when both sides are
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
			{
				if (symbol->isIndirect)
				{
					// A by-value struct parameter the caller passed as the address of its own copy
					// (ir_builder.h): the slot holds that pointer, so the object's address is the
					// pointer's VALUE, one load away - not the slot's own address.
					return emitLoad(loc, emitFrameAddr(loc, symbol->localSlot), IrMemSize::Word);
				}
				return emitFrameAddr(loc, symbol->localSlot);
			}
			// Global (or an unresolved symbol - assumed not to happen on sema-checked input, see
			// the header comment on IrBuilder's contract).
			if (symbol && !symbol->globalName.empty())
				return emitGlobalAddr(loc, symbol->globalName);
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
			// A 64-bit index is converted to `int` for the addressing, exactly as C converts a
			// subscript to ptrdiff.
			IrValue indexValue = toWord(loc, lowerExpr(index->index()), index->index()->type());
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

		if (auto* literal = dynamic_cast<ast::CompoundLiteralExpr*>(expr))
		{
			const Type* type = literal->literalType();
			IrValue addr = emitFrameAddr(loc, newStructTempSlot(type->sizeInBytes()));
			lowerInitializerInto(loc, addr, 0, type, literal->list());
			return addr;
		}

		// Every other Expr kind reaching here is not one of this subset's lvalue forms (see
		// sema::Sema::isLValue(), sema.cpp) - but unlike isLValue(), sema's own visit(MemberExpr&)
		// only requires a `.` base to have struct *type*, not to actually be an lvalue (sema.cpp),
		// so e.g. `make().y` for a struct-returning `make()` reaches here on perfectly valid,
		// sema-checked input. That works out on its own: a struct-typed expression lowers to the
		// ADDRESS of its storage (ir_builder.h), and a struct-returning call's storage is the temp
		// slot the call wrote into - so the "value" this falls back to really is the address the
		// caller asked for. For anything else this is unreachable on sema-checked input, and the
		// value is still the most useful thing to hand back.
		return lowerExpr(expr);
	}

	IrValue IrBuilder::lowerRValue(Expr* expr)
	{
		// Array-to-pointer decay (see the header comment on this method): the array's storage IS the
		// value, so this returns its address directly instead of loading through it - never possible
		// to represent a whole array's bytes as a single scalar Load result anyway.
		//
		// A struct behaves the same way, for the same reason and by the same convention (see
		// ir_builder.h's note on struct-typed expressions) - the difference being that an array
		// really does decay to a pointer in C's own type system, while a struct does not: its
		// address is simply how this IR represents it, and whoever consumes it knows to copy from
		// there rather than treat it as a pointer value.
		const Type* type = expr->type();
		if (type && (type->isArray() || type->isAggregate() || type->isWideInteger()))
			return lowerAddress(expr);
		// A function decays to a pointer to itself, and its address IS its value - there is nothing
		// to load through. Same shape as the array case above, and the same reason: a function type
		// names no object, so no load could produce one.
		if (type && type->isFunction())
			return lowerAddress(expr);
		IrValue addr = lowerAddress(expr);
		return loadOfType(expr->location(), addr, type);
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
		// A 64-bit condition is true when either word is non-zero (there is no -0 to worry about),
		// which is one `or` and the same branch the scalar case already takes.
		if (cond->type() && cond->type()->isWideInteger())
		{
			IrValue low = loadWideWord(loc, value, false, false);
			IrValue high = loadWideWord(loc, value, true, false);
			IrValue either = emitBinOp(loc, IrBinOp::Or, low, high, true);
			emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Ne, false, false, either, emitConstInt(loc, 0), trueBlock, falseBlock });
			return;
		}
		bool isFloat = cond->type() && cond->type()->isFloat();
		IrValue zero = isFloat ? emitConstFloat(loc, 0.0f) : emitConstInt(loc, 0);
		emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Ne, isFloat, isFloat, value, zero, trueBlock, falseBlock });
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

	IrValue IrBuilder::toFloatIfNeeded(support::SourceLocation loc, IrValue value, const Type* type)
	{
		if (type && type->isFloat())
			return value;
		// A wide operand is converted to float through the same helper a cast uses - the whole
		// 64-bit value, not the address's low word.
		if (type && type->isWideInteger())
			return convertForStore(loc, value, type, &Type::Float);
		return emitUnOp(loc, IrUnOp::IntToFloat, value, false, !type || !type->isSigned());
	}

	IrValue IrBuilder::convertForStore(support::SourceLocation loc, IrValue value, const Type* fromType, const Type* toType)
	{
		bool toWide = toType && toType->isWideInteger();
		bool fromWide = fromType && fromType->isWideInteger();

		// A value moving into a 64-bit object: a wide source is already the address of its bytes, a
		// scalar one is extended into a fresh temp. Either way the result IS the wide value (its
		// address), which is what every wide consumer expects.
		if (toWide)
		{
			// A float source is converted whole (F3.3): the magnitude is written sixteen bits at a
			// time, then negated if the float was negative. A negative value into an UNSIGNED 64-bit
			// type is undefined in C, so it takes the magnitude path.
			if (fromType && fromType->isFloat())
			{
				// A negative value into an UNSIGNED 64-bit type is UB, so that case needs no branch
				// and no negative arm at all - the magnitude is the whole answer.
				if (!toType->isSigned())
					return lowerFloatMagnitudeToWide(loc, value);

				// A negative value needs a branch on `value < 0`. A float CondJump operand is routed
				// through the INTEGER register pool by codegen (emitConditionalBranch), so the test
				// is a materialized FCMP instead - the only float comparison form the back end reads
				// correctly.
				BasicBlock& negativeBlock = _currentFunction->createBlock();
				BasicBlock& positiveBlock = _currentFunction->createBlock();
				BasicBlock& mergeBlock = _currentFunction->createBlock();
				IrValue resultAddr = emitFrameAddr(loc, newStructTempSlot(8));
				IrValue isNegative = emitCmp(loc, IrCmpPredicate::Lt, value, emitConstFloat(loc, 0.0f), true, true);
				emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Ne, false, false, isNegative, emitConstInt(loc, 0), &negativeBlock, &positiveBlock });

				_currentBlock = &negativeBlock;
				{
					IrValue positive = emitUnOp(loc, IrUnOp::Neg, value, true);
					IrValue magnitude = lowerFloatMagnitudeToWide(loc, positive);
					IrValue negated = lowerWideNegate(loc, magnitude, false);
					emitMemoryCopy(loc, resultAddr, negated, 8, 8);
				}
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

				_currentBlock = &positiveBlock;
				{
					IrValue magnitude = lowerFloatMagnitudeToWide(loc, value);
					emitMemoryCopy(loc, resultAddr, magnitude, 8, 8);
				}
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

				_currentBlock = &mergeBlock;
				return resultAddr;
			}
			return materializeWide(loc, value, fromType);
		}

		// A 64-bit value moving into a scalar: the low word is the value (truncation), and the
		// scalar conversions below (bool, narrowing) then apply to it as to any other word.
		if (fromWide)
		{
			// A float target converts the whole 64-bit value (F3.3), sign and all, not its address.
			if (toType && toType->isFloat())
			{
				if (!fromType->isSigned())
					return lowerWideMagnitudeToFloat(loc, value, fromType->isVolatile());

				// Read the pair ONCE into a fresh temp: a `volatile long long` cast to float implies
				// a single pair of reads, so both the sign test and the conversion work on the
				// snapshot (a non-volatile source gets the same snapshot, harmlessly).
				IrValue snapshot = makeWideValue(loc,
					loadWideWord(loc, value, false, fromType->isVolatile()),
					loadWideWord(loc, value, true, fromType->isVolatile()));

				BasicBlock& negativeBlock = _currentFunction->createBlock();
				BasicBlock& positiveBlock = _currentFunction->createBlock();
				BasicBlock& mergeBlock = _currentFunction->createBlock();
				IrValue result = _currentFunction->newTemp();
				IrValue high = loadWideWord(loc, snapshot, true, false);
				IrValue zeroWord = emitConstInt(loc, 0);
				emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Lt, false, false, high, zeroWord, &negativeBlock, &positiveBlock });

				_currentBlock = &negativeBlock;
				{
					IrValue negated = lowerWideNegate(loc, snapshot, false);
					IrValue magnitude = lowerWideMagnitudeToFloat(loc, negated, false);
					emitCopyInto(loc, result, emitUnOp(loc, IrUnOp::Neg, magnitude, true), true);
				}
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

				_currentBlock = &positiveBlock;
				emitCopyInto(loc, result, lowerWideMagnitudeToFloat(loc, snapshot, false), true);
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

				_currentBlock = &mergeBlock;
				return result;
			}
			// ...except to bool, where C asks whether the WHOLE 64-bit value is non-zero: the low
			// word alone cannot answer that (`bool b = 0x100000000LL;` is true).
			if (toType && toType->isBool())
			{
				bool isVolatile = fromType->isVolatile();
				IrValue low = loadWideWord(loc, value, false, isVolatile);
				IrValue high = loadWideWord(loc, value, true, isVolatile);
				return emitUnOp(loc, IrUnOp::ToBool, emitBinOp(loc, IrBinOp::Or, low, high, true));
			}
			value = emitLoad(loc, value, IrMemSize::Word, false, toType && toType->isSigned());
			fromType = nullptr;
		}

		bool fromFloat = fromType && fromType->isFloat();
		bool toFloat = toType && toType->isFloat();

		if (toFloat)
			return fromFloat ? value : emitUnOp(loc, IrUnOp::IntToFloat, value, false, !fromType || !fromType->isSigned());

		if (toType && toType->isBool() && fromFloat)
		{
			IrCmpPayload payload;
			payload.result = _currentFunction->newTemp();
			payload.predicate = IrCmpPredicate::Ne;
			payload.isFloat = true;
			payload.lhs = value;
			payload.rhs = emitConstFloat(loc, 0.0f);
			emitVoid(loc, payload);
			return payload.result;
		}

		if (fromFloat)
		{
			// Across the bank first, then the integer conversions below apply to the result exactly
			// as they would to any other int: `(char)1000.0f` truncates twice, once per rule.
			value = emitUnOp(loc, IrUnOp::FloatToInt, value, false, !toType || !toType->isSigned());
			fromType = nullptr; // an int of the register's own width now, not the float it started as
		}

		if (!toType)
			return value;

		// C converts to bool by asking "is it zero", not by keeping the low bit: (bool)256 is true.
		if (toType->isBool())
			return (fromType && fromType->isBool()) ? value : emitUnOp(loc, IrUnOp::ToBool, value);

		// Narrowing to char/short - the only integer types this ABI has that are narrower than a
		// register (enum, long and every pointer are all word-sized, so nothing is lost moving into
		// one). Skipped when the source is already exactly that type: the invariant says such a
		// value is already in range, so a `char` copied into another `char` needs nothing.
		bool toIsNarrow = toType->isChar() || toType->isSChar() || toType->isUChar() ||
			toType->isShort() || toType->isUShort();
		if (!toIsNarrow)
			return value;
		if (fromType && fromType->kind() == toType->kind())
			return value;
		return emitNarrow(loc, value, memSizeOf(toType), !toType->isSigned());
	}

	IrValue IrBuilder::lowerArithmetic(support::SourceLocation loc, BinaryOp op, const Type* resultType,
		const Type* lhsType, const Type* rhsType, IrValue lhsVal, IrValue rhsVal)
	{
		// A 64-bit result has its own lowering (the two-word pair) - see lowerWideArithmetic().
		if (resultType && resultType->isWideInteger())
			return lowerWideArithmetic(loc, op, resultType, lhsType, rhsType, lhsVal, rhsVal);

		// A scalar result with a wide operand - a pointer offset (`p + wide`), a shift amount
		// (`int << wide`) - converts that operand to a word first, exactly as C converts it to
		// `int`. A wide result never reaches here (the dispatch above caught it). A FLOAT result is
		// left alone: its wide operands are converted whole by toFloatIfNeeded() below, not truncated.
		if (!(resultType && resultType->isFloat()))
		{
			lhsVal = toWord(loc, lhsVal, lhsType);
			rhsVal = toWord(loc, rhsVal, rhsType);
		}

		// isArray() alongside isPointer(): node.lhs()->type()/node.rhs()->type() (this function's
		// only caller, visit(BinaryExpr&)) are the AST's own, undecayed annotations - an array
		// operand's real Type stays Array there even though sema type-checked `arr + 1` as pointer
		// arithmetic against a locally decayed copy (see Sema::decayArray(), sema.cpp). Both kinds
		// already share arrayElementType() below, so treating them alike here is enough to get the
		// right per-element scaling without a separate decay step.
		bool lhsIsPointer = lhsType && (lhsType->isPointer() || lhsType->isArray());
		bool rhsIsPointer = rhsType && (rhsType->isPointer() || rhsType->isArray());

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
		bool isFloat = resultType && resultType->isFloat(); // Add/Sub/Mul/Div only - see IrBinOpPayload::isFloat's header comment
		if (isFloat)
		{
			lhsVal = toFloatIfNeeded(loc, lhsVal, lhsType);
			rhsVal = toFloatIfNeeded(loc, rhsVal, rhsType);
		}
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
		return emitBinOp(loc, irOp, lhsVal, rhsVal, isUnsigned, isFloat);
	}

	// ---- expressions --------------------------------------------------------------------------------

	void IrBuilder::visit(ast::IntLiteralExpr& node)
	{
		// A 64-bit literal's two words come straight from the source value (sema typed it LongLong/
		// ULongLong from the `ll`/`LL` suffix) - not from materializeWide(), whose extension would
		// be wrong for a literal like 0x100000000LL whose high word is genuinely non-zero.
		if (node.type() && node.type()->isWideInteger())
		{
			u64 bits = static_cast<u64>(node.value());
			IrValue low = emitConstInt(node.location(), static_cast<i64>(static_cast<u32>(bits)));
			IrValue high = emitConstInt(node.location(), static_cast<i64>(static_cast<u32>(bits >> 32)));
			_lastValue = makeWideValue(node.location(), low, high);
			return;
		}
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
		_module.addStringLiteral(name, node.value(), node.elementSize());
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
		// lowerRValue() decays an array-typed variable to its own address instead of loading through
		// it - see its header comment.
		_lastValue = lowerRValue(&node);
	}

	void IrBuilder::visit(ast::CallExpr& node)
	{
		support::SourceLocation loc = node.location();
		const Type* resultType = node.type();
		bool hasResult = resultType && !resultType->isVoid();

		// F3.4: a 64-bit result comes back in two registers/words. The caller gets a fresh 8-byte
		// temp, which codegen fills with the two returned words; the CallExpr's value is that temp.
		bool returnsWide = hasResult && resultType->isWideInteger();
		IrValue wideResultAddr{};
		if (returnsWide)
			wideResultAddr = emitFrameAddr(loc, newStructTempSlot(8));


		// A struct coming back through memory needs somewhere to come back TO, decided here rather
		// than by the callee: a fresh frame slot, whose address becomes the call's hidden first
		// argument (ir_builder.h's struct convention). The result of the whole CallExpr is then that
		// slot's address, which is exactly what a struct-typed expression is expected to be.
		bool returnsStructIndirect = hasResult && isIndirectStruct(resultType);
		IrValue hiddenDest{};
		if (returnsStructIndirect)
			hiddenDest = emitFrameAddr(loc, newStructTempSlot(resultType->sizeInBytes()));

		// A small struct returned in ret0 also needs a home, since the CallExpr must still produce
		// an address - but the caller gets to write it AFTER the call, from the returned word.
		u32 smallStructSlot = 0;
		bool returnsStructInRegister = hasResult && isStructType(resultType) && !returnsStructIndirect;
		if (returnsStructInRegister)
			smallStructSlot = newStructTempSlot(resultType->sizeInBytes());

		std::vector<IrValue> argValues;
		std::vector<bool> argIsFloat;
		std::vector<bool> argIsWide;
		argValues.reserve(node.args().size() + 1);
		argIsFloat.reserve(node.args().size() + 1);
		argIsWide.reserve(node.args().size() + 1);
		if (returnsStructIndirect)
		{
			argValues.push_back(hiddenDest);
			argIsFloat.push_back(false);
			argIsWide.push_back(false);
		}

		// The callee's parameter types: from its declaration when the callee names a function, and
		// otherwise from the callee's own type - a pointer to a function, or a function a _Generic
		// chose - which carries them just the same. Only an unprototyped callee has none, and then
		// each argument is passed with the type it was computed as. Without this, a call through a
		// pointer passed an int to a long long parameter as one word and a variadic tail as fixed
		// arguments.
		// Whether the callee names a FUNCTION is sema's answer, recorded as the name's type (the
		// function's own type, or the pointer a variable holds) - not a lookup by name here, which a
		// local pointer shadowing a global function of the same name would get wrong.
		std::vector<const Type*> paramTypes;
		paramTypes.reserve(node.args().size());
		bool calleeIsVariadic = false;
		auto* namedCallee = dynamic_cast<ast::NameExpr*>(node.callee());
		const bool namesFunction = namedCallee && namedCallee->type() && namedCallee->type()->isFunction();
		auto declared = namesFunction ? _functionDecls.find(namedCallee->name()) : _functionDecls.end();
		if (declared != _functionDecls.end())
		{
			for (const ast::Param& param : declared->second->params())
				paramTypes.push_back(param.type);
			calleeIsVariadic = declared->second->isVariadic();
		}
		else if (const Type* calleeType = node.callee() ? node.callee()->type() : nullptr)
		{
			if (const ast::FunctionTypeInfo* signature = calleeType->calleeSignature())
			{
				for (const Type* paramType : signature->params())
					paramTypes.push_back(paramType);
				calleeIsVariadic = signature->isVariadic;
			}
		}
		// Where this callee's variadic tail starts, counted in the same argValues indices the Param
		// instructions below are emitted from - so the hidden struct-return pointer, which is an
		// argument here but not in the source, is already accounted for. ~0u means "no tail".
		u32 fixedArgCount = ~0u;
		if (calleeIsVariadic)
			fixedArgCount = static_cast<u32>(paramTypes.size()) + (returnsStructIndirect ? 1u : 0u);

		usize argIndex = 0;
		for (Expr* arg : node.args())
		{
			const Type* argType = arg->type();
			const Type* paramType = argIndex < paramTypes.size() ? paramTypes[argIndex] : nullptr;
			++argIndex;
			// F3.4: whether the argument travels as a two-word pair is the CALLEE's parameter's to
			// decide: a wide parameter takes two words (materializing a scalar or float source into a
			// pair, and truncating a wide argument to the pair's address), while a NARROW parameter
			// takes one word whatever the argument is - `int f(int); long long x; f(x);` truncates x
			// to its low word, and only a wide one marks the Param wide. When the callee is unknown
			// (an indirect call through a function pointer), the argument's own type is all there is,
			// so a wide argument is passed as its pair.
			bool wideArg = paramType ? paramType->isWideInteger() : (argType && argType->isWideInteger());
			if (isIndirectStruct(argType))
			{
				// By value, without a by-value register class: copy the argument into a slot of the
				// caller's own frame and pass that copy's address. The callee may write through it
				// freely - it is nobody else's object.
				IrValue source = lowerExpr(arg); // a struct expression IS its address
				IrValue copy = emitFrameAddr(loc, newStructTempSlot(argType->sizeInBytes()));
				emitMemoryCopy(loc, copy, source, argType->sizeInBytes(), argType->alignment());
				argValues.push_back(copy);
				argIsFloat.push_back(false);
				argIsWide.push_back(false);
				continue;
			}
			if (isStructType(argType))
			{
				// 1/2/4 bytes: the whole object fits one register, so load it and pass it like any
				// other integer argument.
				IrValue source = lowerExpr(arg);
				argValues.push_back(emitLoad(loc, source, irMemSizeForBytes(argType->sizeInBytes())));
				argIsFloat.push_back(false);
				argIsWide.push_back(false);
				continue;
			}
			// C converts an argument to the parameter's type as if by assignment, and here that is
			// not a formality: a narrow parameter whose value never reaches memory (the optimizer
			// keeps it in the register it arrived in) is narrowed nowhere else. `char f(char c)`
			// called with 300 has to see 44.
			IrValue value = lowerExpr(arg);
			if (paramType)
				value = convertForStore(loc, value, argType, paramType);
			argValues.push_back(value);
			argIsFloat.push_back(argType && argType->isFloat());
			argIsWide.push_back(wideArg);
		}

		// A name that resolves to a FUNCTION is a direct call; anything else - a variable holding a
		// pointer, a cast, an array element - is an address to be computed and jumped through. The
		// test is which of the two the name is, not whether the callee is a name at all: with
		// function pointers a variable can be the callee under a name too.
		auto* callee = namedCallee;
		bool isDirect = declared != _functionDecls.end();

		// Lowered here, after the arguments' own expressions and before the Param run below: codegen
		// requires the Params to sit immediately before the Call they belong to (it reads them back
		// by position), so nothing may be emitted between them.
		IrValue calleeAddress{};
		if (!isDirect)
			calleeAddress = lowerExpr(node.callee());

		for (usize i = 0; i < argValues.size(); ++i)
			emitVoid(loc, IrParamPayload{ argValues[i], argIsFloat[i], i >= fixedArgCount, argIsWide[i] });

		IrCallPayload payload;
		payload.hasResult = hasResult && !returnsStructIndirect;
		payload.isFloat = hasResult && resultType->isFloat();
		payload.hasWideResult = returnsWide;
		payload.callee = isDirect ? callee->name() : std::string_view{};
		payload.calleeValue = calleeAddress;
		payload.argCount = static_cast<u32>(argValues.size());
		if (payload.hasResult)
			payload.result = _currentFunction->newTemp();
		if (returnsWide)
			payload.resultHigh = _currentFunction->newTemp(); // the high word ret1 lands in

		emitVoid(loc, payload);

		if (returnsWide)
		{
			// The two returned words are in ret0/ret1; the CallExpr's value is the caller's temp
			// holding them - the same addressed pair every wide value is.
			emitStore(loc, wideResultAddr, IrMemSize::Word, payload.result);
			emitStore(loc, offsetAddress(loc, wideResultAddr, 4), IrMemSize::Word, payload.resultHigh);
			_lastValue = wideResultAddr;
			return;
		}

		if (returnsStructIndirect)
		{
			// The callee returns the same pointer it was handed, so the returned value is ignored
			// here in favour of the address this function already has - one fewer temp to keep
			// alive across the call, and it does not depend on the callee honouring that part of
			// the convention at all.
			_lastValue = hiddenDest;
			return;
		}
		if (returnsStructInRegister)
		{
			IrValue slotAddr = emitFrameAddr(loc, smallStructSlot);
			emitStore(loc, slotAddr, irMemSizeForBytes(resultType->sizeInBytes()), payload.result);
			_lastValue = slotAddr;
			return;
		}
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
				// lowerAddress()'s own Deref case computes *p's address as p's own value (or, when p
				// is itself an array, that array's address directly - matching real C's
				// `*arr == arr[0]`, see its header comment); lowerRValue() then either loads through
				// that address (an ordinary scalar result) or, if `*p` itself denotes an array
				// (`*matrix` where matrix: T[N][M]), decays to that same address with no load at all.
				_lastValue = lowerRValue(&node);
				return;

			case UnaryOp::Negate:
			{
				const Type* operandType = node.operand()->type();
				if (operandType && operandType->isWideInteger())
				{
					IrValue a = materializeWide(loc, lowerExpr(node.operand()), operandType);
					_lastValue = lowerWideNegate(loc, a, operandType->isVolatile());
					return;
				}
				_lastValue = emitUnOp(loc, IrUnOp::Neg, lowerExpr(node.operand()), operandType && operandType->isFloat());
				return;
			}

			case UnaryOp::BitwiseNot:
			{
				const Type* operandType = node.operand()->type();
				if (operandType && operandType->isWideInteger())
				{
					IrValue a = materializeWide(loc, lowerExpr(node.operand()), operandType);
					bool isVolatile = operandType->isVolatile();
					IrValue low = loadWideWord(loc, a, false, isVolatile);
					IrValue high = loadWideWord(loc, a, true, isVolatile);
					IrValue minusOne = emitConstInt(loc, -1);
					_lastValue = makeWideValue(loc, emitBinOp(loc, IrBinOp::Xor, low, minusOne, true),
						emitBinOp(loc, IrBinOp::Xor, high, minusOne, true));
					return;
				}
				_lastValue = emitUnOp(loc, IrUnOp::Not, lowerExpr(node.operand()));
				return;
			}

			case UnaryOp::LogicalNot:
				_lastValue = materializeBoolean(&node);
				return;

			case UnaryOp::PreIncrement:
			case UnaryOp::PreDecrement:
			case UnaryOp::PostIncrement:
			case UnaryOp::PostDecrement:
			{
				const Type* type = node.operand()->type();
				bool isIncrement = (node.op() == UnaryOp::PreIncrement || node.op() == UnaryOp::PostIncrement);
				if (type && type->isWideInteger())
				{
					IrValue addr = lowerAddress(node.operand());
					bool isVolatile = type->isVolatile();
					bool isPre = (node.op() == UnaryOp::PreIncrement || node.op() == UnaryOp::PreDecrement);
					// The object is read ONCE - a `volatile long long` `a++` implies a single read
					// (plus the write), so the two words are snapshotted into a temp and the
					// arithmetic works on that, not on the object again. The snapshot is a post
					// form's value; a pre form returns the newly computed one.
					IrValue snapshot = makeWideValue(loc, loadWideWord(loc, addr, false, isVolatile),
						loadWideWord(loc, addr, true, isVolatile));
					const Type* plain = type->isSigned() ? &Type::LongLong : &Type::ULongLong;
					IrValue newValue = lowerWideArithmetic(loc, isIncrement ? BinaryOp::Add : BinaryOp::Sub,
						plain, plain, &Type::Int, snapshot, emitConstInt(loc, 1));
					emitWideStore(loc, addr, newValue, plain, isVolatile);
					_lastValue = isPre ? newValue : snapshot;
					return;
				}
				bool isFloat = type && type->isFloat();
				IrValue addr = lowerAddress(node.operand());
				IrMemSize size = memSizeOf(type);
				IrValue oldValue = loadOfType(loc, addr, type);
				IrValue stepValue;
				if (isFloat)
					stepValue = emitConstFloat(loc, 1.0f);
				else
				{
					i64 step = (type && type->isPointer() && type->arrayElementType()) ? type->arrayElementType()->sizeInBytes() : 1;
					stepValue = emitConstInt(loc, step);
				}
				IrValue newValue = emitBinOp(loc, isIncrement ? IrBinOp::Add : IrBinOp::Sub, oldValue, stepValue, type && !type->isSigned(), isFloat);
				newValue = convertForStore(loc, newValue, isFloat ? type : &Type::Int, type);
				emitStore(loc, addr, size, newValue, isFloat, type && type->isVolatile());
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

		if (node.op() == BinaryOp::Comma)
		{
			// The left side is lowered for what it does; its value goes unread and the optimizer
			// drops whatever computed it that nothing else uses.
			lowerExpr(node.lhs());
			_lastValue = lowerExpr(node.rhs());
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
				const Type* lhsType = node.lhs()->type();
				const Type* rhsType = node.rhs()->type();
				// A float on either side makes the whole comparison a float one, with a wide operand
				// converted whole by toFloatIfNeeded(). It has to be checked before the wide case:
				// `wide < 1.5f` is a float comparison, not a 64-bit integer one.
				bool cmpIsFloat = (lhsType && lhsType->isFloat()) || (rhsType && rhsType->isFloat());
				if (cmpIsFloat)
				{
					lhs = toFloatIfNeeded(loc, lhs, lhsType);
					rhs = toFloatIfNeeded(loc, rhs, rhsType);
				}
				else if ((lhsType && lhsType->isWideInteger()) || (rhsType && rhsType->isWideInteger()))
				{
					// Both operands are promoted to the common 64-bit type first, so `long long`
					// against `int` compares the int's sign-extended pair, and the signedness of the
					// comparison comes from that common type (unsigned only if it is `unsigned long
					// long`).
					IrValue a = materializeWide(loc, lhs, lhsType);
					IrValue b = materializeWide(loc, rhs, rhsType);
					const Type* common = commonArithmeticType(lhsType, rhsType);
					bool isUnsigned = common && !common->isSigned();
					_lastValue = lowerWideCompare(loc, node.op(), a, b, isUnsigned);
					return;
				}

				IrCmpPayload payload;
				payload.result = _currentFunction->newTemp();
				payload.predicate = cmpPredicateFor(node.op());
				payload.isUnsigned = isUnsignedComparison(lhsType, rhsType);
				payload.isFloat = cmpIsFloat;
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
		const Type* targetType = node.target()->type();

		if (isStructType(targetType) && node.op() == AssignOp::Assign)
		{
			// `a = b` between two structs: a whole-object copy, not a scalar store (sema only allows
			// plain `=` here - a compound operator would have failed its arithmetic-type check). The
			// expression's own value is the destination's address, which keeps the struct convention
			// intact and makes `a = b = c` work: the outer assignment copies from the inner one's
			// destination, exactly as C's by-value chain does.
			IrValue source = lowerExpr(node.value());
			IrValue structDest = lowerAddress(node.target());
			emitMemoryCopy(loc, structDest, source, targetType->sizeInBytes(), targetType->alignment());
			_lastValue = structDest;
			return;
		}
		if (isStructType(targetType))
		{
			_diagnostics.error(DiagId::CompoundAssignToStruct, loc, "compound assignment is not valid for a struct type");
			_lastValue = IrValue{};
			return;
		}

		if (targetType && targetType->isWideInteger())
		{
			// A wide target is eight bytes, never a scalar store: `x = y` is a two-word copy (or an
			// extension of a scalar y), and `x op= y` computes the wide result first. The value of
			// the assignment is the destination's address, which keeps the wide convention intact
			// and makes `a = b = c` copy from the inner destination.
			if (node.op() == AssignOp::Assign)
			{
				// A wide source is copied as its own address (its volatility rides on the loads); a
				// scalar or float source is converted first - so the DESTINATION's qualifier never
				// governs the source's reads.
				const Type* valueType = node.value()->type();
				if (valueType && valueType->isWideInteger())
				{
					IrValue value = lowerExpr(node.value());
					IrValue destAddr = lowerAddress(node.target());
					emitWideStore(loc, destAddr, value, valueType, targetType->isVolatile());
					_lastValue = destAddr;
				}
				else
				{
					IrValue value = convertForStore(loc, lowerExpr(node.value()), valueType, targetType);
					IrValue destAddr = lowerAddress(node.target());
					emitWideStore(loc, destAddr, value, targetType, targetType->isVolatile());
					_lastValue = destAddr;
				}
				return;
			}

			IrValue addr = lowerAddress(node.target());
			IrValue rhs = lowerExpr(node.value());
			BinaryOp binaryOp = binaryOpForCompoundAssign(node.op());
			const Type* promotedType = commonArithmeticType(targetType, node.value()->type());
			IrValue value = lowerArithmetic(loc, binaryOp, promotedType, targetType, node.value()->type(), addr, rhs);
			// A float promoted type (`long long x; x += 1.5f;`) converts back down to the wide
			// target, exactly as `x = x + 1.5f;` does - otherwise the store would refuse it.
			if (promotedType && promotedType->isFloat())
			{
				IrValue converted = convertForStore(loc, value, promotedType, targetType);
				emitWideStore(loc, addr, converted, targetType, targetType->isVolatile());
			}
			else
			{
				emitWideStore(loc, addr, value, promotedType, targetType->isVolatile());
			}
			_lastValue = value;
			return;
		}

		IrMemSize size = memSizeOf(targetType);
		bool targetIsFloat = targetType && targetType->isFloat();

		if (node.op() == AssignOp::Assign)
		{
			// The value BEFORE the target's address, deliberately. C leaves the two operands of an
			// assignment unsequenced with respect to each other, so either order conforms - and this
			// one puts the address computation immediately before the Store that consumes it, which
			// is the shape libs/codegen's address folding can absorb into one `str [base + index]`
			// (see codegen.h). The reverse order works just as well and costs an extra `add` per
			// element written, which is the whole difference the ISA's indexed stores exist to make.
			IrValue value = convertForStore(loc, lowerExpr(node.value()), node.value()->type(), targetType);
			IrValue destAddr = lowerAddress(node.target());
			emitStore(loc, destAddr, size, value, targetIsFloat, targetType && targetType->isVolatile());
			_lastValue = value;
			return;
		}

		// A compound operator reads the target before it writes it, so its address really does have
		// to come first - `x += y` is `x = x + y` with x evaluated once.
		IrValue addr = lowerAddress(node.target());
		IrValue oldValue = loadOfType(loc, addr, targetType);
		IrValue rhs = lowerExpr(node.value());
		BinaryOp binaryOp = binaryOpForCompoundAssign(node.op());
		// `x op= y` means `x = x op y` by definition, so this must agree with what the equivalent
		// BinaryExpr would compute (its own node.type() is already sema's promoted result type) -
		// not the raw, unpromoted target type, which for a narrow unsigned target (e.g.
		// `unsigned char x; x %= someInt;`) would pick the wrong signedness for the operation (and,
		// for `>>=`, the wrong Shr/Sar) versus `x = x % someInt;`. Harmless to compute even when
		// targetType is a pointer (`p += i`): lowerArithmetic()'s pointer branches never read this
		// parameter, only the plain-arithmetic fallback does.
		const Type* promotedType = commonArithmeticType(targetType, node.value()->type());
		IrValue value = lowerArithmetic(loc, binaryOp, promotedType, targetType, node.value()->type(), oldValue, rhs);
		// `x op= y` narrows the promoted result back to x's own (possibly non-float) storage -
		// e.g. `int x; x += 1.5f;` promotes to float for the add, then truncates back to store.
		value = convertForStore(loc, value, promotedType, targetType);

		emitStore(loc, addr, size, value, targetIsFloat, targetType && targetType->isVolatile());
		_lastValue = value;
	}

	void IrBuilder::visit(ast::IndexExpr& node)
	{
		// lowerRValue() decays a row of a multi-dimensional array (`matrix[i]` where matrix is
		// T[N][M], itself typed T[M]) to that row's own address instead of loading through it -
		// see its header comment.
		_lastValue = lowerRValue(&node);
	}

	void IrBuilder::visit(ast::MemberExpr& node)
	{
		// lowerRValue() decays an array-typed struct field (`s.arr`) to its own address instead of
		// loading through it - see its header comment.
		_lastValue = lowerRValue(&node);
	}

	void IrBuilder::visit(ast::CastExpr& node)
	{
		// An int<->int (or pointer) cast needs no real instruction: it is realized only through the
		// memSize a later Load/Store picks based on the AST's own annotated type (an `(char)someInt`
		// truncates for free the next time it is stored/loaded as a byte). Crossing the float/int
		// line is a genuine runtime operation with no such free lunch - see IrUnOp::IntToFloat/
		// FloatToInt's header comment (ir_instr.h) - so that direction alone gets a real conversion
		// here, via the same toFloatIfNeeded()/convertForStore() helpers a plain assignment's mixed
		// arithmetic already uses.
		_lastValue = convertForStore(node.location(), lowerExpr(node.operand()), node.operand()->type(), node.type());
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

	void IrBuilder::visit(ast::AlignofExpr& node)
	{
		_lastValue = emitConstInt(node.location(), node.argumentType() ? static_cast<i64>(node.argumentType()->alignment()) : 1);
	}

	void IrBuilder::visit(ast::MachineOpExpr& node)
	{
		// One instruction, no operands, no result - and no value for the enclosing expression, since
		// all three are void. Nothing else to lower.
		IrMachineOpPayload payload;
		payload.op = node.op();
		emitVoid(node.location(), payload);
		_lastValue = IrValue{};
	}

	void IrBuilder::visit(ast::BuiltinExpr& node)
	{
		// One instruction with a result. The operands are lowered in source order; no conversion is
		// needed between the argument's type and the instruction's, because a narrow integer value
		// is already held sign- or zero-extended to a word (ir_instr.h's own invariant), which is
		// exactly the integer promotion the machine instruction would assume.
		support::SourceLocation loc = node.location();
		std::span<ast::Expr* const> args = node.args();

		// The two compiler builtins that are not a machine instruction lower away here:
		// `__builtin_expect(a, hint)` yields `a` (the hint is a branch-prediction promise with no
		// code of its own), and `__builtin_constant_p(e)` is answered at compile time by the
		// constant evaluator and does not evaluate `e` at all.
		if (node.builtin() == ast::Builtin::Expect)
		{
			IrValue value = args.empty() ? IrValue{} : lowerExpr(args[0]);
			if (args.size() > 1)
				lowerExpr(args[1]); // evaluated for its side effects, if any - its value is ignored
			_lastValue = value;
			return;
		}
		if (node.builtin() == ast::Builtin::ConstantP)
		{
			bool isConstant = !args.empty() && foldConstant(args[0]).has_value();
			_lastValue = emitConstInt(loc, isConstant ? 1 : 0);
			return;
		}

		// Every remaining builtin is one 32-bit machine instruction (or an overflow expansion over
		// one), so a 64-bit operand has no encoding - refuse it rather than use the address's low
		// word. `__builtin_expect` above is the exception, since it just forwards its operand.
		for (ast::Expr* argument : args)
			if (argument->type() && argument->type()->isWideInteger())
				rejectWideFeature(loc, "a 64-bit operand to a builtin");

		// The overflow builtins expand to the operation, a store, and an overflow test:
		//   add.u:  sum < a                       add.s:  ((a^sum) & (b^sum)) < 0
		//   sub.u:  a < b                         sub.s:  ((a^b) & (a^diff)) < 0
		//   mul.u:  mulhu(a,b) != 0               mul.s:  mulhs(a,b) != (product >>s 31)
		if (node.builtin() == ast::Builtin::AddOverflow || node.builtin() == ast::Builtin::SubOverflow ||
			node.builtin() == ast::Builtin::MulOverflow)
		{
			IrValue a = lowerExpr(args[0]);
			IrValue b = lowerExpr(args[1]);
			// The third operand is the pointer VALUE to store through, not an lvalue to take the
			// address of: `&r` lowers to the address, a plain `int* p` to the pointer it holds.
			IrValue destination = lowerExpr(args[2]);

			// C's usual arithmetic conversions: the operation is unsigned if EITHER operand is, so
			// the overflow test does not depend on which operand came first.
			const ast::Type* lhsType = args[0] ? args[0]->type() : nullptr;
			const ast::Type* rhsType = args[1] ? args[1]->type() : nullptr;
			bool isUnsigned = (lhsType && !lhsType->isSigned()) || (rhsType && !rhsType->isSigned());
			ast::Builtin kind = node.builtin();
			IrBinOp op = kind == ast::Builtin::AddOverflow ? IrBinOp::Add
				: kind == ast::Builtin::SubOverflow ? IrBinOp::Sub
				: IrBinOp::Mul;
			IrValue result = emitBinOp(loc, op, a, b, isUnsigned);
			if (destination.isValid())
				emitStore(loc, destination, IrMemSize::Word, result);

			auto lessThanZero = [&](IrValue value) { return emitCmp(loc, IrCmpPredicate::Lt, value, emitConstInt(loc, 0), false); };

			IrValue overflow;
			if (kind == ast::Builtin::MulOverflow)
			{
				if (isUnsigned)
					overflow = emitCmp(loc, IrCmpPredicate::Ne, emitBuiltin(loc, ast::Builtin::MulhUnsigned, a, b),
						emitConstInt(loc, 0), true);
				else
					overflow = emitCmp(loc, IrCmpPredicate::Ne, emitBuiltin(loc, ast::Builtin::MulhSigned, a, b),
						emitBinOp(loc, IrBinOp::Sar, result, emitConstInt(loc, 31), false), false);
			}
			else if (kind == ast::Builtin::AddOverflow)
			{
				if (isUnsigned)
					overflow = emitCmp(loc, IrCmpPredicate::Lt, result, a, true);
				else
					overflow = lessThanZero(emitBinOp(loc, IrBinOp::And,
						emitBinOp(loc, IrBinOp::Xor, a, result, false),
						emitBinOp(loc, IrBinOp::Xor, b, result, false), false));
			}
			else // SubOverflow
			{
				if (isUnsigned)
					overflow = emitCmp(loc, IrCmpPredicate::Lt, a, b, true);
				else
					overflow = lessThanZero(emitBinOp(loc, IrBinOp::And,
						emitBinOp(loc, IrBinOp::Xor, a, b, false),
						emitBinOp(loc, IrBinOp::Xor, a, result, false), false));
			}

			_lastValue = overflow; // a comparison already yields 0 or 1, which is the bool result
			return;
		}

		IrBuiltinPayload payload;
		payload.builtin = node.builtin();
		payload.result = _currentFunction->newTemp();
		if (!args.empty())
			payload.a = lowerExpr(args[0]);
		if (args.size() > 1)
			payload.b = lowerExpr(args[1]);

		emitVoid(loc, payload);
		_lastValue = payload.result;
	}

	void IrBuilder::visit(ast::VaExpr& node)
	{
		using ast::VaOp;
		support::SourceLocation loc = node.location();

		// A __builtin_va_list is an ordinary `char*` lvalue (see the parser), so its ADDRESS is what
		// every form below reads and writes through - the cursor has to survive the call that
		// advanced it.
		IrValue listAddr = lowerAddress(node.list());

		switch (node.op())
		{
			case VaOp::Start:
			{
				// The one step the back end has to resolve: where this function's own tail begins.
				IrValue start = _currentFunction->newTemp();
				emitVoid(loc, IrVaStartPayload{ start });
				emitStore(loc, listAddr, IrMemSize::Word, start);
				_lastValue = IrValue{};
				return;
			}

			case VaOp::Arg:
			{
				// Read through the cursor, then advance it one word - every variadic argument
				// occupies exactly one outgoing stack word (docs/09-Variadic-Convention.md), which
				// is what sema's "must be a 4-byte scalar" check on the type guarantees.
				const Type* argumentType = node.argumentType();
				bool isFloat = argumentType && argumentType->isFloat();
				IrValue cursor = emitLoad(loc, listAddr, IrMemSize::Word);
				IrValue value = emitLoad(loc, cursor, IrMemSize::Word, isFloat);
				IrValue step = emitConstInt(loc, 4);
				IrValue advanced = emitBinOp(loc, IrBinOp::Add, cursor, step, false, false);
				emitStore(loc, listAddr, IrMemSize::Word, advanced);
				_lastValue = value;
				return;
			}

			case VaOp::Copy:
			{
				// Both cursors are plain words; copying one is copying the other's current position.
				IrValue source = lowerExpr(node.second());
				emitStore(loc, listAddr, IrMemSize::Word, source);
				_lastValue = IrValue{};
				return;
			}

			case VaOp::End:
				// Nothing to release: a __builtin_va_list owns no resource, it is a cursor into a frame that
				// the caller is going to reclaim anyway.
				_lastValue = IrValue{};
				return;
		}
	}

	void IrBuilder::visit(ast::GenericSelectionExpr& node)
	{
		// _Generic's controlling expression and every non-selected association are a non-evaluated
		// context, exactly like sizeof's operand (see visit(SizeofExpr&) above): sema already picked
		// the winning association (node.selectedIndex()), so only that one expr is ever lowered here.
		// node.controlling() itself is NEVER lowered - a controlling expression with a side effect,
		// e.g. `_Generic(x++, ...)`, never actually increments x, per the C11 rule sema's own comment
		// on GenericSelectionExpr explains.
		Expr* selected = node.selectedExpr();
		_lastValue = selected ? lowerExpr(selected) : IrValue{};
	}

	bool IrBuilder::tryLowerSelectIdiom(const ast::TernaryExpr& node)
	{
		if (!_options.minMaxIdioms)
			return false;

		auto* cond = dynamic_cast<ast::BinaryExpr*>(node.cond());
		if (!cond)
			return false;

		// Integer min/max only. A float `a < b ? a : b` is NOT fmin/fmax: the select returns the
		// else arm when the test is false (a NaN on either side picks the else arm), whereas fmin
		// returns the non-NaN operand - a difference this phase does not model. `abs` has the same
		// -0.0 caveat, so it is integer too.
		const ast::Type* resultType = node.type();
		if (!resultType || resultType->isFloat() || resultType->isPointer() || resultType->isArray() ||
			resultType->isWideInteger())
			return false;

		Expr* lhs = cond->lhs();
		Expr* rhs = cond->rhs();
		Expr* thenE = node.thenExpr();
		Expr* elseE = node.elseExpr();
		if (!lhs || !rhs || !thenE || !elseE)
			return false;

		const BinaryOp op = cond->op();
		const bool relational = op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;

		// abs: `x < 0 ? -x : x` and its mirrors (zero on either side, non-strict forms included).
		if (relational && resultType->isSigned())
		{
			auto isZero = [](const Expr* e)
			{
				const auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(e);
				return literal && literal->value() == 0 && !literal->isUnsigned();
			};
			auto isNegationOf = [](const Expr* neg, const Expr* value)
			{
				const auto* unary = dynamic_cast<const ast::UnaryExpr*>(neg);
				return unary && unary->op() == UnaryOp::Negate && exprsEquivalent(unary->operand(), value);
			};

			// Exactly one side is the literal 0; the other is the value `x`. The two arms must be
			// `x` and `-x`, and the arm chosen when the test holds for a negative `x` must be `-x`.
			Expr* x = nullptr;
			bool zeroOnLeft = false;
			if (isZero(rhs)) { x = lhs; zeroOnLeft = false; }
			else if (isZero(lhs)) { x = rhs; zeroOnLeft = true; }

			if (x)
			{
				const bool thenIsX = exprsEquivalent(thenE, x);
				const bool thenIsNegX = isNegationOf(thenE, x);
				const bool elseIsX = exprsEquivalent(elseE, x);
				const bool elseIsNegX = isNegationOf(elseE, x);
				const bool armsAreXAndNegX = (thenIsX && elseIsNegX) || (thenIsNegX && elseIsX);
				const bool trueMeansNegative = zeroOnLeft
					? (op == BinaryOp::Gt || op == BinaryOp::Ge)   // 0 > x  /  0 >= x
					: (op == BinaryOp::Lt || op == BinaryOp::Le);  // x < 0  /  x <= 0

				if (armsAreXAndNegX && thenIsNegX == trueMeansNegative)
				{
					support::SourceLocation loc = node.location();
					IrValue value = convertForStore(loc, lowerExpr(x), x->type(), resultType);
					_lastValue = emitBuiltin(loc, ast::Builtin::Abs, value);
					return true;
				}
			}
		}

		// min/max: both arms are exactly the comparison's two operands. `a == b ? a : b` also has
		// that shape, so the operator has to be relational before either is considered.
		if (!relational)
			return false;
		const bool thenIsLhs = exprsEquivalent(thenE, lhs);
		const bool thenIsRhs = exprsEquivalent(thenE, rhs);
		const bool elseIsLhs = exprsEquivalent(elseE, lhs);
		const bool elseIsRhs = exprsEquivalent(elseE, rhs);
		if (!((thenIsLhs && elseIsRhs) || (thenIsRhs && elseIsLhs)))
			return false;

		// True selects the lhs exactly when the test is `lhs < rhs` (or <=); the lhs is then the
		// smaller of the two, i.e. min - and the other way round for max.
		const bool lessWhenTrue = op == BinaryOp::Lt || op == BinaryOp::Le;
		const bool isMin = thenIsLhs == lessWhenTrue;
		ast::Builtin kind = resultType->isSigned()
			? (isMin ? ast::Builtin::MinSigned : ast::Builtin::MaxSigned)
			: (isMin ? ast::Builtin::MinUnsigned : ast::Builtin::MaxUnsigned);

		support::SourceLocation loc = node.location();
		IrValue a = convertForStore(loc, lowerExpr(lhs), lhs->type(), resultType);
		IrValue b = convertForStore(loc, lowerExpr(rhs), rhs->type(), resultType);
		_lastValue = emitBuiltin(loc, kind, a, b);
		return true;
	}

	void IrBuilder::visit(ast::TernaryExpr& node)
	{
		if (tryLowerSelectIdiom(node))
			return;

		support::SourceLocation loc = node.location();
		BasicBlock& thenBlock = _currentFunction->createBlock();
		BasicBlock& elseBlock = _currentFunction->createBlock();
		BasicBlock& mergeBlock = _currentFunction->createBlock();

		// A wide result needs an 8-byte home, and its address has to be materialized in the
		// pre-branch block: emitting the FrameAddr after lowerCondition() below would append it to
		// the already-terminated block, i.e. an unreachable one, and both arms would then read a
		// temp that never runs.
		IrValue wideResultAddr{};
		if (node.type() && node.type()->isWideInteger())
			wideResultAddr = emitFrameAddr(loc, newStructTempSlot(8));

		lowerCondition(node.cond(), &thenBlock, &elseBlock);

		// A wide result is an 8-byte object, so both arms copy their two words into one temp rather
		// than `Copy` a single value into a register.
		if (node.type() && node.type()->isWideInteger())
		{
			IrValue resultAddr = wideResultAddr;
			// One arm: a wide expression is its own address (copied, its volatility on the loads); a
			// scalar or float one is converted to the wide result type first.
			auto emitWideArm = [&](support::SourceLocation armLoc, IrValue dest, ast::Expr* arm, const Type* resultType)
			{
				const Type* armType = arm->type();
				if (armType && armType->isWideInteger())
					emitWideStore(armLoc, dest, lowerExpr(arm), armType, false);
				else
					emitWideStore(armLoc, dest, convertForStore(armLoc, lowerExpr(arm), armType, resultType), resultType, false);
			};

			_currentBlock = &thenBlock;
			emitWideArm(loc, resultAddr, node.thenExpr(), node.type());
			if (!_currentBlock->isTerminated())
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

			_currentBlock = &elseBlock;
			emitWideArm(loc, resultAddr, node.elseExpr(), node.type());
			if (!_currentBlock->isTerminated())
				emitVoid(loc, IrJumpPayload{ &mergeBlock });

			_currentBlock = &mergeBlock;
			_lastValue = resultAddr;
			return;
		}

		IrValue result = _currentFunction->newTemp();
		bool resultIsFloat = node.type() && node.type()->isFloat();

		_currentBlock = &thenBlock;
		emitCopyInto(loc, result, convertForStore(loc, lowerExpr(node.thenExpr()), node.thenExpr()->type(), node.type()), resultIsFloat);
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &elseBlock;
		emitCopyInto(loc, result, convertForStore(loc, lowerExpr(node.elseExpr()), node.elseExpr()->type(), node.type()), resultIsFloat);
		if (!_currentBlock->isTerminated())
			emitVoid(loc, IrJumpPayload{ &mergeBlock });

		_currentBlock = &mergeBlock;
		_lastValue = result;
	}

	void IrBuilder::visit(ast::InitListExpr&)
	{
		// Unreachable on sema-checked input: a brace list only ever appears as a declarator's
		// initializer, and that path goes through lowerInitializerInto(), which reads the node
		// directly instead of dispatching to it (sema reports any other position - see
		// Sema::visit(InitListExpr&)). Produces no value at all rather than a plausible-looking
		// wrong one, so a future caller that lowers an initializer the wrong way fails visibly.
		_lastValue = IrValue{};
	}

	// ---- statements -----------------------------------------------------------------------------

	void IrBuilder::visit(ast::EmptyStmt&) {}

	void IrBuilder::visit(ast::ExprStmt& node)
	{
		lowerExpr(node.expr());
	}

	void IrBuilder::visit(ast::DeclStmt& node)
	{
		for (ast::Decl* decl : node.decls())
			decl->accept(*this);
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
		support::SourceLocation loc = node.location();
		if (!node.value())
		{
			emitVoid(loc, IrReturnPayload{ false, false, IrValue{}, IrValue{}, false });
			return;
		}

		const Type* returnType = _currentFunction->returnType();

		if (returnType && returnType->isWideInteger())
		{
			// F3.4: a 64-bit return goes back in two words (ret0 low, ret1 high). The value is the
			// address of the pair, so load its two words into an IrWideReturnPayload. A `volatile`
			// returned lvalue has no other reader, so these two loads are the only reads of it and
			// must stay observable - the source's qualifier governs them, as every other wide path.
			IrValue value = convertForStore(loc, lowerExpr(node.value()), node.value()->type(), returnType);
			bool sourceVolatile = node.value()->type() && node.value()->type()->isVolatile();
			IrValue low = loadWideWord(loc, value, false, sourceVolatile);
			IrValue high = loadWideWord(loc, value, true, sourceVolatile);
			emitVoid(loc, IrReturnPayload{ true, false, low, high, true });
			return;
		}

		if (_hiddenReturnSlot)
		{
			// The caller handed us where to put the result (ir_builder.h's struct convention):
			// copy it there, then return that same pointer in ret0 so a caller that prefers to read
			// the result back out of the register still can.
			IrValue source = lowerExpr(node.value()); // a struct expression IS its address
			IrValue dest = emitLoad(loc, emitFrameAddr(loc, *_hiddenReturnSlot), IrMemSize::Word);
			emitMemoryCopy(loc, dest, source, returnType->sizeInBytes(), returnType->alignment());
			emitVoid(loc, IrReturnPayload{ true, false, dest, IrValue{}, false });
			return;
		}

		if (isStructType(returnType))
		{
			// 1/2/4 bytes - the whole struct goes back in ret0 as one byte/half/word.
			IrValue source = lowerExpr(node.value());
			IrValue value = emitLoad(loc, source, irMemSizeForBytes(returnType->sizeInBytes()));
			emitVoid(loc, IrReturnPayload{ true, false, value, IrValue{}, false });
			return;
		}

		IrValue value = convertForStore(loc, lowerExpr(node.value()), node.value()->type(), returnType);
		emitVoid(loc, IrReturnPayload{ true, returnType && returnType->isFloat(), value, IrValue{}, false });
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

	// ---- switch dispatch: jump table / binary search ---------------------------------------------
	//
	// A dense case set is worth a table: the dispatch becomes a constant handful of instructions
	// instead of one comparison per case, at the price of 4 bytes per entry in `.rodata`. A sparse
	// but large one still beats the chain with a balanced tree of comparisons (log2 N), which needs
	// no table at all. A small one keeps the plain chain - see docs/13-Switch-Jump-Table-Plan.md.
	namespace
	{
		constexpr usize kSwitchTableMinCases = 5;    // below this the chain is already short enough
		constexpr usize kSwitchTableMaxEntries = 256; // 4 * 256 = 1 KiB of `.rodata` ceiling
		constexpr double kSwitchTableMinDensity = 0.5; // how full [low, high] must be to justify holes
		constexpr usize kSwitchTreeMinCases = 8;     // below this a tree's larger code is not worth it

		// A case value in the controlling expression's own 32-bit representation. Sema folds a case
		// constant in ITS type rather than converting it to the switch's promoted type, so
		// `case -1:` in a `switch (unsigned)` reaches IrBuilder as i64 -1 while the discriminant is
		// 0xFFFFFFFF - equal as 32-bit bit patterns, but not as i64. Normalizing first is what makes
		// the tree's ordering tests agree with the runtime comparison (and keeps the table's
		// `high - low + 1` inside i64). The chain never needed this: equality is sign-agnostic.
		i64 normalizeSwitchValue(i64 value, bool isUnsigned) noexcept
		{
			return isUnsigned ? static_cast<i64>(static_cast<u32>(value))
							  : static_cast<i64>(static_cast<i32>(value));
		}
	}

	bool IrBuilder::emitSwitchDispatch(support::SourceLocation loc, IrValue condValue, bool isUnsigned,
		const std::vector<std::pair<i64, BasicBlock*>>& cases, BasicBlock* defaultBlock, BasicBlock& exitBlock)
	{
		if (!_options.jumpTables || cases.size() < kSwitchTableMinCases)
			return false;

		std::vector<std::pair<i64, BasicBlock*>> normalized;
		normalized.reserve(cases.size());
		for (const auto& [value, block] : cases)
			normalized.emplace_back(normalizeSwitchValue(value, isUnsigned), block);

		i64 low = normalized.front().first;
		i64 high = normalized.front().first;
		for (const auto& [value, block] : normalized)
		{
			low = std::min(low, value);
			high = std::max(high, value);
		}

		// Both bounds are 32-bit now, so the span is at most 2^32 and cannot overflow i64; a
		// non-positive result would still mean it wrapped, which no table can cover.
		i64 span = high - low + 1;
		bool tableFits = span > 0 && span <= static_cast<i64>(kSwitchTableMaxEntries) &&
			static_cast<double>(normalized.size()) / static_cast<double>(span) >= kSwitchTableMinDensity;

		if (tableFits)
		{
			BasicBlock* fallback = defaultBlock ? defaultBlock : &exitBlock;
			auto** targets = static_cast<BasicBlock**>(_arena.allocate(static_cast<usize>(span) * sizeof(BasicBlock*)));
			for (i64 i = 0; i < span; ++i)
				targets[i] = fallback; // a hole in the range goes to `default`, so one bounds check suffices
			for (const auto& [value, block] : normalized)
				targets[value - low] = block;
			emitVoid(loc, IrTableJumpPayload{ condValue, targets, static_cast<u32>(span), low, fallback });
			return true;
		}

		if (normalized.size() < kSwitchTreeMinCases)
			return false; // sparse AND small: the chain is the best of the three

		std::vector<std::pair<i64, BasicBlock*>> sorted = normalized;
		std::sort(sorted.begin(), sorted.end(),
			[](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
		emitSwitchTree(loc, condValue, isUnsigned, sorted, 0, sorted.size(), defaultBlock ? defaultBlock : &exitBlock);
		return true;
	}

	void IrBuilder::emitSwitchTree(support::SourceLocation loc, IrValue condValue, bool isUnsigned,
		const std::vector<std::pair<i64, BasicBlock*>>& cases, usize begin, usize end, BasicBlock* fallback)
	{
		if (begin >= end)
		{
			emitVoid(loc, IrJumpPayload{ fallback });
			return;
		}
		if (end - begin == 1)
		{
			IrValue value = emitConstInt(loc, cases[begin].first);
			emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Eq, isUnsigned, false, condValue, value, cases[begin].second, fallback });
			return;
		}

		usize mid = begin + (end - begin) / 2;
		BasicBlock& lower = _currentFunction->createBlock();
		BasicBlock& upper = _currentFunction->createBlock();
		IrValue pivot = emitConstInt(loc, cases[mid].first);
		// `cond < pivot` -> strictly-lower half; anything else (including `== pivot`) -> the upper
		// half, which still holds `cases[mid]` and tests it for equality on the way down.
		emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Lt, isUnsigned, false, condValue, pivot, &lower, &upper });
		switchToBlock(lower, loc);
		emitSwitchTree(loc, condValue, isUnsigned, cases, begin, mid, fallback);
		switchToBlock(upper, loc);
		emitSwitchTree(loc, condValue, isUnsigned, cases, mid, end, fallback);
	}

	void IrBuilder::visit(ast::SwitchStmt& node)
	{
		support::SourceLocation loc = node.location();

		// A 64-bit discriminant is F9 (a `switch` over a wide type): the dispatch below compares one
		// word, so refuse it rather than compare the value's address.
		if (node.cond()->type() && node.cond()->type()->isWideInteger())
			rejectWideFeature(loc, "a 64-bit switch condition");

		IrValue condValue = lowerExpr(node.cond());

		std::vector<std::pair<i64, BasicBlock*>> cases;
		BasicBlock* defaultBlock = nullptr;
		collectSwitchCases(node.body(), cases, defaultBlock);

		BasicBlock& exitBlock = _currentFunction->createBlock();

		const Type* condType = node.cond()->type();
		bool isUnsigned = condType && !condType->isSigned();

		if (!emitSwitchDispatch(loc, condValue, isUnsigned, cases, defaultBlock, exitBlock))
		{
			// Dispatch chain: one Cmp+CondJump per case value, in source order, falling through to
			// the next comparison on a mismatch. This is the -O0 shape (options().jumpTables off)
			// and the fallback for a switch too small or too sparse for either smarter dispatch.
			for (const auto& [value, block] : cases)
			{
				IrValue constValue = emitConstInt(loc, value);
				BasicBlock& nextTest = _currentFunction->createBlock();
				emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Eq, false, false, condValue, constValue, block, &nextTest });
				_currentBlock = &nextTest;
			}
			emitVoid(loc, IrJumpPayload{ defaultBlock ? defaultBlock : &exitBlock });
		}

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
			{
				// `case low ... high:` expands to one dispatch entry per value, all sharing this
				// case's block. sema has already checked the range is non-empty and within the
				// expansion cap, so the loop is bounded; `high >= low` is re-checked defensively.
				i64 low = *value;
				i64 high = low;
				if (caseStmt->upper())
				{
					if (std::optional<i64> upper = foldConstant(caseStmt->upper()); upper && *upper >= low)
						high = *upper;
				}
				// Iterate by offset: `high` can be INT64_MAX, where `++value` would overflow.
				u64 span = static_cast<u64>(high) - static_cast<u64>(low);
				for (u64 offset = 0; offset <= span; ++offset)
					cases.emplace_back(low + static_cast<i64>(offset), &block);
			}
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

		if (node.storageClass() == ast::StorageClass::Static)
		{
			// A `static` local has to outlive the call, so it cannot be a frame slot: it becomes a
			// file-scope variable under a name carrying its function's, and every reference to it
			// lowers to a GlobalAddr like any other global. Its initializer runs once, at load time,
			// which is why codegen reads it from the VarDecl rather than IrBuilder emitting stores -
			// the same arrangement file-scope variables already use.
			// `f__count`, not `f.count`: a '.' is not an identifier character in CASM, and a leading
			// one would make it a local label instead of a file-scope symbol. The counter suffix is
			// for the case two disjoint blocks of one function each declare `static int count;` -
			// two different objects that would otherwise want the same name.
			std::string base = std::format("{}__{}", _currentFunction->name(), node.name());
			std::string candidate = base;
			for (u32 suffix = 2; !_staticLocalNames.insert(candidate).second; ++suffix)
				candidate = std::format("{}_{}", base, suffix);
			std::string_view uniqueName = internLabel(candidate);
			_module.addStaticLocal(uniqueName, &node);

			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::Global;
			symbol.globalName = uniqueName;
			declareSymbol(node.name(), symbol);
			return;
		}

		if (node.storageClass() == ast::StorageClass::Extern)
		{
			// `extern int x;` inside a block names a file-scope variable defined elsewhere. It
			// declares nothing of its own, so it maps straight onto the global of the same name.
			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::Global;
			symbol.globalName = node.name();
			declareSymbol(node.name(), symbol);
			return;
		}

		LocalSymbol symbol;
		symbol.kind = LocalSymbolKind::Local;
		symbol.localSlot = newLocalSlotFor(node.type() ? node.type()->sizeInBytes() : 4u, node.type() && node.type()->isFloat(),
			node.type() && node.type()->isVolatile(), node.storageClass() == ast::StorageClass::Register,
			node.type() && node.type()->isRestrict());
		declareSymbol(node.name(), symbol);

		if (!node.initializer())
			return;

		const Type* type = node.type();
		if (type && type->isWideInteger())
		{
			// A wide local is eight bytes; its initializer (a plain expression or a brace list) is
			// written as the two words by the same path a struct's field goes through.
			lowerInitializerInto(node.location(), emitFrameAddr(node.location(), symbol.localSlot), 0, type, node.initializer());
			return;
		}
		bool isAggregateInitializer = dynamic_cast<ast::InitListExpr*>(node.initializer()) != nullptr ||
			isStructType(type) ||
			(type && type->isArray()); // `char s[8] = "hola"` is the only non-list form an array takes
		if (isAggregateInitializer)
		{
			// Everything a single scalar store cannot express: a brace list, a string literal
			// filling a char array, or a whole-struct copy. lowerInitializerInto() also zero-fills
			// whatever the initializer does not reach, so the object is fully defined afterwards.
			lowerInitializerInto(node.location(), emitFrameAddr(node.location(), symbol.localSlot), 0, type, node.initializer());
			return;
		}

		IrValue value = convertForStore(node.location(), lowerExpr(node.initializer()), node.initializer()->type(), type);
		IrValue addr = emitFrameAddr(node.location(), symbol.localSlot);
		emitStore(node.location(), addr, memSizeOf(type), value, type && type->isFloat(), type && type->isVolatile());
	}

	void IrBuilder::visit(ast::FunctionDecl& node)
	{
		// Purity is recorded for a prototype too: the module lowers no body for one, but a call to
		// it is still a call to a pure function, and dead-code elimination reads the names from the
		// module rather than from the function list for exactly that reason.
		if (node.isPure())
			_module.addPureFunction(node.name());
		if (!node.isDefinition())
			return; // a prototype has nothing to lower - see the header comment

		IrFunction& function = _module.addFunction(node.name(), node.returnType());
		// Two facts the optimizer needs and cannot see in the body: whether another object may call
		// this (so unused-function elimination must keep it) and whether the program asked for it to
		// be inlined (so the inliner's size limit gives way).
		function.setLinkage(node.hasExternalLinkage(), node.isInline());
		// A third such fact: the inliner must not splice a variadic body into another frame, and
		// codegen must give one a frame pointer to read its argument tail through.
		function.setVariadic(node.isVariadic());
		function.setInterruptHandler(node.isInterruptHandler());
		// And the `__attribute__`s that steer the optimizer: noinline/always_inline for the inliner,
		// pure/const for dead-code elimination (docs/14, F11).
		function.setFunctionAttributes(node.isNoInline(), node.isAlwaysInline(), node.isPure());
		_currentFunction = &function;

		// A struct returned through memory takes a hidden first parameter holding its destination
		// (ir_builder.h's struct convention), so every visible parameter shifts one slot along -
		// the architecture plan's own "argumentos visibles corridos uno" in the ABI table.
		bool returnsStructIndirect = isIndirectStruct(node.returnType());
		_hiddenReturnSlot = returnsStructIndirect ? std::optional<u32>(0u) : std::nullopt;

		std::vector<IrLocalSlot> paramSlots;
		paramSlots.reserve(node.params().size() + 1);
		if (returnsStructIndirect)
			paramSlots.push_back(IrLocalSlot{ 4u, false }); // the hidden destination pointer
		for (const Param& param : node.params())
		{
			// A by-value struct too big for a register arrives as a POINTER to the caller's own
			// copy, so its slot holds four bytes whatever the struct's own size is. The pointer is
			// the compiler's own, never the parameter itself, so no qualifier of the parameter's
			// applies to it.
			if (isIndirectStruct(param.type))
				paramSlots.push_back(IrLocalSlot{ 4u, false });
			else
				// `isVolatile` belongs here for the same reason it does on an ordinary local: it is
				// what keeps ValuePlacement from giving the parameter a register and no memory home
				// at all (value_placement.cpp's own guard reads exactly this flag). Without it a
				// `volatile int` parameter compiled to pure register moves - the accesses carried
				// the mark, so the optimizer left them alone, and then the back end deleted the
				// object they were accesses TO.
				paramSlots.push_back(IrLocalSlot{ param.type ? param.type->sizeInBytes() : 4u,
					param.type && param.type->isFloat(), param.type && param.type->isVolatile(),
					param.isRegister, param.type && param.type->isSigned(),
					param.type && param.type->isRestrict() });
		}
		function.reserveParamSlots(paramSlots);
		// Slot reuse never crosses a function boundary: a slot freed by a scope in the previous
		// function names an index in THAT function's frame - see newLocalSlotFor().
		_freeLocalSlots.clear();

		pushScope();
		u32 slot = returnsStructIndirect ? 1u : 0u;
		for (const Param& param : node.params())
		{
			LocalSymbol symbol;
			symbol.kind = LocalSymbolKind::Local;
			symbol.localSlot = slot++;
			symbol.isIndirect = isIndirectStruct(param.type);
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
			emitVoid(node.location(), IrReturnPayload{ false, false, IrValue{}, IrValue{}, false });

		popScope();
		_hiddenReturnSlot.reset();
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

	void IrBuilder::visit(ast::InterruptVectorDecl&)
	{
		// Nothing to lower: it emits no code and no data, only a binding the linker resolves.
		// Codegen writes it straight from the AST, the same way it writes a global's `let`.
	}

	void IrBuilder::visit(ast::AsmStmt& node)
	{
		IrCallPayload payload;
		payload.callee = "<asm>";
		payload.inlineAsm = node.text().view();
		emitVoid(node.location(), payload);
	}

	void IrBuilder::visit(ast::CompoundLiteralExpr& node)
	{
		// An object of its own in the frame, set up from the list every time this is evaluated (so a literal in a
		// loop starts from its list again, in the same slot). Its value is what the object is: an address for an
		// array or a struct, the loaded value for a scalar - the same split lowerRValue makes for any lvalue.
		_lastValue = lowerRValue(&node);
	}

	void IrBuilder::visit(ast::DesignatedInitExpr&)
	{
		// Sema turns every designated element into a positional one before lowering; none reaches here.
	}

	void IrBuilder::visit(ast::StaticAssertDecl&)
	{
		// Nothing to lower: sema has already decided it, and it emits no code and no data.
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
