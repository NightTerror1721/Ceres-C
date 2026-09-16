#include <ceresc/ir/ir_optimizer.h>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ceresc::ir
{
	namespace
	{
		using support::OptimizationOptions;

		// The fixpoint loop's bound. Every pass either removes something or rewrites an instruction
		// into a strictly simpler one, so it converges on its own well before this - the cap exists
		// so a future pass that accidentally oscillates degrades into "stops optimizing" rather than
		// "never finishes compiling".
		constexpr u32 kMaxRounds = 8;

		// How big a callee may be and still be worth splicing into its caller. Deliberately small:
		// inlining a long body duplicates its whole instruction stream at every call site, and this
		// project's own priority is output a reader can follow (§0), not the last percent of speed.
		constexpr usize kMaxInlineInstrs = 32;
		// What `inline` buys a function that asks for it - see isInlinable().
		constexpr usize kMaxInlineInstrsWhenRequested = 160;

		// resultOf()/forEachOperand()/successorsOf() come from libs/ir's own headers (ir_instr.h,
		// ir_function.h) rather than being repeated here - see their comment there.

		// True for an instruction that can simply be dropped when nothing reads its result.
		//
		// Takes the instruction rather than its opcode because of the one case that cannot be
		// answered from the opcode alone: a Load. Reading a device register has a real side effect
		// on this machine (07-IO-Devices-and-Ports.md), so for as long as nothing could say which
		// loads those were, every load had to be kept. `volatile` is what says it, and it is now
		// recorded on the access itself (IrLoadPayload::isVolatile) precisely so it survives an
		// access that has no local slot to hang it on. A load NOT so marked reads ordinary memory
		// and may go when nothing reads its result.
		bool isPure(const IrInstr& instr) noexcept
		{
			switch (instr.opcode())
			{
				case IrOpcode::Const:
				case IrOpcode::BinOp:
				case IrOpcode::UnOp:
				case IrOpcode::Cmp:
				case IrOpcode::Copy:
				case IrOpcode::FrameAddr:
				case IrOpcode::GlobalAddr:
				case IrOpcode::VaStart:
					return true;
				case IrOpcode::Load:
					return !instr.as<IrLoadPayload>().isVolatile;
				default:
					return false;
			}
		}

		// ---- constant tracking ----------------------------------------------------------------------

		struct ConstValue
		{
			bool isFloat = false;
			i64 intValue = 0;
			f32 floatValue = 0.0f;
		};

		// Every temporary a Const instruction defines, as long as it is defined EXACTLY once in the
		// whole function. That last condition is what makes this safe without SSA: IrBuilder does
		// reuse one temporary id across two definitions in different blocks (materializeBoolean()
		// writes 1 on one path and 0 on the other, visit(TernaryExpr&) copies into one shared result
		// - ir_builder.cpp), and such a temporary is emphatically not a constant.
		std::unordered_map<u32, ConstValue> collectConstants(const IrFunction& function)
		{
			std::unordered_map<u32, u32> defCount;
			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					IrValue result = resultOf(*instr);
					if (result.isValid())
						++defCount[result.id];
				}
			}

			std::unordered_map<u32, ConstValue> constants;
			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					if (instr->opcode() != IrOpcode::Const)
						continue;
					const auto& p = instr->as<IrConstPayload>();
					if (!p.result.isValid() || defCount[p.result.id] != 1)
						continue;
					constants[p.result.id] = ConstValue{ p.isFloat, p.intValue, p.floatValue };
				}
			}
			return constants;
		}

		const ConstValue* findConstant(const std::unordered_map<u32, ConstValue>& constants, IrValue value)
		{
			if (!value.isValid())
				return nullptr;
			auto it = constants.find(value.id);
			return it == constants.end() ? nullptr : &it->second;
		}

		// C's own arithmetic is 32-bit here (libs/ast/type.cpp: int/long alike are 4 bytes), and
		// codegen materializes a Const by truncating to 32 bits anyway - so every folded result is
		// normalized the same way, as the 32-bit pattern read back as a signed value.
		i64 wrap32(i64 value) noexcept { return static_cast<i64>(static_cast<i32>(static_cast<u32>(value))); }

		std::optional<ConstValue> foldBinOp(const IrBinOpPayload& p, const ConstValue& lhs, const ConstValue& rhs)
		{
			if (p.isFloat)
			{
				if (!lhs.isFloat || !rhs.isFloat)
					return std::nullopt; // a mixed pair means IrBuilder would have converted first
				f32 a = lhs.floatValue, b = rhs.floatValue;
				switch (p.op)
				{
					case IrBinOp::Add: return ConstValue{ true, 0, a + b };
					case IrBinOp::Sub: return ConstValue{ true, 0, a - b };
					case IrBinOp::Mul: return ConstValue{ true, 0, a * b };
					case IrBinOp::Div:
						if (b == 0.0f)
							return std::nullopt; // the VM traps instead of producing a value - see the header
						return ConstValue{ true, 0, a / b };
					default: return std::nullopt;
				}
			}

			if (lhs.isFloat || rhs.isFloat)
				return std::nullopt;

			i64 a = lhs.intValue, b = rhs.intValue;
			u32 ua = static_cast<u32>(a), ub = static_cast<u32>(b);
			switch (p.op)
			{
				case IrBinOp::Add: return ConstValue{ false, wrap32(a + b), 0.0f };
				case IrBinOp::Sub: return ConstValue{ false, wrap32(a - b), 0.0f };
				case IrBinOp::Mul: return ConstValue{ false, wrap32(a * b), 0.0f };
				case IrBinOp::Div:
					if (b == 0)
						return std::nullopt; // see the header: the VM leaves the destination untouched
					if (p.isUnsigned)
						return ConstValue{ false, wrap32(static_cast<i64>(ua / ub)), 0.0f };
					if (a == INT32_MIN && b == -1)
						return std::nullopt; // the one signed division that overflows - leave it to the machine
					return ConstValue{ false, wrap32(a / b), 0.0f };
				case IrBinOp::Mod:
					if (b == 0)
						return std::nullopt;
					if (p.isUnsigned)
						return ConstValue{ false, wrap32(static_cast<i64>(ua % ub)), 0.0f };
					if (a == INT32_MIN && b == -1)
						return std::nullopt;
					return ConstValue{ false, wrap32(a % b), 0.0f };
				case IrBinOp::And: return ConstValue{ false, wrap32(static_cast<i64>(ua & ub)), 0.0f };
				case IrBinOp::Or:  return ConstValue{ false, wrap32(static_cast<i64>(ua | ub)), 0.0f };
				case IrBinOp::Xor: return ConstValue{ false, wrap32(static_cast<i64>(ua ^ ub)), 0.0f };
				// Shift counts are masked to five bits by the machine itself (05-Instruction-Set.md),
				// so the folded result has to mask them the same way rather than be undefined.
				case IrBinOp::Shl: return ConstValue{ false, wrap32(static_cast<i64>(ua << (ub & 31))), 0.0f };
				case IrBinOp::Shr: return ConstValue{ false, wrap32(static_cast<i64>(ua >> (ub & 31))), 0.0f };
				case IrBinOp::Sar: return ConstValue{ false, wrap32(static_cast<i32>(a) >> (ub & 31)), 0.0f };
			}
			return std::nullopt;
		}

		std::optional<ConstValue> foldUnOp(const IrUnOpPayload& p, const ConstValue& operand)
		{
			switch (p.op)
			{
				case IrUnOp::Neg:
					if (p.isFloat)
						return operand.isFloat ? std::optional<ConstValue>(ConstValue{ true, 0, -operand.floatValue }) : std::nullopt;
					return operand.isFloat ? std::nullopt : std::optional<ConstValue>(ConstValue{ false, wrap32(-operand.intValue), 0.0f });
				case IrUnOp::Not:
					return operand.isFloat ? std::nullopt
						: std::optional<ConstValue>(ConstValue{ false, wrap32(static_cast<i64>(~static_cast<u32>(operand.intValue))), 0.0f });
				case IrUnOp::LogicalNot:
					return operand.isFloat ? std::nullopt
						: std::optional<ConstValue>(ConstValue{ false, operand.intValue == 0 ? 1 : 0, 0.0f });
				case IrUnOp::IntToFloat:
					if (operand.isFloat)
						return std::nullopt;
					return ConstValue{ true, 0, p.isUnsigned ? static_cast<f32>(static_cast<u32>(operand.intValue))
															 : static_cast<f32>(static_cast<i32>(operand.intValue)) };
				case IrUnOp::FloatToInt:
					if (!operand.isFloat)
						return std::nullopt;
					// A floating-to-integer cast is undefined when its truncated value cannot fit. Leave
					// those cases for the target instruction, as with division by zero above.
					if (p.isUnsigned)
					{
						if (operand.floatValue < 0.0f || operand.floatValue >= 4294967296.0f)
							return std::nullopt;
						return ConstValue{ false, wrap32(static_cast<i64>(static_cast<u32>(operand.floatValue))), 0.0f };
					}
					if (operand.floatValue < -2147483648.0f || operand.floatValue >= 2147483648.0f)
						return std::nullopt;
					return ConstValue{ false, wrap32(static_cast<i64>(static_cast<i32>(operand.floatValue))), 0.0f };
				case IrUnOp::Narrow:
				{
					// Folding this matters more than it looks: EVERY `char c = 'a';` now goes
					// through a Narrow, and without folding, -O1 would emit an `sxtb` of a literal
					// in front of each one.
					if (operand.isFloat)
						return std::nullopt;
					u32 bits = static_cast<u32>(operand.intValue);
					if (p.narrowSize == IrMemSize::Byte)
					{
						u32 low = bits & 0xFFu;
						return ConstValue{ false, p.isUnsigned ? static_cast<i64>(low)
															   : static_cast<i64>(static_cast<i8>(low)), 0.0f };
					}
					if (p.narrowSize == IrMemSize::Half)
					{
						u32 low = bits & 0xFFFFu;
						return ConstValue{ false, p.isUnsigned ? static_cast<i64>(low)
															   : static_cast<i64>(static_cast<i16>(low)), 0.0f };
					}
					return ConstValue{ false, wrap32(operand.intValue), 0.0f }; // Word: nothing to narrow
				}
				case IrUnOp::ToBool:
					return operand.isFloat ? std::nullopt
						: std::optional<ConstValue>(ConstValue{ false, operand.intValue != 0 ? 1 : 0, 0.0f });
			}
			return std::nullopt;
		}

		// Evaluates one of the six predicates over two already-known constants, for both Cmp (which
		// produces a 0/1 value) and CondJump (which picks a branch).
		std::optional<bool> evaluatePredicate(IrCmpPredicate predicate, bool isUnsigned, bool isFloat,
			const ConstValue& lhs, const ConstValue& rhs)
		{
			if (isFloat || lhs.isFloat || rhs.isFloat)
			{
				if (!lhs.isFloat || !rhs.isFloat)
					return std::nullopt;
				f32 a = lhs.floatValue, b = rhs.floatValue;
				switch (predicate)
				{
					case IrCmpPredicate::Eq: return a == b;
					case IrCmpPredicate::Ne: return a != b;
					case IrCmpPredicate::Lt: return a < b;
					case IrCmpPredicate::Le: return a <= b;
					case IrCmpPredicate::Gt: return a > b;
					case IrCmpPredicate::Ge: return a >= b;
				}
				return std::nullopt;
			}

			if (isUnsigned)
			{
				u32 a = static_cast<u32>(lhs.intValue), b = static_cast<u32>(rhs.intValue);
				switch (predicate)
				{
					case IrCmpPredicate::Eq: return a == b;
					case IrCmpPredicate::Ne: return a != b;
					case IrCmpPredicate::Lt: return a < b;
					case IrCmpPredicate::Le: return a <= b;
					case IrCmpPredicate::Gt: return a > b;
					case IrCmpPredicate::Ge: return a >= b;
				}
				return std::nullopt;
			}

			i32 a = static_cast<i32>(lhs.intValue), b = static_cast<i32>(rhs.intValue);
			switch (predicate)
			{
				case IrCmpPredicate::Eq: return a == b;
				case IrCmpPredicate::Ne: return a != b;
				case IrCmpPredicate::Lt: return a < b;
				case IrCmpPredicate::Le: return a <= b;
				case IrCmpPredicate::Gt: return a > b;
				case IrCmpPredicate::Ge: return a >= b;
			}
			return std::nullopt;
		}

		IrInstr* makeConst(support::Arena& arena, support::SourceLocation loc, IrValue result, const ConstValue& value)
		{
			IrConstPayload payload;
			payload.result = result;
			payload.intValue = value.intValue;
			payload.floatValue = value.floatValue;
			payload.isFloat = value.isFloat;
			return arena.create<IrInstr>(loc, payload);
		}

		IrInstr* makeCopy(support::Arena& arena, support::SourceLocation loc, IrValue result, IrValue source, bool isFloat)
		{
			IrCopyPayload payload;
			payload.result = result;
			payload.isFloat = isFloat;
			payload.source = source;
			return arena.create<IrInstr>(loc, payload);
		}

		// ---- pass: constant folding, algebraic simplification, branch simplification ----------------

		// The integer identity/absorbing patterns worth recognizing, as (operand known constant) ->
		// what the whole operation collapses to. Float is deliberately excluded: `x * 1.0f` is not
		// `x` for a NaN or a signed zero, and this project has no fast-math promise to trade that
		// away for.
		enum class Simplification { None, TakeLhs, TakeRhs, Zero };

		Simplification simplifyBinOp(const IrBinOpPayload& p, const ConstValue* lhs, const ConstValue* rhs)
		{
			if (p.isFloat)
				return Simplification::None;
			auto isInt = [](const ConstValue* c, i64 wanted) { return c && !c->isFloat && c->intValue == wanted; };

			switch (p.op)
			{
				case IrBinOp::Add:
					if (isInt(rhs, 0)) return Simplification::TakeLhs;
					if (isInt(lhs, 0)) return Simplification::TakeRhs;
					return Simplification::None;
				case IrBinOp::Sub:
					if (isInt(rhs, 0)) return Simplification::TakeLhs;
					return Simplification::None;
				case IrBinOp::Mul:
					if (isInt(rhs, 1)) return Simplification::TakeLhs;
					if (isInt(lhs, 1)) return Simplification::TakeRhs;
					if (isInt(rhs, 0) || isInt(lhs, 0)) return Simplification::Zero;
					return Simplification::None;
				case IrBinOp::Div:
					if (isInt(rhs, 1)) return Simplification::TakeLhs;
					return Simplification::None;
				case IrBinOp::And:
					if (isInt(rhs, 0) || isInt(lhs, 0)) return Simplification::Zero;
					return Simplification::None;
				case IrBinOp::Or:
				case IrBinOp::Xor:
					if (isInt(rhs, 0)) return Simplification::TakeLhs;
					if (isInt(lhs, 0)) return Simplification::TakeRhs;
					return Simplification::None;
				case IrBinOp::Shl:
				case IrBinOp::Shr:
				case IrBinOp::Sar:
					if (isInt(rhs, 0)) return Simplification::TakeLhs;
					return Simplification::None;
				default:
					return Simplification::None;
			}
		}

		bool foldFunction(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.constantFolding && !options.algebraicSimplification && !options.branchSimplification)
				return false;

			std::unordered_map<u32, ConstValue> constants = collectConstants(function);
			bool changed = false;

			for (const auto& block : function.blocks())
			{
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());

				for (IrInstr* instr : block->instrs())
				{
					IrInstr* replacement = nullptr;

					switch (instr->opcode())
					{
						case IrOpcode::BinOp:
						{
							const auto& p = instr->as<IrBinOpPayload>();
							const ConstValue* lhs = findConstant(constants, p.lhs);
							const ConstValue* rhs = findConstant(constants, p.rhs);

							if (options.constantFolding && lhs && rhs)
							{
								if (std::optional<ConstValue> folded = foldBinOp(p, *lhs, *rhs))
								{
									replacement = makeConst(arena, instr->location(), p.result, *folded);
									constants[p.result.id] = *folded;
									break;
								}
							}
							if (options.algebraicSimplification)
							{
								switch (simplifyBinOp(p, lhs, rhs))
								{
									case Simplification::TakeLhs: replacement = makeCopy(arena, instr->location(), p.result, p.lhs, p.isFloat); break;
									case Simplification::TakeRhs: replacement = makeCopy(arena, instr->location(), p.result, p.rhs, p.isFloat); break;
									case Simplification::Zero:
									{
										ConstValue zero{ false, 0, 0.0f };
										replacement = makeConst(arena, instr->location(), p.result, zero);
										constants[p.result.id] = zero;
										break;
									}
									case Simplification::None: break;
								}
							}
							break;
						}

						case IrOpcode::UnOp:
						{
							if (!options.constantFolding)
								break;
							const auto& p = instr->as<IrUnOpPayload>();
							if (const ConstValue* operand = findConstant(constants, p.operand))
							{
								if (std::optional<ConstValue> folded = foldUnOp(p, *operand))
								{
									replacement = makeConst(arena, instr->location(), p.result, *folded);
									constants[p.result.id] = *folded;
								}
							}
							break;
						}

						case IrOpcode::Cmp:
						{
							if (!options.constantFolding)
								break;
							const auto& p = instr->as<IrCmpPayload>();
							const ConstValue* lhs = findConstant(constants, p.lhs);
							const ConstValue* rhs = findConstant(constants, p.rhs);
							if (!lhs || !rhs)
								break;
							if (std::optional<bool> result = evaluatePredicate(p.predicate, p.isUnsigned, p.isFloat, *lhs, *rhs))
							{
								ConstValue value{ false, *result ? 1 : 0, 0.0f };
								replacement = makeConst(arena, instr->location(), p.result, value);
								constants[p.result.id] = value;
							}
							break;
						}

						case IrOpcode::CondJump:
						{
							if (!options.branchSimplification)
								break;
							const auto& p = instr->as<IrCondJumpPayload>();
							// Both arms landing in the same block makes the condition irrelevant even
							// when nothing about it is known - this shows up after jump threading
							// collapses two different empty blocks onto one target.
							if (p.trueTarget == p.falseTarget)
							{
								replacement = arena.create<IrInstr>(instr->location(), IrJumpPayload{ p.trueTarget });
								break;
							}
							const ConstValue* lhs = findConstant(constants, p.lhs);
							const ConstValue* rhs = findConstant(constants, p.rhs);
							if (!lhs || !rhs)
								break;
							if (std::optional<bool> taken = evaluatePredicate(p.predicate, p.isUnsigned, p.isFloat, *lhs, *rhs))
								replacement = arena.create<IrInstr>(instr->location(), IrJumpPayload{ *taken ? p.trueTarget : p.falseTarget });
							break;
						}

						default: break;
					}

					if (replacement)
					{
						rewritten.push_back(replacement);
						changed = true;
					}
					else
					{
						rewritten.push_back(instr);
					}
				}

				block->replaceInstrs(std::move(rewritten));
			}
			return changed;
		}

		// ---- local escape analysis (shared by load forwarding and dead-store elimination) -----------

		// Which locals never have their address escape into anything but a Load/Store through it.
		// Such a local cannot be reached through any pointer in the program, which is what makes the
		// two memory passes below safe: nothing else can observe or change it.
		//
		// libs/codegen's value_placement.cpp asks the same question for a different purpose (may
		// this local live in a register?) and answers it with extra conditions of its own about
		// access width and register banks. Keeping the two separate rather than sharing one
		// "escapes" helper keeps each one's conditions next to the decision it guards - and they are
		// both conservative, so disagreeing costs an optimization, never correctness.
		std::vector<bool> collectNonEscapingLocals(const IrFunction& function)
		{
			usize localCount = function.localCount();
			usize tempCount = function.tempCount();
			std::vector<bool> escapes(localCount, false);
			std::vector<u32> frameAddrLocal(tempCount, ~0u);
			std::unordered_map<u32, u32> defCount;

			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					IrValue result = resultOf(*instr);
					if (result.isValid())
						++defCount[result.id];
					if (instr->opcode() != IrOpcode::FrameAddr)
						continue;
					const auto& p = instr->as<IrFrameAddrPayload>();
					if (p.result.isValid() && p.result.id < tempCount && p.localIndex < localCount)
						frameAddrLocal[p.result.id] = p.localIndex;
				}
			}

			auto markEscape = [&](IrValue value)
			{
				if (value.isValid() && value.id < tempCount && frameAddrLocal[value.id] != ~0u)
					escapes[frameAddrLocal[value.id]] = true;
			};

			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					if (instr->opcode() == IrOpcode::Load)
						continue; // the address operand of a load is the one use that does not escape
					if (instr->opcode() == IrOpcode::Store)
					{
						// Same for a store's address - but the VALUE it stores is an escape, since
						// that address is now reachable from wherever it was written.
						markEscape(instr->as<IrStorePayload>().value);
						continue;
					}
					forEachOperand(*instr, markEscape);
				}
			}

			for (usize t = 0; t < tempCount; ++t)
				if (frameAddrLocal[t] != ~0u && defCount[static_cast<u32>(t)] > 1)
					escapes[frameAddrLocal[t]] = true;

			std::vector<bool> nonEscaping(localCount);
			for (usize i = 0; i < localCount; ++i)
				nonEscaping[i] = !escapes[i];
			return nonEscaping;
		}

		// The local a temporary is the address of, or ~0u.
		std::vector<u32> mapFrameAddrTemps(const IrFunction& function)
		{
			std::vector<u32> frameAddrLocal(function.tempCount(), ~0u);
			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					if (instr->opcode() != IrOpcode::FrameAddr)
						continue;
					const auto& p = instr->as<IrFrameAddrPayload>();
					if (p.result.isValid() && p.result.id < frameAddrLocal.size() && p.localIndex < function.localCount())
						frameAddrLocal[p.result.id] = p.localIndex;
				}
			}
			return frameAddrLocal;
		}

		// ---- pass: copy propagation ------------------------------------------------------------------

		// Rebuilds one instruction with its operands renamed through `replacement`. Returns nullptr
		// when nothing changed, so the caller can keep the original instruction.
		IrInstr* renameOperands(const IrInstr& instr, support::Arena& arena, const std::unordered_map<u32, IrValue>& replacement)
		{
			auto rename = [&](IrValue value) -> IrValue
			{
				if (!value.isValid())
					return value;
				auto it = replacement.find(value.id);
				return it == replacement.end() ? value : it->second;
			};
			bool changed = false;
			forEachOperand(instr, [&](IrValue value) { if (!(rename(value) == value)) changed = true; });
			if (!changed)
				return nullptr;

			support::SourceLocation loc = instr.location();
			switch (instr.opcode())
			{
				case IrOpcode::BinOp:
				{
					IrBinOpPayload p = instr.as<IrBinOpPayload>();
					p.lhs = rename(p.lhs);
					p.rhs = rename(p.rhs);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::UnOp:
				{
					IrUnOpPayload p = instr.as<IrUnOpPayload>();
					p.operand = rename(p.operand);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Cmp:
				{
					IrCmpPayload p = instr.as<IrCmpPayload>();
					p.lhs = rename(p.lhs);
					p.rhs = rename(p.rhs);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Copy:
				{
					IrCopyPayload p = instr.as<IrCopyPayload>();
					p.source = rename(p.source);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Load:
				{
					IrLoadPayload p = instr.as<IrLoadPayload>();
					p.address = rename(p.address);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Store:
				{
					IrStorePayload p = instr.as<IrStorePayload>();
					p.address = rename(p.address);
					p.value = rename(p.value);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Param:
				{
					IrParamPayload p = instr.as<IrParamPayload>();
					p.value = rename(p.value);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::CondJump:
				{
					IrCondJumpPayload p = instr.as<IrCondJumpPayload>();
					p.lhs = rename(p.lhs);
					p.rhs = rename(p.rhs);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Return:
				{
					IrReturnPayload p = instr.as<IrReturnPayload>();
					p.value = rename(p.value);
					return arena.create<IrInstr>(loc, p);
				}
				default:
					return nullptr;
			}
		}

		bool propagateCopies(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.copyPropagation)
				return false;

			std::unordered_map<u32, u32> defCount;
			for (const auto& block : function.blocks())
				for (const IrInstr* instr : block->instrs())
					if (IrValue result = resultOf(*instr); result.isValid())
						++defCount[result.id];

			// `%b = copy %a` lets every read of %b read %a instead - but only when BOTH are written
			// exactly once in the whole function. IrBuilder reuses a temporary id for two
			// definitions on purpose (materializeBoolean(), visit(TernaryExpr&) - ir_builder.cpp),
			// and with two definitions in play "the value of %a here" is no longer a question the
			// instruction list alone can answer.
			std::unordered_map<u32, IrValue> replacement;
			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					if (instr->opcode() != IrOpcode::Copy)
						continue;
					const auto& p = instr->as<IrCopyPayload>();
					if (!p.result.isValid() || !p.source.isValid())
						continue;
					if (defCount[p.result.id] != 1 || defCount[p.source.id] != 1)
						continue;
					replacement[p.result.id] = p.source;
				}
			}
			if (replacement.empty())
				return false;

			// Chains (`%c = copy %b`, `%b = copy %a`) collapse to their ultimate source, bounded so a
			// malformed cycle cannot spin here.
			for (auto& [id, target] : replacement)
			{
				IrValue resolved = target;
				for (u32 step = 0; step < 16; ++step)
				{
					auto it = replacement.find(resolved.id);
					if (it == replacement.end() || it->second == resolved)
						break;
					resolved = it->second;
				}
				target = resolved;
			}

			bool changed = false;
			for (const auto& block : function.blocks())
			{
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;
				for (IrInstr* instr : block->instrs())
				{
					if (IrInstr* renamed = renameOperands(*instr, arena, replacement))
					{
						rewritten.push_back(renamed);
						blockChanged = true;
					}
					else
					{
						rewritten.push_back(instr);
					}
				}
				if (blockChanged)
				{
					block->replaceInstrs(std::move(rewritten));
					changed = true;
				}
			}
			return changed;
		}

		// ---- pass: store-to-load forwarding ----------------------------------------------------------

		bool forwardLoads(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.loadForwarding)
				return false;

			std::vector<bool> nonEscaping = collectNonEscapingLocals(function);
			std::vector<u32> frameAddrLocal = mapFrameAddrTemps(function);
			std::span<const IrLocalSlot> localSlots = function.localSlots();
			bool changed = false;

			for (const auto& block : function.blocks())
			{
				// What each local is known to hold right now, and only within this block: control
				// arriving from anywhere else would have its own answer, and no attempt is made to
				// merge them.
				std::unordered_map<u32, IrValue> known;
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;

				auto localOf = [&](IrValue address) -> u32
				{
					if (!address.isValid() || address.id >= frameAddrLocal.size())
						return ~0u;
					u32 local = frameAddrLocal[address.id];
					if (local == ~0u || local >= nonEscaping.size() || !nonEscaping[local])
						return ~0u;
					return local;
				};

				for (IrInstr* instr : block->instrs())
				{
					if (instr->opcode() == IrOpcode::Store)
					{
						const auto& p = instr->as<IrStorePayload>();
						u32 local = localOf(p.address);
						if (local != ~0u)
						{
							// Either the whole local is volatile, or this one access is - the second
							// is how `volatile int* p` says it, since the qualifier is on the pointee
							// and there is no local slot to hang it on.
							if (localSlots[local].isVolatile || p.isVolatile)
							{
								known.erase(local);
								rewritten.push_back(instr);
								continue;
							}
							if (p.size == irMemSizeForBytes(localSlots[local].sizeInBytes) &&
								p.isFloat == localSlots[local].isFloat)
							{
								known[local] = p.value;
							}
							else
							{
								// A store of a DIFFERENT width to the same local - `*(char*)&x = 1`
								// over an int, which survives the escape analysis because the address
								// is still only ever a store operand. It overwrites part of the slot
								// with something this pass cannot name, so whatever was known about
								// the local stops being true and must be forgotten rather than merely
								// left un-updated: a later full-width load has to become a real load
								// again. (Dropping the entry is also why a narrower store need not be
								// modelled precisely - forgetting is always sound, it only costs the
								// optimization.)
								known.erase(local);
							}
						}
						rewritten.push_back(instr);
						continue;
					}

					if (instr->opcode() == IrOpcode::Load)
					{
						const auto& p = instr->as<IrLoadPayload>();
						u32 local = localOf(p.address);
						auto it = (local == ~0u) ? known.end() : known.find(local);
						if (local != ~0u && !localSlots[local].isVolatile && !p.isVolatile && it != known.end() &&
							p.size == irMemSizeForBytes(localSlots[local].sizeInBytes) &&
							p.isFloat == localSlots[local].isFloat)
						{
							// Nothing else can have written this local since - it never escaped -
							// so the value stored into it is still exactly what a load would read.
							rewritten.push_back(makeCopy(arena, instr->location(), p.result, it->second, p.isFloat));
							blockChanged = true;
							continue;
						}
						rewritten.push_back(instr);
						continue;
					}

					rewritten.push_back(instr);
				}

				if (blockChanged)
				{
					block->replaceInstrs(std::move(rewritten));
					changed = true;
				}
			}
			return changed;
		}

		// ---- pass: dead store elimination -------------------------------------------------------------

		bool eliminateDeadStores(IrFunction& function, const OptimizationOptions& options)
		{
			if (!options.deadStoreElimination)
				return false;

			std::vector<bool> nonEscaping = collectNonEscapingLocals(function);
			std::vector<u32> frameAddrLocal = mapFrameAddrTemps(function);

			// A local that never appears as a load's address is never read at all - and since it did
			// not escape, nothing outside this function can read it either. Every store into it is
			// therefore dead. Deliberately whole-local rather than per-store: "is this particular
			// store overwritten before any read" needs a reaching-stores analysis, and the
			// whole-local case is the one inlining actually produces (a parameter copied in and then
			// forwarded away entirely).
			std::vector<bool> everRead(function.localCount(), false);
			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					if (instr->opcode() != IrOpcode::Load)
						continue;
					IrValue address = instr->as<IrLoadPayload>().address;
					if (address.isValid() && address.id < frameAddrLocal.size() && frameAddrLocal[address.id] != ~0u)
						everRead[frameAddrLocal[address.id]] = true;
				}
			}

			bool changed = false;
			for (const auto& block : function.blocks())
			{
				std::vector<IrInstr*> live;
				live.reserve(block->instrs().size());
				for (IrInstr* instr : block->instrs())
				{
					if (instr->opcode() == IrOpcode::Store)
					{
						const auto& store = instr->as<IrStorePayload>();
						IrValue address = store.address;
						u32 local = (address.isValid() && address.id < frameAddrLocal.size()) ? frameAddrLocal[address.id] : ~0u;
						if (local != ~0u && local < nonEscaping.size() && !function.localSlots()[local].isVolatile &&
							!store.isVolatile && nonEscaping[local] && !everRead[local])
						{
							changed = true;
							continue;
						}
					}
					live.push_back(instr);
				}
				if (live.size() != block->instrs().size())
					block->replaceInstrs(std::move(live));
			}
			return changed;
		}

		// ---- pass: unused function elimination ---------------------------------------------------------

		bool removeUnusedFunctions(IrModule& module, const OptimizationOptions& options)
		{
			if (!options.unusedFunctionElimination)
				return false;
			// A function is a root when something outside this unit could call it: `main`, which the
			// linker looks up by name, and every function with external linkage, which another object
			// may call at link time (12-Labels-and-Symbols.md). Only a `static` function that no chain
			// of calls from a root reaches can be dropped - this used to assume one self-contained
			// translation unit, which stopped being true the moment ceresc learned to compile several
			// files into objects and link them.
			std::unordered_map<std::string_view, const IrFunction*> byName;
			for (const auto& function : module.functions())
				byName.emplace(function->name(), function.get());

			std::vector<std::string_view> worklist;
			std::unordered_map<std::string_view, bool> reached;
			for (const auto& function : module.functions())
			{
				// An interrupt handler is a root whatever its linkage: the machine reaches it through
				// the vector table, and no instruction anywhere names it.
				if (function->name() != "main" && !function->hasExternalLinkage() && !function->isInterruptHandler())
					continue;
				reached[function->name()] = true;
				worklist.push_back(function->name());
			}
			while (!worklist.empty())
			{
				std::string_view name = worklist.back();
				worklist.pop_back();
				auto it = byName.find(name);
				if (it == byName.end())
					continue;
				auto reach = [&](std::string_view name)
				{
					if (name.empty() || reached[name])
						return;
					reached[name] = true;
					worklist.push_back(name);
				};
				for (const auto& block : it->second->blocks())
				{
					for (const IrInstr* instr : block->instrs())
					{
						// A call by name is the obvious edge. The other one is a GlobalAddr naming a
						// function: taking a function's address makes it reachable through whatever
						// that address is later stored in, and nothing in this unit need ever name
						// it in a Call again. A function pointer is the case that needs it, and an
						// interrupt handler - reachable only through the vector table - is the same
						// shape of edge with no instruction at all at the far end of it.
						if (instr->opcode() == IrOpcode::Call)
							reach(instr->as<IrCallPayload>().callee);
						else if (instr->opcode() == IrOpcode::GlobalAddr)
						{
							std::string_view name = instr->as<IrGlobalAddrPayload>().name;
							if (byName.contains(name))
								reach(name);
						}
					}
				}
			}

			std::vector<bool> keep;
			keep.reserve(module.functions().size());
			bool changed = false;
			for (const auto& function : module.functions())
			{
				bool live = reached[function->name()];
				keep.push_back(live);
				changed = changed || !live;
			}
			if (changed)
				module.retainFunctions(keep);
			return changed;
		}

		// ---- pass: jump threading --------------------------------------------------------------------

		// Follows a chain of blocks that do nothing but jump onward, and answers where control
		// really ends up. Bounded by the block count so a `.L1: jp .L1` loop resolves to itself
		// instead of spinning here.
		BasicBlock* resolveJumpTarget(BasicBlock* target, usize blockCount)
		{
			for (usize step = 0; step < blockCount; ++step)
			{
				std::span<IrInstr* const> instrs = target->instrs();
				if (instrs.size() != 1 || instrs.front()->opcode() != IrOpcode::Jump)
					return target;
				BasicBlock* next = instrs.front()->as<IrJumpPayload>().target;
				if (next == target)
					return target;
				target = next;
			}
			return target;
		}

		bool threadJumps(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.jumpThreading)
				return false;

			usize blockCount = function.blocks().size();
			bool changed = false;

			for (const auto& block : function.blocks())
			{
				std::span<IrInstr* const> instrs = block->instrs();
				if (instrs.empty())
					continue;

				IrInstr* last = instrs.back();
				IrInstr* replacement = nullptr;

				if (last->opcode() == IrOpcode::Jump)
				{
					const auto& p = last->as<IrJumpPayload>();
					BasicBlock* resolved = resolveJumpTarget(p.target, blockCount);
					// A block that is itself just a jump must keep its own single instruction:
					// rewriting it to point past itself is fine, but it stays the block others may
					// still target until unreachable-block removal decides otherwise.
					if (resolved != p.target && resolved != block.get())
						replacement = arena.create<IrInstr>(last->location(), IrJumpPayload{ resolved });
				}
				else if (last->opcode() == IrOpcode::CondJump)
				{
					const auto& p = last->as<IrCondJumpPayload>();
					BasicBlock* trueTarget = resolveJumpTarget(p.trueTarget, blockCount);
					BasicBlock* falseTarget = resolveJumpTarget(p.falseTarget, blockCount);
					if (trueTarget != p.trueTarget || falseTarget != p.falseTarget)
					{
						IrCondJumpPayload rewritten = p;
						rewritten.trueTarget = trueTarget;
						rewritten.falseTarget = falseTarget;
						replacement = arena.create<IrInstr>(last->location(), rewritten);
					}
				}

				if (!replacement)
					continue;

				std::vector<IrInstr*> rewritten(instrs.begin(), instrs.end());
				rewritten.back() = replacement;
				block->replaceInstrs(std::move(rewritten));
				changed = true;
			}
			return changed;
		}

		// ---- pass: unreachable block elimination -----------------------------------------------------

		bool removeUnreachableBlocks(IrFunction& function, const OptimizationOptions& options)
		{
			if (!options.unreachableBlockElimination)
				return false;

			std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
			if (blocks.empty())
				return false;

			std::unordered_map<const BasicBlock*, usize> indexOf;
			for (usize i = 0; i < blocks.size(); ++i)
				indexOf[blocks[i].get()] = i;

			std::vector<bool> reachable(blocks.size(), false);
			std::vector<usize> worklist{ 0 };
			reachable[0] = true;
			while (!worklist.empty())
			{
				usize index = worklist.back();
				worklist.pop_back();
				for (BasicBlock* successor : successorsOf(function, index))
				{
					auto it = indexOf.find(successor);
					if (it == indexOf.end() || reachable[it->second])
						continue;
					reachable[it->second] = true;
					worklist.push_back(it->second);
				}
			}

			if (std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }))
				return false;

			function.retainBlocks(reachable);
			return true;
		}

		// ---- pass: dead code elimination -------------------------------------------------------------

		bool eliminateDeadCode(IrFunction& function, const OptimizationOptions& options)
		{
			if (!options.deadCodeElimination)
				return false;

			bool changedAtAll = false;
			for (u32 round = 0; round < kMaxRounds; ++round)
			{
				std::unordered_map<u32, u32> useCount;
				for (const auto& block : function.blocks())
					for (const IrInstr* instr : block->instrs())
						forEachOperand(*instr, [&](IrValue value) { if (value.isValid()) ++useCount[value.id]; });

				bool changed = false;
				for (const auto& block : function.blocks())
				{
					std::vector<IrInstr*> live;
					live.reserve(block->instrs().size());
					for (IrInstr* instr : block->instrs())
					{
						IrValue result = resultOf(*instr);
						bool dead = isPure(*instr) && result.isValid() && useCount[result.id] == 0;
						if (dead)
							changed = true;
						else
							live.push_back(instr);
					}
					if (live.size() != block->instrs().size())
						block->replaceInstrs(std::move(live));
				}

				changedAtAll |= changed;
				if (!changed)
					break;
			}
			return changedAtAll;
		}

		// ---- pass: inlining ---------------------------------------------------------------------------

		// A callee worth splicing: one straight-line block that ends in a Return and calls nothing
		// itself (which also rules out recursion by construction), short enough to be worth copying,
		// and not the entry point - `main` is never called from anywhere in the first place, and
		// codegen gives it a different epilogue entirely (codegen.h's own note).
		bool isInlinable(const IrFunction& function)
		{
			if (function.name() == "main")
				return false;
			// Nothing calls an interrupt handler, so there is no call site to splice it into - and its
			// prologue and epilogue are the point of it (save every register, `iret`), which a spliced
			// body would leave behind.
			if (function.isInterruptHandler())
				return false;
			// A variadic callee is handed arguments its parameter list does not describe, so there is
			// nothing for inlineCall() to bind them to - and its body reads them out of the caller's
			// frame (IrOpcode::VaStart), which stops meaning anything once the body is spliced into a
			// different frame entirely. Ruled out here rather than left to inlineCall()'s arity guard,
			// which would already decline every such call but only by accident of the argument count.
			if (function.isVariadic())
				return false;
			if (function.blocks().size() != 1)
				return false;

			std::span<IrInstr* const> instrs = function.blocks().front()->instrs();
			// `inline` raises the size limit rather than removing it: the request is a hint about what
			// is worth copying, not a licence to copy a four-hundred-instruction body into every call.
			usize limit = function.isInlineHint() ? kMaxInlineInstrsWhenRequested : kMaxInlineInstrs;
			if (instrs.empty() || instrs.size() > limit)
				return false;
			if (instrs.back()->opcode() != IrOpcode::Return)
				return false;

			for (IrInstr* instr : instrs)
			{
				if (instr->opcode() == IrOpcode::Call)
					return false;
				// A Return anywhere but at the very end would mean control leaves early, which a
				// single block cannot express anyway - but check rather than assume.
				if (instr->opcode() == IrOpcode::Return && instr != instrs.back())
					return false;
			}
			return true;
		}

		// Rebuilds one callee instruction inside the caller: every temporary it names becomes a
		// fresh caller temporary, and every local slot it names becomes the caller slot reserved for
		// it. Only ever applied to the shapes isInlinable() allows, so Jump/CondJump (which would
		// also need their BasicBlock* remapped) can never reach here.
		IrInstr* remapInstr(const IrInstr& instr, support::Arena& arena, IrFunction& caller,
			std::unordered_map<u32, IrValue>& tempMap, const std::vector<u32>& localMap)
		{
			auto mapValue = [&](IrValue value) -> IrValue
			{
				if (!value.isValid())
					return value;
				auto it = tempMap.find(value.id);
				if (it != tempMap.end())
					return it->second;
				IrValue fresh = caller.newTemp();
				tempMap.emplace(value.id, fresh);
				return fresh;
			};

			support::SourceLocation loc = instr.location();
			switch (instr.opcode())
			{
				case IrOpcode::Const:
				{
					IrConstPayload p = instr.as<IrConstPayload>();
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::BinOp:
				{
					IrBinOpPayload p = instr.as<IrBinOpPayload>();
					p.lhs = mapValue(p.lhs);
					p.rhs = mapValue(p.rhs);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::UnOp:
				{
					IrUnOpPayload p = instr.as<IrUnOpPayload>();
					p.operand = mapValue(p.operand);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Cmp:
				{
					IrCmpPayload p = instr.as<IrCmpPayload>();
					p.lhs = mapValue(p.lhs);
					p.rhs = mapValue(p.rhs);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Copy:
				{
					IrCopyPayload p = instr.as<IrCopyPayload>();
					p.source = mapValue(p.source);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::FrameAddr:
				{
					IrFrameAddrPayload p = instr.as<IrFrameAddrPayload>();
					if (p.localIndex >= localMap.size())
						return nullptr;
					p.localIndex = localMap[p.localIndex];
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::GlobalAddr:
				{
					IrGlobalAddrPayload p = instr.as<IrGlobalAddrPayload>();
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Load:
				{
					IrLoadPayload p = instr.as<IrLoadPayload>();
					p.address = mapValue(p.address);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Store:
				{
					IrStorePayload p = instr.as<IrStorePayload>();
					p.address = mapValue(p.address);
					p.value = mapValue(p.value);
					return arena.create<IrInstr>(loc, p);
				}
				default:
					return nullptr; // Param/Call/Jump/CondJump/Return never reach here - see isInlinable()
			}
		}

		// Splices `callee`'s body in place of the Call at the end of `prefix`, whose last `argCount`
		// entries are that call's Param instructions. Returns false (leaving `prefix` untouched) if
		// anything about the shape is not what inlining assumes.
		bool spliceInlinedCall(std::vector<IrInstr*>& prefix, const IrCallPayload& call, support::SourceLocation callLoc,
			const IrFunction& callee, IrFunction& caller, support::Arena& arena)
		{
			if (call.argCount != callee.paramCount() || call.argCount > prefix.size())
				return false;
			std::span<IrInstr* const> body = callee.blocks().front()->instrs();
			const auto& ret = body.back()->as<IrReturnPayload>();
			if (call.hasResult != ret.hasValue || (call.hasResult && !ret.value.isValid()))
				return false;
			std::vector<bool> defined(callee.tempCount(), false);
			for (IrInstr* instr : body)
			{
				if (instr->opcode() == IrOpcode::FrameAddr && instr->as<IrFrameAddrPayload>().localIndex >= callee.localCount())
					return false;
				IrValue result = resultOf(*instr);
				if (result.isValid() && result.id < defined.size())
					defined[result.id] = true;
			}
			if (call.hasResult && (ret.value.id >= defined.size() || !defined[ret.value.id]))
				return false;

			std::vector<IrValue> argValues(call.argCount);
			for (u32 i = 0; i < call.argCount; ++i)
			{
				const IrInstr* param = prefix[prefix.size() - call.argCount + i];
				if (param->opcode() != IrOpcode::Param)
					return false;
				argValues[i] = param->as<IrParamPayload>().value;
			}
			prefix.resize(prefix.size() - call.argCount); // the Params are subsumed by the stores below

			// Every callee slot - parameters first, then its own locals - gets a fresh slot in the
			// caller's frame. They are ordinary caller locals from here on, which is what lets the
			// register allocator (libs/codegen) promote them exactly like any other.
			std::vector<u32> localMap(callee.localCount());
			for (u32 i = 0; i < callee.localCount(); ++i)
			{
				const IrLocalSlot& slot = callee.localSlots()[i];
				localMap[i] = caller.newLocalSlot(slot.sizeInBytes, slot.isFloat);
			}

			// Each argument is stored into the slot standing in for its parameter, which is exactly
			// what the callee's own prologue would have done with the incoming register.
			for (u32 i = 0; i < call.argCount; ++i)
			{
				const IrLocalSlot& slot = callee.localSlots()[i];
				IrFrameAddrPayload addr;
				addr.result = caller.newTemp();
				addr.localIndex = localMap[i];
				prefix.push_back(arena.create<IrInstr>(callLoc, addr));

				IrStorePayload store;
				store.size = irMemSizeForBytes(slot.sizeInBytes);
				store.isFloat = slot.isFloat;
				store.address = addr.result;
				store.value = argValues[i];
				prefix.push_back(arena.create<IrInstr>(callLoc, store));
			}

			std::unordered_map<u32, IrValue> tempMap;
			for (IrInstr* instr : body)
			{
				if (instr->opcode() == IrOpcode::Return)
				{
					const auto& returnPayload = instr->as<IrReturnPayload>();
					if (call.hasResult)
					{
						// The caller reads this call's result, so the callee has to actually produce
						// one, and it has to be a value the body already defined (which is where
						// tempMap got its entry). Anything else is IR sema would have rejected -
						// bail out and leave the real call in place rather than invent a value.
						if (!returnPayload.hasValue || !returnPayload.value.isValid())
							return false;
						auto mapped = tempMap.find(returnPayload.value.id);
						if (mapped == tempMap.end())
							return false;
						prefix.push_back(makeCopy(arena, instr->location(), call.result, mapped->second, call.isFloat));
					}
					break; // the Return is the callee's last instruction - see isInlinable()
				}

				IrInstr* copied = remapInstr(*instr, arena, caller, tempMap, localMap);
				if (!copied)
					return false;
				prefix.push_back(copied);
			}
			return true;
		}

		bool inlineCalls(IrModule& module, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.inlining)
				return false;

			std::unordered_map<std::string_view, const IrFunction*> inlinable;
			for (const auto& function : module.functions())
				if (isInlinable(*function))
					inlinable.emplace(function->name(), function.get());
			if (inlinable.empty())
				return false;

			bool changed = false;
			for (const auto& function : module.functions())
			{
				for (const auto& block : function->blocks())
				{
					std::vector<IrInstr*> rewritten;
					rewritten.reserve(block->instrs().size());
					bool blockChanged = false;

					for (IrInstr* instr : block->instrs())
					{
						if (instr->opcode() != IrOpcode::Call)
						{
							rewritten.push_back(instr);
							continue;
						}

						const auto& call = instr->as<IrCallPayload>();
						auto candidate = inlinable.find(call.callee);
						// Never inline a function into itself: isInlinable() already rules out a
						// callee that calls anything, but a self-call would still be reachable if a
						// future relaxation let it through.
						if (candidate == inlinable.end() || candidate->second == function.get())
						{
							rewritten.push_back(instr);
							continue;
						}

						std::vector<IrInstr*> attempt = rewritten;
						if (spliceInlinedCall(attempt, call, instr->location(), *candidate->second, *function, arena))
						{
							rewritten = std::move(attempt);
							blockChanged = true;
						}
						else
						{
							rewritten.push_back(instr);
						}
					}

					if (blockChanged)
					{
						block->replaceInstrs(std::move(rewritten));
						changed = true;
					}
				}
			}
			return changed;
		}
	}

	void optimize(IrModule& module, support::Arena& arena, const OptimizationOptions& options)
	{
		inlineCalls(module, arena, options);

		for (const auto& function : module.functions())
		{
			for (u32 round = 0; round < kMaxRounds; ++round)
			{
				// Order within a round matters less than the round itself: each pass exposes work
				// for the others (forwarding turns a load into a copy, propagation makes that copy
				// dead, dead-code removal empties a block, threading routes around it), so the loop
				// runs until nothing moves rather than trying to get the order exactly right.
				bool changed = false;
				changed |= foldFunction(*function, arena, options);
				changed |= forwardLoads(*function, arena, options);
				changed |= propagateCopies(*function, arena, options);
				changed |= eliminateDeadStores(*function, options);
				changed |= threadJumps(*function, arena, options);
				changed |= removeUnreachableBlocks(*function, options);
				changed |= eliminateDeadCode(*function, options);
				if (!changed)
					break;
			}
		}

		// Last, so that a function left callerless BY inlining is dropped too.
		removeUnusedFunctions(module, options);
	}
}
