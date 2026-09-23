#include <ceresc/ir/ir_optimizer.h>

#include <algorithm>
#include <bit>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
				case IrOpcode::Builtin:
					return true;
				case IrOpcode::Load:
					return !instr.as<IrLoadPayload>().isVolatile;
				default:
					return false;
			}
		}

		// A call to a function declared `pure` or `const` has no side effects, so one whose result
		// nothing reads may be dropped - which is what those attributes buy over an ordinary call.
		// `pure` may still read memory, but reading is not an effect that has to happen.
		bool isPureCall(const IrInstr& instr, const std::unordered_set<std::string_view>& pureFunctions)
		{
			if (instr.opcode() != IrOpcode::Call)
				return false;
			const IrCallPayload& payload = instr.as<IrCallPayload>();
			return payload.hasResult && !payload.callee.empty() && pureFunctions.contains(payload.callee);
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

		// The truth of `x OP x` for a non-float x: true for ==, <= and >=, false for !=, < and >. A
		// float x is excluded, because NaN makes even `x == x` false.
		constexpr bool selfComparisonTruth(IrCmpPredicate predicate) noexcept
		{
			return predicate == IrCmpPredicate::Eq || predicate == IrCmpPredicate::Le || predicate == IrCmpPredicate::Ge;
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
							const auto& p = instr->as<IrCmpPayload>();
							// `x == x` is 1 and `x < x` is 0 for every integer or pointer x - this
							// shows up once load forwarding or copy propagation makes both operands
							// the same temporary. Floats are excluded (NaN).
							if (options.algebraicSimplification && !p.isFloat && p.lhs.isValid() && p.lhs == p.rhs)
							{
								ConstValue value{ false, selfComparisonTruth(p.predicate) ? 1 : 0, 0.0f };
								replacement = makeConst(arena, instr->location(), p.result, value);
								constants[p.result.id] = value;
								break;
							}
							if (!options.constantFolding)
								break;
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
							// No self-comparison CondJump branch is needed: a relational condition is
							// lowered as `Cmp` + `CondJump Ne(cmpResult, 0)` (ir_builder.cpp), so the
							// Cmp above folds `x OP x` to a constant and the constant path below then
							// resolves the branch. A CondJump's operands are never the same value.
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

		// ---- pass: sparse conditional constant propagation (SCCP) --------------------------------------
		//
		// foldFunction() folds constants, but it is not aware of which way a branch goes: it treats a
		// branch whose condition is known as any other fold, and it never asks what that means for the
		// blocks on the other side. That is what this pass adds, with the classic SCCP lattice and
		// worklist:
		//
		//   - every temporary gets a lattice value: Undefined (nothing known yet), Constant(v), or
		//     Overdefined (provably not a constant). A temporary defined more than once is Overdefined
		//     from the start, because this IR is deliberately not SSA - it reuses ids across the arms
		//     of a ternary or a materialized boolean.
		//   - the entry block is executable; a terminator marks its successor edges executable as soon
		//     as its condition is known: only the taken one when Constant, both when Overdefined, and
		//     neither while Undefined (the terminator is re-evaluated when an operand's lattice moves).
		//   - a worklist re-evaluates every instruction that reads a temporary whose lattice changed.
		//
		// The payoff is a branch on a known constant becoming a jump to the taken arm, and - the case
		// foldFunction cannot reach at all, because a switch dispatch is a TableJump and not a
		// comparison - a constant switch collapsing straight to its matching case. Unreachable-block
		// elimination then drops whatever those resolved branches left behind.
		//
		// Value folding itself is left to foldFunction(), which runs first in the round and already
		// turns an all-constant BinOp/UnOp/Cmp into a Const; the lattice only folds them so it can
		// know a branch condition. A Copy is deliberately not rewritten either - propagateCopies()
		// exists to erase it, and folding one into a Const here would hide the copy from that pass.

		struct SccpLattice
		{
			enum class Kind : u8 { Undefined, Constant, Overdefined };

			Kind kind = Kind::Undefined;
			ConstValue constant;
		};

		bool propagateConditionalConstants(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.conditionalConstants)
				return false;

			std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
			const usize blockCount = blocks.size();
			const usize tempCount = function.tempCount();
			if (blockCount == 0)
				return false;

			// How many times each temporary is defined, and which block each instruction lives in.
			// Only a temporary with exactly one definition can be a constant - see the note above.
			std::vector<u32> defCount(tempCount, 0);
			std::unordered_map<const IrInstr*, usize> instrBlock;
			std::unordered_map<const BasicBlock*, usize> blockIndex;
			for (usize bi = 0; bi < blockCount; ++bi)
			{
				blockIndex.emplace(blocks[bi].get(), bi);
				for (const IrInstr* instr : blocks[bi]->instrs())
				{
					instrBlock.emplace(instr, bi);
					if (IrValue result = resultOf(*instr); result.isValid() && result.id < tempCount)
						++defCount[result.id];
				}
			}

			// Which instructions read each temporary - so a change in its lattice re-evaluates them.
			std::vector<std::vector<IrInstr*>> users(tempCount);
			for (const auto& block : blocks)
				for (IrInstr* instr : block->instrs())
					forEachOperand(*instr, [&](IrValue value)
					{
						if (value.isValid() && value.id < tempCount)
							users[value.id].push_back(instr);
					});

			std::vector<SccpLattice> lattice(tempCount);
			for (usize t = 0; t < tempCount; ++t)
				if (defCount[t] != 1)
					lattice[t].kind = SccpLattice::Kind::Overdefined;

			std::vector<bool> executable(blockCount, false);
			std::vector<IrInstr*> worklist;
			std::unordered_set<const IrInstr*> queued;
			auto enqueue = [&](IrInstr* instr)
			{
				if (instr && queued.insert(instr).second)
					worklist.push_back(instr);
			};

			auto isTerminator = [](const IrInstr& instr)
			{
				switch (instr.opcode())
				{
					case IrOpcode::Jump:
					case IrOpcode::CondJump:
					case IrOpcode::TableJump:
					case IrOpcode::Return:
						return true;
					default:
						return false;
				}
			};

			// Marks a block executable (once) and queues its instructions. An unterminated block
			// falls through to the next one, exactly as successorsOf() reads the CFG.
			auto makeExecutable = [&](usize start)
			{
				usize bi = start;
				while (bi < blockCount && !executable[bi])
				{
					executable[bi] = true;
					std::span<IrInstr* const> instrs = blocks[bi]->instrs();
					for (IrInstr* instr : instrs)
						enqueue(instr);
					if (!instrs.empty() && isTerminator(*instrs.back()))
						break;
					++bi;
				}
			};

			SccpLattice overdefined;
			overdefined.kind = SccpLattice::Kind::Overdefined;
			auto latticeOf = [&](IrValue value) -> SccpLattice
			{
				if (!value.isValid() || value.id >= tempCount)
					return overdefined;
				return lattice[value.id];
			};

			auto constant = [](const ConstValue& value)
			{
				SccpLattice result;
				result.kind = SccpLattice::Kind::Constant;
				result.constant = value;
				return result;
			};

			auto evalBinOp = [&](const IrBinOpPayload& p) -> SccpLattice
			{
				const SccpLattice lhs = latticeOf(p.lhs);
				const SccpLattice rhs = latticeOf(p.rhs);
				if (lhs.kind == SccpLattice::Kind::Overdefined || rhs.kind == SccpLattice::Kind::Overdefined)
					return overdefined;
				if (lhs.kind == SccpLattice::Kind::Undefined || rhs.kind == SccpLattice::Kind::Undefined)
					return SccpLattice{};
				if (std::optional<ConstValue> folded = foldBinOp(p, lhs.constant, rhs.constant))
					return constant(*folded);
				return overdefined; // e.g. division by zero: the VM's result is not a compile-time constant
			};

			auto evalUnOp = [&](const IrUnOpPayload& p) -> SccpLattice
			{
				const SccpLattice operand = latticeOf(p.operand);
				if (operand.kind == SccpLattice::Kind::Overdefined)
					return overdefined;
				if (operand.kind == SccpLattice::Kind::Undefined)
					return SccpLattice{};
				if (std::optional<ConstValue> folded = foldUnOp(p, operand.constant))
					return constant(*folded);
				return overdefined;
			};

			auto evalCmp = [&](const IrCmpPayload& p) -> SccpLattice
			{
				// `x OP x` is a constant for an integer x whatever the operand's lattice says - the
				// two reads name the same value. A float is excluded because NaN makes it false.
				if (!p.isFloat && p.lhs.isValid() && p.lhs == p.rhs)
					return constant(ConstValue{ false, selfComparisonTruth(p.predicate) ? 1 : 0, 0.0f });
				const SccpLattice lhs = latticeOf(p.lhs);
				const SccpLattice rhs = latticeOf(p.rhs);
				if (lhs.kind == SccpLattice::Kind::Overdefined || rhs.kind == SccpLattice::Kind::Overdefined)
					return overdefined;
				if (lhs.kind == SccpLattice::Kind::Undefined || rhs.kind == SccpLattice::Kind::Undefined)
					return SccpLattice{};
				if (std::optional<bool> result = evaluatePredicate(p.predicate, p.isUnsigned, p.isFloat, lhs.constant, rhs.constant))
					return constant(ConstValue{ false, *result ? 1 : 0, 0.0f });
				return overdefined;
			};

			auto resolveCondition = [&](IrCmpPredicate predicate, bool isUnsigned, bool isFloat, IrValue lhs, IrValue rhs)
				-> std::optional<bool>
			{
				if (!isFloat && lhs.isValid() && lhs == rhs)
					return selfComparisonTruth(predicate);
				const SccpLattice l = latticeOf(lhs);
				const SccpLattice r = latticeOf(rhs);
				if (l.kind != SccpLattice::Kind::Constant || r.kind != SccpLattice::Kind::Constant)
					return std::nullopt;
				return evaluatePredicate(predicate, isUnsigned, isFloat, l.constant, r.constant);
			};

			auto enqueueUsers = [&](IrValue result)
			{
				if (!result.isValid() || result.id >= tempCount)
					return;
				for (IrInstr* user : users[result.id])
					enqueue(user);
			};

			auto joinResult = [&](IrValue result, const SccpLattice& value)
			{
				if (!result.isValid() || result.id >= tempCount)
					return;
				SccpLattice& slot = lattice[result.id];
				if (slot.kind == SccpLattice::Kind::Overdefined || value.kind == SccpLattice::Kind::Undefined)
					return;
				if (value.kind == SccpLattice::Kind::Overdefined)
				{
					slot.kind = SccpLattice::Kind::Overdefined;
					enqueueUsers(result);
					return;
				}
				if (slot.kind == SccpLattice::Kind::Undefined)
				{
					slot = value;
					enqueueUsers(result);
					return;
				}
				// Already Constant: a conflicting value would mean this temporary is not one.
				if (slot.constant.isFloat != value.constant.isFloat || slot.constant.intValue != value.constant.intValue ||
					slot.constant.floatValue != value.constant.floatValue)
				{
					slot.kind = SccpLattice::Kind::Overdefined;
					enqueueUsers(result);
				}
			};

			auto evaluate = [&](IrInstr* instr)
			{
				auto blockIt = instrBlock.find(instr);
				if (blockIt == instrBlock.end() || !executable[blockIt->second])
					return;

				auto mark = [&](BasicBlock* target)
				{
					if (auto it = blockIndex.find(target); it != blockIndex.end())
						makeExecutable(it->second);
				};

				switch (instr->opcode())
				{
					case IrOpcode::Const:
					{
						const auto& p = instr->as<IrConstPayload>();
						joinResult(p.result, constant(ConstValue{ p.isFloat, p.intValue, p.floatValue }));
						break;
					}
					case IrOpcode::BinOp: joinResult(instr->as<IrBinOpPayload>().result, evalBinOp(instr->as<IrBinOpPayload>())); break;
					case IrOpcode::UnOp: joinResult(instr->as<IrUnOpPayload>().result, evalUnOp(instr->as<IrUnOpPayload>())); break;
					case IrOpcode::Cmp: joinResult(instr->as<IrCmpPayload>().result, evalCmp(instr->as<IrCmpPayload>())); break;
					case IrOpcode::Copy: joinResult(instr->as<IrCopyPayload>().result, latticeOf(instr->as<IrCopyPayload>().source)); break;
					case IrOpcode::Jump: mark(instr->as<IrJumpPayload>().target); break;
					case IrOpcode::CondJump:
					{
						const auto& p = instr->as<IrCondJumpPayload>();
						if (p.trueTarget == p.falseTarget)
						{
							mark(p.trueTarget); // the condition is irrelevant when both arms land together
							break;
						}
						if (std::optional<bool> taken = resolveCondition(p.predicate, p.isUnsigned, p.isFloat, p.lhs, p.rhs))
							mark(*taken ? p.trueTarget : p.falseTarget);
						else if (latticeOf(p.lhs).kind == SccpLattice::Kind::Overdefined ||
							latticeOf(p.rhs).kind == SccpLattice::Kind::Overdefined)
						{
							mark(p.trueTarget);
							mark(p.falseTarget);
						}
						break;
					}
					case IrOpcode::TableJump:
					{
						const auto& p = instr->as<IrTableJumpPayload>();
						const SccpLattice discriminant = latticeOf(p.discriminant);
						if (discriminant.kind == SccpLattice::Kind::Undefined)
							break;
						if (discriminant.kind == SccpLattice::Kind::Overdefined)
						{
							for (u32 i = 0; i < p.entryCount; ++i)
								mark(p.targets[i]);
							mark(p.defaultTarget);
							break;
						}
						// The runtime computes `discriminant - low` in 32 bits and compares it
						// unsigned against entryCount (codegen.cpp), so mirror that exactly.
						u32 index = static_cast<u32>(discriminant.constant.intValue) - static_cast<u32>(p.low);
						mark(index < p.entryCount ? p.targets[index] : p.defaultTarget);
						break;
					}
					default:
						if (IrValue result = resultOf(*instr); result.isValid())
							joinResult(result, overdefined);
						break;
				}
			};

			makeExecutable(0);
			while (!worklist.empty())
			{
				IrInstr* instr = worklist.back();
				worklist.pop_back();
				queued.erase(instr);
				evaluate(instr);
			}

			// Only terminators are rewritten here: a branch on a known constant becomes a jump, and a
			// switch on a known constant becomes a jump straight to the matching case. Unreachable
			// blocks are left to removeUnreachableBlocks(), which reads the CFG these rewrites leave.
			bool changed = false;
			for (usize bi = 0; bi < blockCount; ++bi)
			{
				if (!executable[bi])
					continue;
				const auto& block = blocks[bi];
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;

				for (IrInstr* instr : block->instrs())
				{
					IrInstr* replacement = nullptr;
					if (instr->opcode() == IrOpcode::CondJump)
					{
						const auto& p = instr->as<IrCondJumpPayload>();
						if (std::optional<bool> taken = resolveCondition(p.predicate, p.isUnsigned, p.isFloat, p.lhs, p.rhs))
							replacement = arena.create<IrInstr>(instr->location(), IrJumpPayload{ *taken ? p.trueTarget : p.falseTarget });
					}
					else if (instr->opcode() == IrOpcode::TableJump)
					{
						const auto& p = instr->as<IrTableJumpPayload>();
						const SccpLattice discriminant = latticeOf(p.discriminant);
						if (discriminant.kind == SccpLattice::Kind::Constant)
						{
							u32 index = static_cast<u32>(discriminant.constant.intValue) - static_cast<u32>(p.low);
							BasicBlock* target = index < p.entryCount ? p.targets[index] : p.defaultTarget;
							replacement = arena.create<IrInstr>(instr->location(), IrJumpPayload{ target });
						}
					}

					if (replacement)
					{
						rewritten.push_back(replacement);
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

		// ---- pass: strength reduction ------------------------------------------------------------------
		//
		// A multiplication, division or remainder by a compile-time CONSTANT POWER OF TWO is one
		// shift or mask on any machine, and the arithmetic it replaces (imul/idiv/imod, and the
		// unsigned mul/div/mod) is the expensive part of a loop body. The rewrites:
		//
		//   x * 2^k  ->  x << k                          (a multiply wraps the same way, either sign)
		//   x /u 2^k ->  x >>u k                         (logical shift: the result is non-negative)
		//   x %u 2^k ->  x & (2^k - 1)
		//   x /s 2^k ->  (x + bias) >>s k                bias = ((x >>s 31) >>u (32-k)) is 2^k-1
		//   x %s 2^k ->  x - ((x /s 2^k) << k)           for a negative x and 0 otherwise, so the
		//                                                bias rounds toward -inf BEFORE the
		//                                                arithmetic shift and the quotient truncates
		//                                                toward zero exactly as C's / does
		//
		// The bias is built by shifting x's SIGN down, not x itself: `(x >>s 31) >>u (32-k)` is one
		// instruction pair and needs no wide mask (the only other way to spell 0/2^k-1), and it is
		// correct for every dividend - reading x's own top k bits is not.
		//
		// Only a power of two is recognized. The general "magic number" division needs a
		// 32x32 -> 64 multiply-high, and the IR has no opcode for one today (MULH/IMULH exist in
		// the ISA but nothing reaches them); dividing by 10 still goes through IDIV. See docs/14.
		bool rewriteStrength(const IrBinOpPayload& p, support::SourceLocation loc, IrFunction& function,
			support::Arena& arena, const std::unordered_map<u32, ConstValue>& constants, std::vector<IrInstr*>& out)
		{
			if (p.isFloat)
				return false;

			const ConstValue* lhs = findConstant(constants, p.lhs);
			const ConstValue* rhs = findConstant(constants, p.rhs);

			const ConstValue* constant = nullptr;
			IrValue value;
			if (p.op == IrBinOp::Mul)
			{
				// Commutative: whichever side is the constant leaves the other as the value.
				if (rhs && !lhs) { constant = rhs; value = p.lhs; }
				else if (lhs && !rhs) { constant = lhs; value = p.rhs; }
			}
			else if (p.op == IrBinOp::Div || p.op == IrBinOp::Mod)
			{
				// Not commutative: only a constant DIVISOR becomes a shift.
				if (rhs && !lhs) { constant = rhs; value = p.lhs; }
			}
			if (!constant || constant->isFloat)
				return false;

			u32 bits = static_cast<u32>(constant->intValue);
			if (!std::has_single_bit(bits))
				return false;
			u32 k = std::countr_zero(bits);
			if (k == 0)
				return false; // x*1 / x/1 / x%1 are the identity cases algebraic simplification owns

			// A signed divisor must be positive: a negative one would need an extra negation, and
			// INT_MIN is the one signed value with no positive counterpart.
			if (!p.isUnsigned && p.op != IrBinOp::Mul && static_cast<i32>(bits) <= 0)
				return false;

			auto emitConst = [&](i64 v) -> IrValue
			{
				IrValue result = function.newTemp();
				IrConstPayload c;
				c.result = result;
				c.intValue = v;
				out.push_back(arena.create<IrInstr>(loc, c));
				return result;
			};
			auto emitBin = [&](IrValue result, IrBinOp op, IrValue a, IrValue b)
			{
				IrBinOpPayload bop;
				bop.result = result;
				bop.op = op;
				bop.lhs = a;
				bop.rhs = b;
				out.push_back(arena.create<IrInstr>(loc, bop));
			};
			auto emitTemp = [&](IrBinOp op, IrValue a, IrValue b) -> IrValue
			{
				IrValue result = function.newTemp();
				emitBin(result, op, a, b);
				return result;
			};

			switch (p.op)
			{
				case IrBinOp::Mul:
				{
					IrValue shift = emitConst(k);
					emitBin(p.result, IrBinOp::Shl, value, shift);
					return true;
				}
				case IrBinOp::Div:
				{
					if (p.isUnsigned)
					{
						IrValue shift = emitConst(k);
						emitBin(p.result, IrBinOp::Shr, value, shift);
						return true;
					}
					// The bias is 2^k-1 for a negative dividend and 0 otherwise: x's sign (0 or -1),
					// shifted logically down, leaves 0 or the k low bits set. Reading x itself
					// instead (`x >>u (32-k)`) would take x's top k bits, which are all ones only in
					// a narrow range - a large-magnitude dividend would then round the wrong way.
					IrValue sign = emitTemp(IrBinOp::Sar, value, emitConst(31));
					IrValue bias = emitTemp(IrBinOp::Shr, sign, emitConst(32 - k));
					IrValue sum = emitTemp(IrBinOp::Add, value, bias);
					emitBin(p.result, IrBinOp::Sar, sum, emitConst(k));
					return true;
				}
				case IrBinOp::Mod:
				{
					if (p.isUnsigned)
					{
						emitBin(p.result, IrBinOp::And, value, emitConst(static_cast<i64>(bits) - 1));
						return true;
					}
					IrValue sign = emitTemp(IrBinOp::Sar, value, emitConst(31));
					IrValue bias = emitTemp(IrBinOp::Shr, sign, emitConst(32 - k));
					IrValue sum = emitTemp(IrBinOp::Add, value, bias);
					// The quotient is read again below, so it needs a temporary of its own rather
					// than p.result.
					IrValue quotient = emitTemp(IrBinOp::Sar, sum, emitConst(k));
					IrValue scaled = emitTemp(IrBinOp::Shl, quotient, emitConst(k));
					emitBin(p.result, IrBinOp::Sub, value, scaled);
					return true;
				}
				default:
					return false;
			}
		}

		bool reduceStrength(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.strengthReduction)
				return false;

			std::unordered_map<u32, ConstValue> constants = collectConstants(function);
			bool changedAtAll = false;

			for (const auto& block : function.blocks())
			{
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;

				for (IrInstr* instr : block->instrs())
				{
					if (instr->opcode() != IrOpcode::BinOp)
					{
						rewritten.push_back(instr);
						continue;
					}

					std::vector<IrInstr*> replacement;
					if (rewriteStrength(instr->as<IrBinOpPayload>(), instr->location(), function, arena, constants, replacement))
					{
						rewritten.insert(rewritten.end(), replacement.begin(), replacement.end());
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
					changedAtAll = true;
				}
			}
			return changedAtAll;
		}

		// ---- pass: common subexpression elimination ----------------------------------------------------
		//
		// renameOperands() is defined with the copy-propagation pass further down; CSE reuses it to
		// apply its own substitutions, so it is forward-declared here.
		IrInstr* renameOperands(const IrInstr& instr, support::Arena& arena, const std::unordered_map<u32, IrValue>& replacement);

		// If a pure expression was already computed in this block, computing it again is wasted. The
		// pass value-numbers each block: an instruction whose operation and (already canonicalized)
		// operands match one seen earlier is replaced by a reference to the earlier result, and the
		// duplicate is dropped.
		//
		// Two guards make this sound on a NON-SSA IR:
		//   - a temporary can be defined more than once (materializeBoolean(), visit(TernaryExpr&)),
		//     so only a result with exactly one definition, and whose operands each have exactly one,
		//     is numbered - its value cannot change underneath the key;
		//   - a duplicate may be dropped only when its result is used solely in the CURRENT block (or
		//     nowhere), so no other block is left naming a definition that is gone.
		// Loads and stores are never numbered - they read and write memory - and a Call is never
		// reused.

		// A structural key for a pure instruction: a tag, up to three small fields (opcode/flags/
		// width), and up to two operand ids. A POD, so numbering allocates nothing per instruction.
		// GlobalAddr is deliberately not numbered: its key would be a name, which this fixed key
		// cannot hold, and a repeated global address is rare anyway.
		struct CseKey
		{
			u8 tag = 0, f0 = 0, f1 = 0, f2 = 0;
			u32 x = 0, y = 0;
			constexpr bool operator==(const CseKey&) const noexcept = default;
		};

		struct CseKeyHash
		{
			usize operator()(const CseKey& key) const noexcept
			{
				usize hash = 1469598103934665603ull; // FNV-1a
				auto mix = [&](u64 value) { hash = (hash ^ value) * 1099511628211ull; };
				mix(key.tag); mix(key.f0); mix(key.f1); mix(key.f2); mix(key.x); mix(key.y);
				return hash;
			}
		};

		// True (and fills `out`) only for an opcode that is pure by construction AND whose operands
		// are each defined exactly once, so their value cannot change underneath the key.
		bool cseNumberable(const IrInstr& instr, const std::vector<u32>& defCount, usize tempCount, CseKey& out)
		{
			auto stable = [&](IrValue value)
			{
				return !value.isValid() || (value.id < tempCount && defCount[value.id] == 1);
			};
			switch (instr.opcode())
			{
				case IrOpcode::BinOp:
				{
					const auto& p = instr.as<IrBinOpPayload>();
					if (!stable(p.lhs) || !stable(p.rhs))
						return false;
					out = CseKey{ 1, static_cast<u8>(p.op), static_cast<u8>(p.isUnsigned), static_cast<u8>(p.isFloat), p.lhs.id, p.rhs.id };
					return true;
				}
				case IrOpcode::UnOp:
				{
					const auto& p = instr.as<IrUnOpPayload>();
					if (!stable(p.operand))
						return false;
					out = CseKey{ 2, static_cast<u8>(p.op), static_cast<u8>(p.isFloat), static_cast<u8>(p.isUnsigned),
						p.operand.id, static_cast<u32>(p.narrowSize) };
					return true;
				}
				case IrOpcode::Cmp:
				{
					const auto& p = instr.as<IrCmpPayload>();
					if (!stable(p.lhs) || !stable(p.rhs))
						return false;
					out = CseKey{ 3, static_cast<u8>(p.predicate), static_cast<u8>(p.isUnsigned), static_cast<u8>(p.isFloat), p.lhs.id, p.rhs.id };
					return true;
				}
				case IrOpcode::Builtin:
				{
					const auto& p = instr.as<IrBuiltinPayload>();
					if (!stable(p.a) || !stable(p.b))
						return false;
					out = CseKey{ 4, static_cast<u8>(p.builtin), 0, 0, p.a.id, p.b.id };
					return true;
				}
				case IrOpcode::FrameAddr:
					out = CseKey{ 5, 0, 0, 0, instr.as<IrFrameAddrPayload>().localIndex, 0 };
					return true;
				case IrOpcode::VaStart:
					out = CseKey{ 6, 0, 0, 0, 0, 0 };
					return true;
				default:
					return false; // Const/Copy/Load/Store/Call/... : not numbered
			}
		}

		bool eliminateCommonSubexpressions(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.commonSubexpressionElimination)
				return false;

			usize tempCount = function.tempCount();

			// How many times each temporary is defined - see the soundness note above.
			std::vector<u32> defCount(tempCount, 0);
			for (const auto& block : function.blocks())
				for (const IrInstr* instr : block->instrs())
					if (IrValue result = resultOf(*instr); result.isValid() && result.id < tempCount)
						++defCount[result.id];

			// The single block each temporary is USED in: kNoUse if unused, kMultiUse if used in more
			// than one. A duplicate is only droppable when its result is used solely in the current
			// block (or nowhere).
			constexpr u32 kNoUse = 0xFFFFFFFEu;
			constexpr u32 kMultiUse = 0xFFFFFFFFu;
			std::vector<u32> useBlock(tempCount, kNoUse);
			for (usize bi = 0; bi < function.blocks().size(); ++bi)
			{
				u32 b = static_cast<u32>(bi);
				std::vector<bool> seen(tempCount, false);
				for (const IrInstr* instr : function.blocks()[bi]->instrs())
					forEachOperand(*instr, [&](IrValue value)
					{
						if (!value.isValid() || value.id >= tempCount || seen[value.id])
							return;
						seen[value.id] = true;
						u32& slot = useBlock[value.id];
						if (slot == kNoUse)
							slot = b;
						else if (slot != b)
							slot = kMultiUse;
					});
			}

			bool changedAtAll = false;
			for (usize bi = 0; bi < function.blocks().size(); ++bi)
			{
				u32 b = static_cast<u32>(bi);
				const auto& block = function.blocks()[bi];
				std::unordered_map<CseKey, IrValue, CseKeyHash> available; // expression -> first result
				std::unordered_map<u32, IrValue> substitution;             // duplicate result -> canonical
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;

				for (IrInstr* instr : block->instrs())
				{
					IrInstr* current = instr;
					if (IrInstr* renamed = renameOperands(*instr, arena, substitution))
						current = renamed;

					IrValue result = resultOf(*current);
					CseKey key;
					if (result.isValid() && result.id < tempCount && defCount[result.id] == 1 &&
						cseNumberable(*current, defCount, tempCount, key))
					{
						auto found = available.find(key);
						if (found != available.end() && (useBlock[result.id] == kNoUse || useBlock[result.id] == b))
						{
							substitution[result.id] = found->second;
							blockChanged = true;
							continue; // the earlier result is this one's value - drop it
						}
						if (found == available.end())
							available.emplace(key, result);
					}

					rewritten.push_back(current);
				}

				if (blockChanged)
				{
					block->replaceInstrs(std::move(rewritten));
					changedAtAll = true;
				}
			}
			return changedAtAll;
		}

		// ---- pass: block layout ------------------------------------------------------------------------
		//
		// The back end emits blocks in the order they appear and drops a `jp` whose target is the
		// next block emitted (codegen's fallthroughBranches). Nothing reordered them, so an if/else
		// or a loop left jumps that could have been fall-throughs. This walks a trace: from a block,
		// follow the successor it would rather fall into (a Jump's target, a CondJump's false arm,
		// which is the arm codegen already falls into) while that successor is unvisited, then start
		// a new trace at the next unvisited block.
		//
		// Only reorders - block ids and every BasicBlock* are untouched. Refuses unless every block
		// is terminated, so no block's implicit fall-through successor changes.
		bool layoutBlocks(IrFunction& function, const OptimizationOptions& options)
		{
			if (!options.blockLayout)
				return false;

			std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
			if (blocks.size() <= 1)
				return false;
			for (const auto& block : blocks)
				if (!block->isTerminated())
					return false;

			// The successor a block would rather fall into. A Jump wants its target; a CondJump
			// wants its false arm, which is the arm codegen already falls into - UNLESS the true arm
			// is itself just `Jump falseTarget` (the common `if (cond) { body }` shape). Following
			// the false arm then would sink the body (the common path) to the end and turn its
			// trailing jump into an explicit one, so the true arm is followed instead.
			auto preferred = [](BasicBlock* block) -> BasicBlock*
			{
				std::span<IrInstr* const> instrs = block->instrs();
				const IrInstr& last = *instrs.back();
				if (last.opcode() == IrOpcode::Jump)
					return last.as<IrJumpPayload>().target;
				if (last.opcode() == IrOpcode::CondJump)
				{
					const IrCondJumpPayload& p = last.as<IrCondJumpPayload>();
					std::span<IrInstr* const> trueArm = p.trueTarget->instrs();
					if (!trueArm.empty() && trueArm.back()->opcode() == IrOpcode::Jump &&
						trueArm.back()->as<IrJumpPayload>().target == p.falseTarget)
						return p.trueTarget;
					return p.falseTarget;
				}
				return nullptr; // a Return or a TableJump has no fall-through
			};

			// How many explicit jumps an order leaves that cannot be dropped: a Jump whose target is
			// not the next block, and a CondJump where neither arm is (codegen can branch to one arm
			// and fall through the other - either arm for an integer compare, but only the false arm
			// for a float one, because NaN makes the predicates non-complementary).
			auto explicitJumps = [](std::span<BasicBlock* const> order) -> u32
			{
				u32 total = 0;
				for (usize i = 0; i < order.size(); ++i)
				{
					BasicBlock* next = (i + 1 < order.size()) ? order[i + 1] : nullptr;
					std::span<IrInstr* const> instrs = order[i]->instrs();
					if (instrs.empty())
						continue;
					const IrInstr& last = *instrs.back();
					if (last.opcode() == IrOpcode::Jump)
					{
						if (last.as<IrJumpPayload>().target != next)
							++total;
					}
					else if (last.opcode() == IrOpcode::CondJump)
					{
						const IrCondJumpPayload& p = last.as<IrCondJumpPayload>();
						bool fallsThrough = p.falseTarget == next || (!p.isFloat && p.trueTarget == next);
						if (!fallsThrough)
							++total;
					}
				}
				return total;
			};

			std::vector<BasicBlock*> current;
			current.reserve(blocks.size());
			for (const auto& block : blocks)
				current.push_back(block.get());

			std::unordered_set<const BasicBlock*> visited;
			std::vector<BasicBlock*> order;
			order.reserve(blocks.size());

			auto trace = [&](BasicBlock* start)
			{
				BasicBlock* currentBlock = start;
				while (currentBlock && !visited.contains(currentBlock))
				{
					visited.insert(currentBlock);
					order.push_back(currentBlock);
					BasicBlock* next = preferred(currentBlock);
					currentBlock = (next && !visited.contains(next)) ? next : nullptr;
				}
			};

			trace(blocks.front().get()); // the entry block first
			for (const auto& block : blocks)
				if (!visited.contains(block.get()))
					trace(block.get());
			if (order.size() != blocks.size())
				return false;

			// Commit only when the new order is no worse in explicit jumps - never trade a
			// fall-through away for a reordering that does not pay for it.
			if (explicitJumps(order) > explicitJumps(current))
				return false;

			for (usize i = 0; i < order.size(); ++i)
			{
				if (order[i] != blocks[i].get())
				{
					function.reorderBlocks(order);
					return true;
				}
			}
			return false;
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
				case IrOpcode::TableJump:
				{
					IrTableJumpPayload p = instr.as<IrTableJumpPayload>();
					p.discriminant = rename(p.discriminant);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Builtin:
				{
					// Operands only: renaming a definition is the inliner's remapInstr, not this
					// pass. (The map is keyed by single-definition Copy results, so p.result can
					// never be a key here anyway.)
					IrBuiltinPayload p = instr.as<IrBuiltinPayload>();
					p.a = rename(p.a);
					p.b = rename(p.b);
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

		// ---- pass: restrict load forwarding ----------------------------------------------------------

		// A load through a `restrict` pointer forwards from the most recent store through the SAME
		// pointer, whatever stores through other pointers sit in between - restrict is the promise
		// that no other pointer aliases the pointee. Only the direct form is recognized: the address
		// has to be a load of a restrict pointer local's value (`*p`, where p is a restrict parameter
		// or local). Pointer arithmetic (`p[i]`) has a computed address this pass does not trace back
		// to the pointer, so it is simply not forwarded - a future pass can extend this.
		//
		// A store through any pointer other than the same restrict one, and any call, forgets every
		// entry: such a store may alias a restrict pointee, and the compiler must stay correct when
		// the pointer it is looking at is not provably the restrict pointer itself.
		bool forwardRestrictLoads(IrFunction& function, support::Arena& arena, const OptimizationOptions& options)
		{
			if (!options.loadForwarding)
				return false;

			std::span<const IrLocalSlot> localSlots = function.localSlots();
			std::vector<u32> frameAddrLocal = mapFrameAddrTemps(function);

			// The instruction that defined each temporary - what lets this pass tell "the address is
			// p's value" from a Load whose address is a FrameAddr naming a restrict local.
			std::vector<const IrInstr*> definer(function.tempCount(), nullptr);
			for (const auto& block : function.blocks())
				for (const IrInstr* instr : block->instrs())
					if (IrValue result = resultOf(*instr); result.isValid() && result.id < definer.size())
						definer[result.id] = instr;

			auto restrictLocalOf = [&](IrValue address) -> u32
			{
				if (!address.isValid() || address.id >= definer.size())
					return ~0u;
				const IrInstr* def = definer[address.id];
				if (!def || def->opcode() != IrOpcode::Load)
					return ~0u;
				IrValue addr = def->as<IrLoadPayload>().address;
				if (!addr.isValid() || addr.id >= frameAddrLocal.size())
					return ~0u;
				u32 local = frameAddrLocal[addr.id];
				if (local == ~0u || local >= localSlots.size() || !localSlots[local].isRestrict)
					return ~0u;
				return local;
			};

			struct Known { IrValue value; IrMemSize size; bool isFloat; };

			bool changed = false;
			for (const auto& block : function.blocks())
			{
				std::unordered_map<u32, Known> known;
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(block->instrs().size());
				bool blockChanged = false;

				for (IrInstr* instr : block->instrs())
				{
					if (instr->opcode() == IrOpcode::Store)
					{
						const auto& p = instr->as<IrStorePayload>();
						u32 local = restrictLocalOf(p.address);
						if (local != ~0u)
						{
							// A volatile store's effect cannot be named, so whatever was known stops
							// being true; an ordinary one is the new known value.
							if (p.isVolatile)
								known.erase(local);
							else
								known[local] = Known{ p.value, p.size, p.isFloat };
						}
						else
						{
							known.clear(); // a store through any other pointer may alias
						}
						rewritten.push_back(instr);
						continue;
					}

					if (instr->opcode() == IrOpcode::Load)
					{
						const auto& p = instr->as<IrLoadPayload>();
						u32 local = restrictLocalOf(p.address);
						auto it = (local == ~0u) ? known.end() : known.find(local);
						if (local != ~0u && !p.isVolatile && it != known.end() &&
							p.size == it->second.size && p.isFloat == it->second.isFloat)
						{
							rewritten.push_back(makeCopy(arena, instr->location(), p.result, it->second.value, p.isFloat));
							blockChanged = true;
							continue;
						}
						rewritten.push_back(instr);
						continue;
					}

					if (instr->opcode() == IrOpcode::Call)
						known.clear(); // the callee may write through any pointer it can reach

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
			// therefore dead. That whole-local case is one half of this pass (and the one inlining
			// produces: a parameter copied in and then forwarded away entirely); the other half,
			// below, drops a store a later store overwrites before any read, within a block.
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

			// Per-store, within a block: a full-width store to a non-escaping local that a later
			// full-width store overwrites before any load of it is dead - the earlier value is
			// never observed. Narrower stores are not tracked (a reinterpretation through a cast
			// only touches part of the slot), and a load between two stores makes the earlier one
			// observable. Nothing else can write a non-escaping local, so a call in between is
			// irrelevant.
			bool changed = false;
			for (const auto& block : function.blocks())
			{
				std::vector<IrInstr*> live;
				live.reserve(block->instrs().size());
				std::unordered_map<u32, usize> lastFullStore; // local -> index into `live`
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
					if (instr->opcode() == IrOpcode::Load)
					{
						if (u32 local = localOf(instr->as<IrLoadPayload>().address); local != ~0u)
							lastFullStore.erase(local); // the preceding store is now read
						live.push_back(instr);
						continue;
					}
					if (instr->opcode() != IrOpcode::Store)
					{
						live.push_back(instr);
						continue;
					}

					const auto& store = instr->as<IrStorePayload>();
					u32 local = localOf(store.address);
					if (local == ~0u || function.localSlots()[local].isVolatile || store.isVolatile)
					{
						live.push_back(instr);
						continue;
					}
					if (!everRead[local])
					{
						blockChanged = true; // never read anywhere - dead by the whole-local rule
						continue;
					}
					const IrLocalSlot& slot = function.localSlots()[local];
					bool fullWidth = store.size == irMemSizeForBytes(slot.sizeInBytes) && store.isFloat == slot.isFloat;
					if (!fullWidth)
					{
						lastFullStore.erase(local);
						live.push_back(instr);
						continue;
					}
					if (auto it = lastFullStore.find(local); it != lastFullStore.end())
					{
						live[it->second] = nullptr; // that earlier store is overwritten unread
						blockChanged = true;
					}
					live.push_back(instr);
					lastFullStore[local] = live.size() - 1;
				}

				if (blockChanged)
				{
					std::vector<IrInstr*> kept;
					kept.reserve(live.size());
					for (IrInstr* instr : live)
						if (instr != nullptr)
							kept.push_back(instr);
					block->replaceInstrs(std::move(kept));
					changed = true;
				}
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
				// Likewise one whose address a data initializer holds: that reference is bytes in the
				// image, not an instruction, so no walk over the bodies below could ever find it.
				if (function->name() != "main" && !function->hasExternalLinkage() && !function->isInterruptHandler() &&
					!function->isAddressTakenByData())
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
						// An indirect call names nothing, so it adds no edge of its own - what keeps
						// its target alive is the GlobalAddr that produced the address, below.
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
				else if (last->opcode() == IrOpcode::TableJump)
				{
					// The table's entries deserve the same treatment a branch target gets: a `case`
					// label whose body is empty is a block that only jumps onward, and threading it
					// out is what lets several entries collapse onto one real body.
					const auto& p = last->as<IrTableJumpPayload>();
					std::vector<BasicBlock*> resolved(p.entryCount);
					bool anyChanged = false;
					for (u32 i = 0; i < p.entryCount; ++i)
					{
						resolved[i] = resolveJumpTarget(p.targets[i], blockCount);
						anyChanged = anyChanged || resolved[i] != p.targets[i];
					}
					BasicBlock* resolvedDefault = resolveJumpTarget(p.defaultTarget, blockCount);
					anyChanged = anyChanged || resolvedDefault != p.defaultTarget;
					if (anyChanged)
					{
						auto** targets = static_cast<BasicBlock**>(arena.allocate(p.entryCount * sizeof(BasicBlock*)));
						for (u32 i = 0; i < p.entryCount; ++i)
							targets[i] = resolved[i];
						IrTableJumpPayload rewritten = p;
						rewritten.targets = targets;
						rewritten.defaultTarget = resolvedDefault;
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

		// ---- pass: loop-invariant code motion (LICM) ---------------------------------------------------
		//
		// A natural loop is a back edge `u -> v` together with every block that reaches `u` without
		// passing through `v`; `v` is the loop header. Finding one needs a dominator tree (a back edge
		// is an edge into a block that dominates its source), which this pass computes on its own -
		// nothing else in the IR asks for one today.
		//
		// The one transformation is hoisting: an instruction inside the loop whose operands are all
		// defined outside it (or are themselves hoisted) moves into the loop's PREHEADER - the single
		// block outside the loop whose only successor is the header, and the header's only entry from
		// outside. Only side-effect-free instructions that cannot fault are hoisted, so executing one
		// more often than the loop would have is unobservable: division and remainder are excluded
		// because the VM raises Trap on a zero divisor, and a float-to-int conversion because it is
		// undefined out of range - hoisting either could make a loop that never reaches it fault or
		// misbehave.
		//
		// A loop with no such preheader (its header has several entries from outside, or the unique
		// one has another successor) is skipped rather than given one: inserting a block would change
		// the CFG for every later pass, and a hoist into a block that can also leave the loop could
		// read an operand that was never defined on that path.

		struct NaturalLoop
		{
			usize header = 0;
			usize preheader = ~usize(0);
			std::vector<bool> blocks; // indexed by block index
		};

		// A dominator set as a bitset, so the dataflow intersection is O(blocks/64) rather than
		// O(blocks) - a function with many blocks is otherwise cubic in the fixpoint below.
		struct BlockSet
		{
			std::vector<u64> words;

			explicit BlockSet(usize count = 0) : words((count + 63) / 64, 0) {}
			void set(usize i) { words[i / 64] |= 1ull << (i % 64); }
			bool test(usize i) const { return (words[i / 64] >> (i % 64)) & 1ull; }
			void setAll(usize count)
			{
				words.assign((count + 63) / 64, ~0ull);
				if (count % 64)
					words.back() = (1ull << (count % 64)) - 1;
			}
			void intersectWith(const BlockSet& other)
			{
				for (usize i = 0; i < words.size(); ++i)
					words[i] &= other.words[i];
			}
			bool operator==(const BlockSet&) const noexcept = default;
		};

		std::vector<BlockSet> computeDominators(usize blockCount, const std::vector<std::vector<usize>>& predecessors)
		{
			std::vector<BlockSet> dom(blockCount);
			for (usize b = 0; b < blockCount; ++b)
			{
				dom[b] = BlockSet(blockCount);
				dom[b].setAll(blockCount); // optimistic: every block dominates every other, until proven otherwise
				dom[b].set(b);
			}
			dom[0] = BlockSet(blockCount);
			dom[0].set(0); // only the entry dominates the entry

			bool changed = true;
			while (changed)
			{
				changed = false;
				for (usize b = 1; b < blockCount; ++b)
				{
					BlockSet next(blockCount);
					if (predecessors[b].empty())
					{
						next.set(b); // unreachable: only itself
					}
					else
					{
						next = dom[predecessors[b][0]];
						for (usize k = 1; k < predecessors[b].size(); ++k)
							next.intersectWith(dom[predecessors[b][k]]);
						next.set(b);
					}
					if (!(next == dom[b]))
					{
						dom[b] = std::move(next);
						changed = true;
					}
				}
			}
			return dom;
		}

		bool isTerminatorInstr(const IrInstr& instr)
		{
			switch (instr.opcode())
			{
				case IrOpcode::Jump:
				case IrOpcode::CondJump:
				case IrOpcode::TableJump:
				case IrOpcode::Return:
					return true;
				default:
					return false;
			}
		}

		// True for an instruction LICM may hoist: pure, and unable to fault. Loads are excluded (they
		// read memory that a store in the loop may have written), as are calls and stores.
		bool isHoistableInstruction(const IrInstr& instr) noexcept
		{
			switch (instr.opcode())
			{
				case IrOpcode::BinOp:
				{
					const IrBinOp op = instr.as<IrBinOpPayload>().op;
					return op != IrBinOp::Div && op != IrBinOp::Mod; // a zero divisor would raise Trap
				}
				case IrOpcode::UnOp:
					return instr.as<IrUnOpPayload>().op != IrUnOp::FloatToInt; // undefined out of range
				case IrOpcode::Cmp:
				case IrOpcode::Copy:
				case IrOpcode::FrameAddr:
				case IrOpcode::GlobalAddr:
				case IrOpcode::Builtin:
					return true;
				default:
					return false;
			}
		}

		bool hoistLoopInvariants(IrFunction& function, const OptimizationOptions& options)
		{
			if (!options.loopInvariantMotion)
				return false;

			std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
			const usize blockCount = blocks.size();
			const usize tempCount = function.tempCount();
			if (blockCount < 2)
				return false;

			std::unordered_map<const BasicBlock*, usize> indexOf;
			for (usize i = 0; i < blockCount; ++i)
				indexOf.emplace(blocks[i].get(), i);

			std::vector<std::vector<usize>> predecessors(blockCount);
			std::vector<std::vector<usize>> successors(blockCount);
			for (usize i = 0; i < blockCount; ++i)
			{
				for (BasicBlock* successor : successorsOf(function, i))
				{
					auto it = indexOf.find(successor);
					if (it == indexOf.end())
						continue;
					successors[i].push_back(it->second);
					predecessors[it->second].push_back(i);
				}
			}

			std::vector<u32> defCount(tempCount, 0);
			std::vector<usize> defBlock(tempCount, ~usize(0));
			for (usize i = 0; i < blockCount; ++i)
			{
				for (const IrInstr* instr : blocks[i]->instrs())
				{
					if (IrValue result = resultOf(*instr); result.isValid() && result.id < tempCount)
					{
						++defCount[result.id];
						defBlock[result.id] = i;
					}
				}
			}

			std::vector<BlockSet> dom = computeDominators(blockCount, predecessors);

			// What a hoisted load needs: the local it reads must not escape (so no pointer and no
			// call can write it) and must not be stored anywhere in the loop (so its value is the
			// same on every iteration) - see the load case in the hoist test below.
			std::vector<bool> nonEscaping = collectNonEscapingLocals(function);
			std::vector<u32> frameAddrLocal = mapFrameAddrTemps(function);

			// Every natural loop, merged by header (a header with two latches is one loop).
			std::vector<NaturalLoop> loops;
			auto loopForHeader = [&](usize header) -> NaturalLoop&
			{
				for (NaturalLoop& loop : loops)
					if (loop.header == header)
						return loop;
				loops.push_back(NaturalLoop{ header, ~usize(0), std::vector<bool>(blockCount, false) });
				loops.back().blocks[header] = true;
				return loops.back();
			};

			for (usize u = 0; u < blockCount; ++u)
			{
				for (usize v : successors[u])
				{
					if (!dom[u].test(v))
						continue; // not a back edge: v does not dominate u
					NaturalLoop& loop = loopForHeader(v);
					loop.blocks[u] = true;
					// Everything that reaches the latch `u` without passing through the header `v`.
					std::vector<usize> stack{ u };
					while (!stack.empty())
					{
						usize x = stack.back();
						stack.pop_back();
						if (x == v)
							continue; // never expand the header's own predecessors
						for (usize p : predecessors[x])
						{
							if (!loop.blocks[p])
							{
								loop.blocks[p] = true;
								stack.push_back(p);
							}
						}
					}
				}
			}

			for (NaturalLoop& loop : loops)
			{
				usize outside = 0;
				for (usize p : predecessors[loop.header])
					if (!loop.blocks[p])
						++outside;
				if (outside != 1)
					continue; // several entries from outside: nowhere to hoist to
				for (usize p : predecessors[loop.header])
				{
					if (loop.blocks[p])
						continue;
					if (successors[p].size() == 1 && successors[p][0] == loop.header)
						loop.preheader = p;
					break;
				}
			}

			// Innermost first, so a value hoisted into an inner preheader can be hoisted further by
			// an enclosing loop in the same pass.
			std::sort(loops.begin(), loops.end(), [](const NaturalLoop& a, const NaturalLoop& b)
			{
				return std::count(a.blocks.begin(), a.blocks.end(), true) <
					std::count(b.blocks.begin(), b.blocks.end(), true);
			});

			bool changedAtAll = false;
			for (const NaturalLoop& loop : loops)
			{
				if (loop.preheader == ~usize(0))
					continue;

				std::vector<bool> definedInLoop(tempCount, false);
				for (usize i = 0; i < blockCount; ++i)
					if (loop.blocks[i])
						for (const IrInstr* instr : blocks[i]->instrs())
							if (IrValue result = resultOf(*instr); result.isValid() && result.id < tempCount)
								definedInLoop[result.id] = true;

				// A value hoisted into the preheader stays live for the whole loop, so it is live
				// across any Call the loop makes - and the allocator's own "does this live range
				// span a call" test is linear in emission order, which a back edge fools. Refuse a
				// loop that calls anything rather than hand the allocator a range it would put in a
				// caller-saved register that the call then clobbers. (A hoisted value whose every use
				// is inside the loop is dead once the loop ends, so a call OUTSIDE it is harmless.)
				bool loopHasCall = false;
				for (usize i = 0; i < blockCount && !loopHasCall; ++i)
					if (loop.blocks[i])
						for (const IrInstr* instr : blocks[i]->instrs())
							if (instr->opcode() == IrOpcode::Call)
							{
								loopHasCall = true;
								break;
							}
				if (loopHasCall)
					continue;

				std::vector<bool> usedOutsideLoop(tempCount, false);
				for (usize i = 0; i < blockCount; ++i)
				{
					if (loop.blocks[i])
						continue;
					for (const IrInstr* instr : blocks[i]->instrs())
						forEachOperand(*instr, [&](IrValue value)
						{
							if (value.isValid() && value.id < tempCount)
								usedOutsideLoop[value.id] = true;
						});
				}

				// Which non-escaping local the loop writes. A load of one it does NOT write is the
				// same value on every iteration, so it may be hoisted too.
				std::vector<bool> storedInLoop(function.localCount(), false);
				for (usize i = 0; i < blockCount; ++i)
					if (loop.blocks[i])
						for (const IrInstr* instr : blocks[i]->instrs())
							if (instr->opcode() == IrOpcode::Store)
							{
								IrValue address = instr->as<IrStorePayload>().address;
								if (address.isValid() && address.id < frameAddrLocal.size() && frameAddrLocal[address.id] != ~0u)
									storedInLoop[frameAddrLocal[address.id]] = true;
							}

				auto hoistable = [&](const IrInstr& instr) -> bool
				{
					if (instr.opcode() != IrOpcode::Load)
						return isHoistableInstruction(instr);
					const IrLoadPayload& load = instr.as<IrLoadPayload>();
					if (load.isVolatile || !load.address.isValid() || load.address.id >= frameAddrLocal.size())
						return false;
					u32 local = frameAddrLocal[load.address.id];
					return local != ~0u && local < nonEscaping.size() && nonEscaping[local] && !storedInLoop[local];
				};

				// A temporary is invariant when it is defined outside the loop by a single
				// definition that dominates the preheader, or when it is itself being hoisted. A
				// second definition makes the value ambiguous (this IR is not SSA), so it is not
				// hoisted over.
				auto invariant = [&](IrValue value, const std::unordered_set<u32>& hoisted)
				{
					if (!value.isValid() || value.id >= tempCount)
						return true;
					if (hoisted.contains(value.id))
						return true;
					if (definedInLoop[value.id])
						return false;
					return defCount[value.id] == 1 && dom[loop.preheader].test(defBlock[value.id]);
				};

				std::unordered_set<u32> hoisted;
				std::vector<IrInstr*> order;
				bool progress = true;
				while (progress)
				{
					progress = false;
					for (usize i = 0; i < blockCount; ++i)
					{
						if (!loop.blocks[i])
							continue;
						for (IrInstr* instr : blocks[i]->instrs())
						{
							IrValue result = resultOf(*instr);
							if (!result.isValid() || result.id >= tempCount || hoisted.contains(result.id))
								continue;
							if (usedOutsideLoop[result.id])
								continue; // hoisting would extend its live range past the loop
							if (defCount[result.id] != 1 || !hoistable(*instr))
								continue;
							bool allInvariant = true;
							forEachOperand(*instr, [&](IrValue value)
							{
								if (!invariant(value, hoisted))
									allInvariant = false;
							});
							if (!allInvariant)
								continue;
							hoisted.insert(result.id);
							order.push_back(instr);
							progress = true;
						}
					}
				}

				if (order.empty())
					continue;

				std::unordered_set<const IrInstr*> moving(order.begin(), order.end());
				for (usize i = 0; i < blockCount; ++i)
				{
					if (!loop.blocks[i])
						continue;
					std::span<IrInstr* const> instrs = blocks[i]->instrs();
					std::vector<IrInstr*> kept;
					kept.reserve(instrs.size());
					for (IrInstr* instr : instrs)
						if (!moving.contains(instr))
							kept.push_back(instr);
					if (kept.size() != instrs.size())
						blocks[i]->replaceInstrs(std::move(kept));
				}

				std::span<IrInstr* const> pre = blocks[loop.preheader]->instrs();
				usize insertAt = (!pre.empty() && isTerminatorInstr(*pre.back())) ? pre.size() - 1 : pre.size();
				std::vector<IrInstr*> rebuilt;
				rebuilt.reserve(pre.size() + order.size());
				rebuilt.insert(rebuilt.end(), pre.begin(), pre.begin() + static_cast<std::ptrdiff_t>(insertAt));
				rebuilt.insert(rebuilt.end(), order.begin(), order.end());
				rebuilt.insert(rebuilt.end(), pre.begin() + static_cast<std::ptrdiff_t>(insertAt), pre.end());
				blocks[loop.preheader]->replaceInstrs(std::move(rebuilt));
				changedAtAll = true;
			}
			return changedAtAll;
		}

		// ---- pass: dead code elimination -------------------------------------------------------------

		bool eliminateDeadCode(IrFunction& function, const OptimizationOptions& options,
			const std::unordered_set<std::string_view>& pureFunctions)
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
						bool dead = (isPure(*instr) || isPureCall(*instr, pureFunctions)) && result.isValid() && useCount[result.id] == 0;
						if (dead)
						{
							// A dropped call takes its queued arguments with it: the Param
							// instructions immediately before it feed nothing else, and keeping
							// them would keep the argument computations alive too. They are by
							// construction the last `argCount` instructions emitted, so they are
							// the tail of `live`.
							if (instr->opcode() == IrOpcode::Call)
							{
								u32 queued = instr->as<IrCallPayload>().argCount;
								while (queued > 0 && !live.empty() && live.back()->opcode() == IrOpcode::Param)
								{
									live.pop_back();
									--queued;
								}
							}
							changed = true;
						}
						else
						{
							live.push_back(instr);
						}
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

		// How many instructions a function's body holds, across all of its blocks - the size the
		// inliner weighs against the limit below.
		usize instructionCount(const IrFunction& function)
		{
			usize total = 0;
			for (const auto& block : function.blocks())
				total += block->instrs().size();
			return total;
		}

		// The instruction budget a callee gets before copying it stops being worth it. `inline` is a
		// hint about what is worth copying, not a licence to copy a four-hundred-instruction body into
		// every call; `always_inline` is the one that genuinely asks for no limit.
		usize inlineLimit(const IrFunction& function)
		{
			if (function.isAlwaysInline())
				return ~usize(0);
			if (function.isInlineHint())
				return kMaxInlineInstrsWhenRequested;
			return kMaxInlineInstrs;
		}

		// A callee worth splicing: any shape of control flow (several blocks, calls to other
		// functions), short enough to be worth copying, and not the entry point - `main` is never
		// called from anywhere in the first place, and codegen gives it a different epilogue
		// (codegen.h's own note). Any function that is called can be inlined; only `main`, a handler
		// and a variadic function are ruled out by what they are rather than by their shape.
		bool isInlinable(const IrFunction& function)
		{
			if (function.name() == "main")
				return false;
			// `__attribute__((noinline))` is an instruction, not a hint: the function is never a
			// candidate however small it is. `always_inline` is the other side of it, and is
			// handled by raising the size limit below.
			if (function.isNoInline())
				return false;
			// Nothing calls an interrupt handler, so there is no call site to splice it into - and its
			// prologue and epilogue are the point of it (save every register, `iret`), which a spliced
			// body would leave behind.
			if (function.isInterruptHandler())
				return false;
			// A variadic callee is handed arguments its parameter list does not describe, so there is
			// nothing for the splice to bind them to - and its body reads them out of the caller's
			// frame (IrOpcode::VaStart), which stops meaning anything once the body is spliced into a
			// different frame entirely.
			if (function.isVariadic())
				return false;
			if (function.blocks().empty())
				return false;
			// The last block has to end in a terminator: an unterminated one falls through to whatever
			// block is emitted next, and a splice moves the body somewhere else entirely.
			std::span<IrInstr* const> lastInstrs = function.blocks().back()->instrs();
			if (lastInstrs.empty() || !isTerminatorInstr(*lastInstrs.back()))
				return false;

			usize total = instructionCount(function);
			return total != 0 && total <= inlineLimit(function);
		}

		// Rebuilds one callee instruction inside the caller: every temporary it names becomes a
		// fresh caller temporary, every local slot it names becomes the caller slot reserved for it,
		// and every block a branch names becomes the caller block its copy lives in. A Return is not
		// handled here: the splice turns each one into a copy of the result plus a jump to the
		// continuation, so it never reaches this switch.
		IrInstr* remapInstr(const IrInstr& instr, support::Arena& arena, IrFunction& caller,
			std::unordered_map<u32, IrValue>& tempMap, const std::vector<u32>& localMap,
			const std::unordered_map<const BasicBlock*, BasicBlock*>& blockMap)
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
			auto mapBlock = [&](const BasicBlock* block) -> BasicBlock*
			{
				auto it = blockMap.find(block);
				return it == blockMap.end() ? nullptr : it->second;
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
				case IrOpcode::Param:
				{
					IrParamPayload p = instr.as<IrParamPayload>();
					p.value = mapValue(p.value);
					return arena.create<IrInstr>(loc, p);
				}
				// A call inside the callee becomes a call inside the caller. Its own Params sit
				// immediately before it and are remapped in the same pass, so the argCount still
				// describes them.
				case IrOpcode::Call:
				{
					IrCallPayload p = instr.as<IrCallPayload>();
					p.calleeValue = mapValue(p.calleeValue);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::Jump:
				{
					IrJumpPayload p = instr.as<IrJumpPayload>();
					if (BasicBlock* target = mapBlock(p.target))
					{
						p.target = target;
						return arena.create<IrInstr>(loc, p);
					}
					return nullptr;
				}
				case IrOpcode::CondJump:
				{
					IrCondJumpPayload p = instr.as<IrCondJumpPayload>();
					BasicBlock* trueTarget = mapBlock(p.trueTarget);
					BasicBlock* falseTarget = mapBlock(p.falseTarget);
					if (!trueTarget || !falseTarget)
						return nullptr;
					p.lhs = mapValue(p.lhs);
					p.rhs = mapValue(p.rhs);
					p.trueTarget = trueTarget;
					p.falseTarget = falseTarget;
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::TableJump:
				{
					const IrTableJumpPayload& p = instr.as<IrTableJumpPayload>();
					BasicBlock** targets = static_cast<BasicBlock**>(arena.allocate(sizeof(BasicBlock*) * p.entryCount, alignof(BasicBlock*)));
					for (u32 i = 0; i < p.entryCount; ++i)
					{
						BasicBlock* mapped = mapBlock(p.targets[i]);
						if (!mapped)
							return nullptr;
						targets[i] = mapped;
					}
					BasicBlock* defaultTarget = mapBlock(p.defaultTarget);
					if (!defaultTarget)
						return nullptr;
					IrTableJumpPayload copy = p;
					copy.targets = targets;
					copy.defaultTarget = defaultTarget;
					copy.discriminant = mapValue(p.discriminant);
					return arena.create<IrInstr>(loc, copy);
				}
				case IrOpcode::Builtin:
				{
					IrBuiltinPayload p = instr.as<IrBuiltinPayload>();
					p.a = mapValue(p.a);
					p.b = mapValue(p.b);
					p.result = mapValue(p.result);
					return arena.create<IrInstr>(loc, p);
				}
				case IrOpcode::MachineOp:
					return arena.create<IrInstr>(loc, instr.as<IrMachineOpPayload>());
				default:
					return nullptr; // Return is handled by the splice; VaStart never reaches here
			}
		}

		// The caller temporary a callee value maps to, creating one on first sight. Used both for a
		// value an instruction defines and for a Return's value, which may be one the body only
		// passes through (a parameter) rather than produces.
		IrValue mapCalleeValue(IrFunction& caller, std::unordered_map<u32, IrValue>& tempMap, IrValue value)
		{
			if (!value.isValid())
				return value;
			auto it = tempMap.find(value.id);
			if (it != tempMap.end())
				return it->second;
			IrValue fresh = caller.newTemp();
			tempMap.emplace(value.id, fresh);
			return fresh;
		}

		// Appends the FrameAddr+Store pair that settles argument `i` into the caller slot mapped to the
		// callee's parameter slot - exactly what the callee's own prologue would have done with the
		// incoming register.
		void appendArgumentStore(std::vector<IrInstr*>& out, support::Arena& arena, IrFunction& caller,
			const IrFunction& callee, const std::vector<u32>& localMap, const std::vector<IrValue>& argValues,
			u32 i, support::SourceLocation loc)
		{
			const IrLocalSlot& slot = callee.localSlots()[i];
			IrFrameAddrPayload addr;
			addr.result = caller.newTemp();
			addr.localIndex = localMap[i];
			out.push_back(arena.create<IrInstr>(loc, addr));

			IrStorePayload store;
			store.size = irMemSizeForBytes(slot.sizeInBytes);
			store.isFloat = slot.isFloat;
			store.address = addr.result;
			store.value = argValues[i];
			out.push_back(arena.create<IrInstr>(loc, store));
		}

		// Splices `callee`'s body over the Call at `instrs[callIndex]`, whose preceding `argCount`
		// entries are that call's Param instructions. The caller's block keeps everything before those
		// Params, stores each argument into the callee slot standing in for its parameter, and jumps
		// into a fresh copy of the callee's entry block; everything after the Call moves to a fresh
		// continuation block that every callee Return jumps to. Returns false (touching nothing) when
		// anything about the shape is not what inlining assumes.
		bool spliceInlinedCall(IrFunction& caller, BasicBlock& block, std::span<IrInstr* const> instrs,
			usize callIndex, const IrCallPayload& call, support::SourceLocation callLoc,
			const IrFunction& callee, support::Arena& arena)
		{
			if (call.argCount != callee.paramCount() || call.argCount > callIndex)
				return false;

			// Snapshot the block: replacing its instructions below frees the storage `instrs` spans.
			std::vector<IrInstr*> original(instrs.begin(), instrs.end());

			std::vector<IrValue> argValues(call.argCount);
			for (u32 i = 0; i < call.argCount; ++i)
			{
				const IrInstr* param = original[callIndex - call.argCount + i];
				if (param->opcode() != IrOpcode::Param)
					return false;
				argValues[i] = param->as<IrParamPayload>().value;
			}

			// Validate the whole callee body BEFORE allocating anything in the caller: a bad slot, a
			// VaStart, an inline-assembly block (its labels would be defined twice once the body is
			// copied) or a Return of a value the body never defines must decline cleanly rather than
			// leave a half-spliced block and a trail of orphaned blocks behind.
			std::vector<bool> defined(callee.tempCount(), false);
			for (const auto& calleeBlock : callee.blocks())
			{
				for (const IrInstr* instr : calleeBlock->instrs())
				{
					if (instr->opcode() == IrOpcode::FrameAddr && instr->as<IrFrameAddrPayload>().localIndex >= callee.localCount())
						return false;
					if (instr->opcode() == IrOpcode::VaStart)
						return false;
					if (instr->opcode() == IrOpcode::Call && !instr->as<IrCallPayload>().inlineAsm.empty())
						return false;
					IrValue result = resultOf(*instr);
					if (result.isValid() && result.id < defined.size())
						defined[result.id] = true;
				}
			}
			for (const auto& calleeBlock : callee.blocks())
				for (const IrInstr* instr : calleeBlock->instrs())
				{
					if (instr->opcode() != IrOpcode::Return)
						continue;
					const IrReturnPayload& ret = instr->as<IrReturnPayload>();
					if (!ret.hasValue)
					{
						// A body that falls off the end gets a bare `ret` even in an `int` function
						// (IrBuilder closes the last block that way). A call site that reads the result
						// cannot be served by a path that produces none, so decline it.
						if (call.hasResult)
							return false;
						continue;
					}
					if (!ret.value.isValid() || ret.value.id >= defined.size() || !defined[ret.value.id])
						return false;
				}

			// The single-block fast path only handles a body that ends in a Return; a single block
			// ending in any other terminator (say `lbl: goto lbl;`) falls through to the general path.
			const bool singleBlockReturns = callee.blocks().size() == 1 &&
				!callee.blocks().front()->instrs().empty() &&
				callee.blocks().front()->instrs().back()->opcode() == IrOpcode::Return;

			// Everything past this point allocates in the caller and must not fail. Every block is
			// terminated (IrBuilder guarantees it, and `isInlinable` checks the last one), so a
			// caller's tail always ends in its own terminator - no fall-through has to be repaired.
			//
			// Every callee slot - parameters first, then its own locals - gets a fresh slot in the
			// caller's frame, keeping its volatile/register/restrict properties so the accesses through
			// it stay what they were.
			std::vector<u32> localMap(callee.localCount());
			for (u32 i = 0; i < callee.localCount(); ++i)
			{
				const IrLocalSlot& slot = callee.localSlots()[i];
				localMap[i] = caller.newLocalSlot(slot.sizeInBytes, slot.isFloat, slot.isVolatile, slot.preferRegister, slot.isRestrict);
			}

			// Map every value the callee defines to a fresh caller temporary up front, so a use that
			// is textually before its definition (a loop-carried value) still names the right temp.
			std::unordered_map<u32, IrValue> tempMap;
			for (const auto& calleeBlock : callee.blocks())
				for (const IrInstr* instr : calleeBlock->instrs())
				{
					IrValue result = resultOf(*instr);
					if (result.isValid() && tempMap.find(result.id) == tempMap.end())
						tempMap.emplace(result.id, caller.newTemp());
				}

			// Fast path: a single-block callee that ends in a Return splices straight into the
			// caller's block, keeping the body adjacent to the instructions that follow the call. That
			// adjacency is what lets constant folding and load forwarding fold the result away (the
			// `add(3,4)` -> 7 case), which the general path's extra continuation block would break.
			std::span<const std::unique_ptr<BasicBlock>> calleeBlocks = callee.blocks();
			if (singleBlockReturns)
			{
				const BasicBlock& only = *calleeBlocks.front();
				std::vector<IrInstr*> rewritten;
				rewritten.reserve(original.size() + 2 * callee.paramCount());
				for (usize i = 0; i < callIndex - call.argCount; ++i)
					rewritten.push_back(original[i]);
				for (u32 i = 0; i < call.argCount; ++i)
					appendArgumentStore(rewritten, arena, caller, callee, localMap, argValues, i, callLoc);
				const std::unordered_map<const BasicBlock*, BasicBlock*> noBlocks;
				for (const IrInstr* instr : only.instrs())
				{
					if (instr->opcode() == IrOpcode::Return)
					{
						if (call.hasResult)
							rewritten.push_back(makeCopy(arena, instr->location(), call.result, mapCalleeValue(caller, tempMap, instr->as<IrReturnPayload>().value), call.isFloat));
						break;
					}
					IrInstr* copied = remapInstr(*instr, arena, caller, tempMap, localMap, noBlocks);
					if (!copied)
						return false;
					rewritten.push_back(copied);
				}
				for (usize i = callIndex + 1; i < original.size(); ++i)
					rewritten.push_back(original[i]);
				block.replaceInstrs(std::move(rewritten));
				return true;
			}

			std::unordered_map<const BasicBlock*, BasicBlock*> blockMap;
			for (const auto& calleeBlock : calleeBlocks)
				blockMap.emplace(calleeBlock.get(), &caller.createBlock());
			BasicBlock* continuation = &caller.createBlock();
			BasicBlock* entryCopy = blockMap.at(calleeBlocks.front().get());

			// The caller's block: prefix, then the argument stores, then a jump into the copy. Each
			// argument is stored into the slot standing in for its parameter, which is exactly what
			// the callee's own prologue would have done with the incoming register.
			std::vector<IrInstr*> head;
			head.reserve(callIndex - call.argCount + 2 * callee.paramCount() + 1);
			for (usize i = 0; i < callIndex - call.argCount; ++i)
				head.push_back(original[i]);
			for (u32 i = 0; i < call.argCount; ++i)
				appendArgumentStore(head, arena, caller, callee, localMap, argValues, i, callLoc);
			head.push_back(arena.create<IrInstr>(callLoc, IrJumpPayload{ entryCopy }));
			block.replaceInstrs(std::move(head));

			// The continuation: the caller's own instructions after the call. Every block ends in a
			// terminator (IrBuilder's own invariant), so this always ends in one too.
			std::vector<IrInstr*> tail;
			for (usize i = callIndex + 1; i < original.size(); ++i)
				tail.push_back(original[i]);
			continuation->replaceInstrs(std::move(tail));

			// Every callee block, copied in place. A Return becomes (optional) a copy of the result
			// into the call's own result, then a jump to the continuation.
			for (usize cbIndex = 0; cbIndex < calleeBlocks.size(); ++cbIndex)
			{
				const BasicBlock& calleeBlock = *calleeBlocks[cbIndex];
				std::vector<IrInstr*> body;
				body.reserve(calleeBlock.instrs().size() + 1);
				bool terminated = false;
				for (const IrInstr* instr : calleeBlock.instrs())
				{
					if (instr->opcode() == IrOpcode::Return)
					{
						if (call.hasResult)
							body.push_back(makeCopy(arena, instr->location(), call.result, mapCalleeValue(caller, tempMap, instr->as<IrReturnPayload>().value), call.isFloat));
						body.push_back(arena.create<IrInstr>(instr->location(), IrJumpPayload{ continuation }));
						terminated = true;
						break;
					}
					IrInstr* copied = remapInstr(*instr, arena, caller, tempMap, localMap, blockMap);
					if (!copied)
						return false;
					body.push_back(copied);
					if (isTerminatorInstr(*instr))
						terminated = true;
				}
				if (!terminated && cbIndex + 1 < calleeBlocks.size())
					body.push_back(arena.create<IrInstr>(callLoc, IrJumpPayload{ blockMap.at(calleeBlocks[cbIndex + 1].get()) }));
				blockMap.at(&calleeBlock)->replaceInstrs(std::move(body));
			}
			return true;
		}

		bool inlineCalls(IrModule& module, support::Arena& arena, const OptimizationOptions& options, u32& inlinedOut)
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
			for (const auto& functionPtr : module.functions())
			{
				IrFunction& function = *functionPtr;

				// Snapshot the blocks to scan: inlining appends the callee's block copies to the
				// caller, and those copies are deliberately NOT rescanned in this pass - which is what
				// makes inlining a recursive or mutually recursive callee terminate instead of
				// expanding forever.
				std::vector<BasicBlock*> blocks;
				blocks.reserve(function.blocks().size());
				for (const auto& block : function.blocks())
					blocks.push_back(block.get());

				for (BasicBlock* block : blocks)
				{
					// This block's call sites, collected first and processed from last to first: a
					// splice keeps everything before the call (the prefix, including every earlier
					// call and its Params) at the same index, so an earlier site stays valid.
					std::vector<usize> callIndices;
					{
						std::span<IrInstr* const> instrs = block->instrs();
						for (usize i = 0; i < instrs.size(); ++i)
							if (instrs[i]->opcode() == IrOpcode::Call)
								callIndices.push_back(i);
					}

					for (auto it = callIndices.rbegin(); it != callIndices.rend(); ++it)
					{
						std::span<IrInstr* const> instrs = block->instrs();
						usize index = *it;
						if (index >= instrs.size() || instrs[index]->opcode() != IrOpcode::Call)
							continue;
						const auto& call = instrs[index]->as<IrCallPayload>();
						if (call.isIndirect())
							continue; // the target is an address computed at run time - nothing to look up
						auto candidate = inlinable.find(call.callee);
						// Never inline a function into itself.
						if (candidate == inlinable.end() || candidate->second == &function)
							continue;
						// The callee may have grown by an earlier inline in this same pass; splice it
						// only if it still fits the budget, so a chain of inlines cannot blow up.
						if (instructionCount(*candidate->second) > inlineLimit(*candidate->second))
							continue;
						if (spliceInlinedCall(function, *block, instrs, index, call, instrs[index]->location(), *candidate->second, arena))
						{
							changed = true;
							++inlinedOut;
						}
					}
				}
			}
			return changed;
		}
	}

	void optimize(IrModule& module, support::Arena& arena, const OptimizationOptions& options, OptimizationStats* stats)
	{
		auto countInstructions = [](const IrModule& target)
		{
			u32 total = 0;
			for (const auto& function : target.functions())
				for (const auto& block : function->blocks())
					total += static_cast<u32>(block->instrs().size());
			return total;
		};

		// Counted before ANY pass, including inlining - "instructionsBefore" is what the module
		// held when optimize() was entered.
		if (stats)
			stats->instructionsBefore = countInstructions(module);

		u32 inlined = 0;
		inlineCalls(module, arena, options, inlined);
		if (stats)
			stats->inlinedCalls = inlined;

		// The functions declared `pure` or `const`. Dead-code elimination needs the set by name:
		// it runs per function and cannot look a callee up in the module.
		std::unordered_set<std::string_view> pureFunctions(module.pureFunctions().begin(), module.pureFunctions().end());
		for (const auto& function : module.functions())
			if (function->isPure())
				pureFunctions.insert(function->name());

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
				changed |= propagateConditionalConstants(*function, arena, options);
				changed |= reduceStrength(*function, arena, options);
				changed |= forwardLoads(*function, arena, options);
				changed |= forwardRestrictLoads(*function, arena, options);
				changed |= propagateCopies(*function, arena, options);
				changed |= eliminateCommonSubexpressions(*function, arena, options);
				changed |= eliminateDeadStores(*function, options);
				changed |= threadJumps(*function, arena, options);
				changed |= removeUnreachableBlocks(*function, options);
				changed |= hoistLoopInvariants(*function, options);
				changed |= layoutBlocks(*function, options);
				changed |= eliminateDeadCode(*function, options, pureFunctions);
				if (!changed)
					break;
			}
		}

		// Last, so that a function left callerless BY inlining is dropped too.
		removeUnusedFunctions(module, options);

		if (stats)
		{
			stats->functions = static_cast<u32>(module.functions().size());
			stats->instructionsAfter = countInstructions(module);
			u32 jumpTables = 0;
			for (const auto& function : module.functions())
				for (const auto& block : function->blocks())
					for (const IrInstr* instr : block->instrs())
						if (instr->opcode() == IrOpcode::TableJump)
							++jumpTables;
			stats->jumpTables = jumpTables;
		}
	}
}
