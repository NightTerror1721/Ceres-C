#include <ceresc/codegen/value_placement.h>

#include <algorithm>
#include <unordered_map>

namespace ceresc::codegen
{
	namespace
	{
		using namespace ir;

		// r4/r5 are codegen's scratch pair. The allocatable pools below keep them reserved by
		// omitting them from every list.
		constexpr u32 kInvalidLocal = ~0u;

		// r6/r7/r12 are caller-saved and free in any function; r0-r3 additionally in one that calls
		// nothing, since nothing will clobber them (they are only the ARGUMENT registers on the way
		// in, and a function that makes no call never has to set them up for one). r8-r11 stay out -
		// see value_placement.h's header comment.
		std::vector<u32> allocatableIntRegisters(bool hasCalls)
		{
			if (hasCalls)
				return { 6, 7, 12 };
			return { 6, 7, 12, 0, 1, 2, 3 };
		}

		std::vector<u32> allocatableFloatRegisters(bool hasCalls)
		{
			if (hasCalls)
				return { 6, 7 };
			return { 6, 7, 0, 1, 2, 3 };
		}

		// The callee-saved half, which a call does NOT clobber. A local that has to survive a call
		// can live here, at the price of the function saving and restoring each one around its own
		// body - see value_placement.h's header comment on why these stay out of the caller-saved
		// pools above.
		std::vector<u32> allocatableCalleeSavedIntRegisters()
		{
			return { 8, 9, 10, 11 };
		}

		std::vector<u32> allocatableCalleeSavedFloatRegisters()
		{
			return { 8, 9, 10, 11, 12, 13, 14, 15 };
		}

		// Per-block live-in/live-out over temporaries, iterated to a fixpoint - see
		// value_placement.h's own note on why a linear interval is not good enough.
		struct Liveness
		{
			std::vector<std::vector<bool>> liveIn;  // [block][tempId]
			std::vector<std::vector<bool>> liveOut;
		};

		Liveness computeLiveness(const IrFunction& function)
		{
			usize blockCount = function.blocks().size();
			usize tempCount = function.tempCount();

			std::vector<std::vector<bool>> use(blockCount, std::vector<bool>(tempCount, false));
			std::vector<std::vector<bool>> def(blockCount, std::vector<bool>(tempCount, false));

			for (usize b = 0; b < blockCount; ++b)
			{
				for (IrInstr* instr : function.blocks()[b]->instrs())
				{
					// Reads first: a temporary read before this block writes it is an upward-exposed
					// use, which is what makes it live on entry to the block.
					forEachOperand(*instr, [&](IrValue value)
					{
						if (value.isValid() && value.id < tempCount && !def[b][value.id])
							use[b][value.id] = true;
					});
					IrValue result = resultOf(*instr);
					if (result.isValid() && result.id < tempCount)
						def[b][result.id] = true;
				}
			}

			Liveness liveness;
			liveness.liveIn.assign(blockCount, std::vector<bool>(tempCount, false));
			liveness.liveOut.assign(blockCount, std::vector<bool>(tempCount, false));

			std::vector<std::vector<BasicBlock*>> successors(blockCount);
			std::unordered_map<const BasicBlock*, usize> indexOf;
			for (usize b = 0; b < blockCount; ++b)
				indexOf[function.blocks()[b].get()] = b;
			for (usize b = 0; b < blockCount; ++b)
				successors[b] = successorsOf(function, b);

			bool changed = true;
			while (changed)
			{
				changed = false;
				for (usize raw = 0; raw < blockCount; ++raw)
				{
					usize b = blockCount - 1 - raw; // backward order converges faster for a backward analysis
					std::vector<bool> out(tempCount, false);
					for (BasicBlock* successor : successors[b])
					{
						auto it = indexOf.find(successor);
						if (it == indexOf.end())
							continue;
						const std::vector<bool>& successorIn = liveness.liveIn[it->second];
						for (usize t = 0; t < tempCount; ++t)
							out[t] = out[t] || successorIn[t];
					}

					std::vector<bool> in(tempCount, false);
					for (usize t = 0; t < tempCount; ++t)
						in[t] = use[b][t] || (out[t] && !def[b][t]);

					if (out != liveness.liveOut[b] || in != liveness.liveIn[b])
					{
						liveness.liveOut[b] = std::move(out);
						liveness.liveIn[b] = std::move(in);
						changed = true;
					}
				}
			}
			return liveness;
		}
	}

	ValuePlacement::ValuePlacement(const IrFunction& function, const support::OptimizationOptions& options)
	{
		usize tempCount = function.tempCount();
		usize localCount = function.localCount();
		std::span<const IrLocalSlot> localSlots = function.localSlots();

		_temps.assign(tempCount, Placement{});
		_locals.assign(localCount, Placement{});
		_virtualAddress.assign(tempCount, kInvalidLocal);

		// Which values any surviving instruction still mentions. Temporary ids and local slots are
		// handed out as the IR is BUILT and never renumbered, so an optimized function is full of
		// ids nothing refers to any more (ir_optimizer.h drops whole instructions, and inlining
		// reserves a slot per callee local that forwarding may then make unnecessary). Giving those
		// a frame field each would reserve stack for values that no longer exist - which is exactly
		// what made an -O2 function that needs no frame at all still open one.
		std::vector<bool> tempReferenced(tempCount, false);
		std::vector<bool> localReferenced(localCount, false);
		for (const auto& block : function.blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				if (IrValue result = resultOf(*instr); result.isValid() && result.id < tempCount)
					tempReferenced[result.id] = true;
				forEachOperand(*instr, [&](IrValue value)
				{
					if (value.isValid() && value.id < tempCount)
						tempReferenced[value.id] = true;
				});
				if (instr->opcode() == IrOpcode::FrameAddr)
				{
					u32 local = instr->as<IrFrameAddrPayload>().localIndex;
					if (local < localCount)
						localReferenced[local] = true;
				}
			}
		}
		// A parameter nothing reads is deliberately NOT forced to be referenced: it already arrived
		// wherever the caller put it, and if the body never looks at it there is nothing for the
		// prologue to do with it either (generateFunction() skips it).

		FrameLayout layout(function);
		_outgoingSlotCount = layout.outgoingSlotCount();

		std::vector<bool> paramIsFloat(function.paramCount());
		for (u32 i = 0; i < function.paramCount(); ++i)
			paramIsFloat[i] = localSlots[i].isFloat;
		_paramArrival = assignArgSlots(paramIsFloat);

		bool anyParamOnStack = std::any_of(_paramArrival.begin(), _paramArrival.end(),
			[](const ArgSlot& slot) { return slot.kind == ArgSlotKind::Stack; });

		bool hasCalls = false;
		for (const auto& block : function.blocks())
			for (const IrInstr* instr : block->instrs())
				if (instr->opcode() == IrOpcode::Call)
					hasCalls = true;

		// ---- escape analysis: which locals can live in a register at all -------------------------
		//
		// A local is disqualified the moment anything but a Load/Store address consumes a FrameAddr
		// naming it, or the access width disagrees with the slot's own (which only happens once an
		// address has been reinterpreted through a pointer - and that address must have escaped to
		// get there).
		std::vector<bool> localEscapes(localCount, false);
		std::vector<u32> frameAddrLocal(tempCount, kInvalidLocal);
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
			if (value.isValid() && value.id < tempCount && frameAddrLocal[value.id] != kInvalidLocal)
				localEscapes[frameAddrLocal[value.id]] = true;
		};

		for (const auto& block : function.blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				switch (instr->opcode())
				{
					case IrOpcode::Load:
					{
						const auto& p = instr->as<IrLoadPayload>();
						u32 local = (p.address.isValid() && p.address.id < tempCount) ? frameAddrLocal[p.address.id] : kInvalidLocal;
						if (local != kInvalidLocal &&
							(p.size != irMemSizeForBytes(localSlots[local].sizeInBytes) || p.isFloat != localSlots[local].isFloat))
							localEscapes[local] = true; // a reinterpreting access - keep it in memory
						break;
					}
					case IrOpcode::Store:
					{
						const auto& p = instr->as<IrStorePayload>();
						u32 local = (p.address.isValid() && p.address.id < tempCount) ? frameAddrLocal[p.address.id] : kInvalidLocal;
						if (local != kInvalidLocal &&
							(p.size != irMemSizeForBytes(localSlots[local].sizeInBytes) || p.isFloat != localSlots[local].isFloat))
							localEscapes[local] = true;
						markEscape(p.value); // an address STORED somewhere is an address that got away
						break;
					}
					default:
						// Every other reader of a FrameAddr temporary treats it as an ordinary value.
						forEachOperand(*instr, markEscape);
						break;
				}
			}
		}

		// A FrameAddr defined more than once cannot be reasoned about slot-by-slot (IrBuilder does
		// reuse a temporary id across two definitions - ir_optimizer.cpp's own note), so anything it
		// might name stays in memory.
		for (usize t = 0; t < tempCount; ++t)
			if (frameAddrLocal[t] != kInvalidLocal && defCount[static_cast<u32>(t)] > 1)
				localEscapes[frameAddrLocal[t]] = true;

		// A temporary whose value is handed straight to a Call's Param (a call argument) cannot live
		// in an argument register (r0-r3/f0-f3): the argument-setup moves that precede the call
		// write those registers in order, and a source sitting in one would be clobbered before its
		// own move read it. Fase 3 below lets call-free temporaries use the argument registers, but
		// only the ones that are NOT arguments themselves.
		std::vector<bool> callArg(tempCount, false);
		for (const auto& block : function.blocks())
			for (const IrInstr* instr : block->instrs())
				if (instr->opcode() == IrOpcode::Param)
				{
					IrValue value = instr->as<IrParamPayload>().value;
					if (value.isValid() && value.id < tempCount)
						callArg[value.id] = true;
				}

		// ---- register assignment ------------------------------------------------------------------

		std::vector<u32> freeInt = allocatableIntRegisters(hasCalls);
		std::vector<u32> freeFloat = allocatableFloatRegisters(hasCalls);
		auto takeRegister = [](std::vector<u32>& pool, u32 wanted) -> std::optional<u32>
		{
			if (pool.empty())
				return std::nullopt;
			auto it = std::find(pool.begin(), pool.end(), wanted);
			if (it == pool.end())
				it = pool.begin();
			u32 reg = *it;
			pool.erase(it);
			return reg;
		};

		if (options.registerAllocation)
		{
			// Locals first: they are live for the whole function, so whatever they take is gone for
			// its whole length. In a call-free function that is the caller-saved pool; in one that
			// calls, it is the callee-saved pool below.
			//
			// Two passes over the same list, not one: everything the program asked for with
			// `register` is considered before anything that did not. The pool runs out when there
			// are more eligible locals than registers, and until now that was settled purely by
			// declaration order - so a `register` local declared late lost to ordinary variables
			// declared early, and the keyword changed nothing at all in the generated code.
			//
			// Only the ORDER changes. Every condition below is a correctness rule (a call clobbers
			// the pool, an escaped local needs an address, a volatile one needs a memory home, a
			// wider-than-a-word one does not fit) and `register` does not relax one: C says the
			// keyword is a request the implementation may decline, never a promise it must keep at
			// the cost of a wrong answer.
			//
			// "Does not fit" means wider than a register, not narrower than one. A `char`, a
			// `short` and a `bool` all fit with room to spare, and the IR's own narrowing
			// invariant (ir_instr.h) is what makes holding one in a register mean the same thing
			// as holding it in a byte or half-word field: a value of narrow type is ALWAYS
			// already in its narrowed representation, so the strb/ldrsb pair a frame field would
			// have gone through has nothing left to do. The one place that is not automatic is a
			// parameter, which arrives from outside this function - codegen's prologue narrows
			// one on the way into its register, exactly as the store into a field used to.
			// The callee-saved pools, reached only by a call-making function. An interrupt handler
			// takes them too - its own save/restore is the interrupt prologue/epilogue's job, whose
			// mask already covers the whole allocatable set (codegen.h), so no extra pushm/popm is
			// emitted around the body for it.
			std::vector<u32> calleeFreeInt = allocatableCalleeSavedIntRegisters();
			std::vector<u32> calleeFreeFloat = allocatableCalleeSavedFloatRegisters();

			auto assignLocals = [&](bool requested)
			{
				for (u32 i = 0; i < localCount; ++i)
				{
					const IrLocalSlot& slot = localSlots[i];
					if (slot.preferRegister != requested)
						continue;

					bool paramOnStack = i < function.paramCount() && _paramArrival[i].kind == ArgSlotKind::Stack;
					if (!localReferenced[i] || localEscapes[i] || slot.isVolatile || slot.sizeInBytes > 4 || paramOnStack)
						continue;

					// A leaf's local goes in the caller-saved pool; a call-making function's goes in
					// the callee-saved pool instead - the only registers a call cannot clobber, and
					// the price is the pushm/popm pair codegen emits around the body.
					std::vector<u32>& pool = hasCalls ? (slot.isFloat ? calleeFreeFloat : calleeFreeInt)
						: (slot.isFloat ? freeFloat : freeInt);
					// A parameter prefers the register it already arrived in: taking it means the
					// prologue has nothing at all to emit for that parameter. Only in a leaf, though
					// - that register is caller-saved, and a call would clobber it.
					u32 preferred = (!hasCalls && i < function.paramCount()) ? _paramArrival[i].index : ~0u;

					if (std::optional<u32> reg = takeRegister(pool, preferred))
					{
						_locals[i] = Placement{ PlacementKind::Register, *reg, slot.isFloat };
						if (hasCalls)
						{
							if (slot.isFloat)
								_calleeSavedFloatMask |= (1u << *reg);
							else
								_calleeSavedIntMask |= (1u << *reg);
						}
					}
				}
			};
			assignLocals(true);
			assignLocals(false);
		}

		// Every FrameAddr naming a register-resident local becomes virtual: there is no address to
		// compute, and codegen turns the Load/Store through it into a register move.
		for (usize t = 0; t < tempCount; ++t)
		{
			u32 local = frameAddrLocal[t];
			if (local != kInvalidLocal && _locals[local].kind == PlacementKind::Register)
			{
				_temps[t] = Placement{ PlacementKind::Virtual, local, false };
				_virtualAddress[t] = local;
			}
		}

		// One liveness solution serves both the register pass below and the slot-sharing pass after
		// it - the analysis is the expensive part, and nothing between them changes the CFG.
		Liveness liveness;
		if (options.registerAllocation && tempCount > 0)
			liveness = computeLiveness(function);

		if (options.registerAllocation && tempCount > 0)
		{
			for (usize b = 0; b < function.blocks().size(); ++b)
			{
				std::span<IrInstr* const> instrs = function.blocks()[b]->instrs();

				// Within one block the code is straight-line, so a temporary defined here and never
				// live on the way out has an exact last-read index - and a register can be handed
				// back the moment it passes.
				std::vector<usize> lastUse(tempCount, 0);
				std::vector<bool> usedHere(tempCount, false);
				for (usize i = 0; i < instrs.size(); ++i)
				{
					forEachOperand(*instrs[i], [&](IrValue value)
					{
						if (value.isValid() && value.id < tempCount)
						{
							lastUse[value.id] = i;
							usedHere[value.id] = true;
						}
					});
				}

				std::vector<u32> blockFreeInt = freeInt;
				std::vector<u32> blockFreeFloat = freeFloat;
				// The argument registers, only ever handed out in a call-making function and only to
				// a temporary that is NOT itself a call argument (see callArg[] above). A leaf already
				// has them in freeInt/freeFloat.
				std::vector<u32> blockFreeIntArg = hasCalls ? std::vector<u32>{ 0, 1, 2, 3 } : std::vector<u32>{};
				std::vector<u32> blockFreeFloatArg = hasCalls ? std::vector<u32>{ 0, 1, 2, 3 } : std::vector<u32>{};
				struct Held { u32 reg; bool isFloat; bool fromArgPool; usize until; };
				std::vector<Held> held;

				for (usize i = 0; i < instrs.size(); ++i)
				{
					// Return registers freed by everything whose last read was before this point.
					for (usize h = held.size(); h-- > 0;)
					{
						if (held[h].until >= i)
							continue;
						if (held[h].fromArgPool)
							(held[h].isFloat ? blockFreeFloatArg : blockFreeIntArg).push_back(held[h].reg);
						else
							(held[h].isFloat ? blockFreeFloat : blockFreeInt).push_back(held[h].reg);
						held.erase(held.begin() + static_cast<std::ptrdiff_t>(h));
					}

					// A call clobbers every allocatable caller-saved register, so nothing may hold
					// one across it - enforced below by refusing to grant a register at all when the
					// live range that would need it contains a Call.
					IrValue result = resultOf(*instrs[i]);
					if (!result.isValid() || result.id >= tempCount)
						continue;
					if (_temps[result.id].kind == PlacementKind::Virtual)
						continue;
					if (defCount[result.id] != 1 || liveness.liveOut[b][result.id] || !usedHere[result.id])
						continue; // defined twice, or read outside this block - it needs a real home

					// No call may sit between the definition and the last read.
					bool spansCall = false;
					for (usize k = i + 1; k <= lastUse[result.id] && k < instrs.size(); ++k)
						if (instrs[k]->opcode() == IrOpcode::Call)
							spansCall = true;
					if (spansCall)
						continue;

					bool isFloat = false;
					switch (instrs[i]->opcode())
					{
						case IrOpcode::Const: isFloat = instrs[i]->as<IrConstPayload>().isFloat; break;
						case IrOpcode::BinOp: isFloat = instrs[i]->as<IrBinOpPayload>().isFloat; break;
						case IrOpcode::UnOp:
						{
							const auto& p = instrs[i]->as<IrUnOpPayload>();
							isFloat = p.op == IrUnOp::IntToFloat || (p.isFloat && p.op == IrUnOp::Neg);
							break;
						}
						case IrOpcode::Copy: isFloat = instrs[i]->as<IrCopyPayload>().isFloat; break;
						case IrOpcode::Load: isFloat = instrs[i]->as<IrLoadPayload>().isFloat; break;
						case IrOpcode::Call: isFloat = instrs[i]->as<IrCallPayload>().isFloat; break;
						default: isFloat = false; break; // Cmp yields 0/1, FrameAddr/GlobalAddr an address
					}

					std::vector<u32>& pool = isFloat ? blockFreeFloat : blockFreeInt;
					std::vector<u32>& argPool = isFloat ? blockFreeFloatArg : blockFreeIntArg;
					bool fromArgPool = false;
					u32 reg;
					if (!pool.empty())
					{
						reg = pool.back();
						pool.pop_back();
					}
					else if (!callArg[result.id] && !argPool.empty())
					{
						reg = argPool.back();
						argPool.pop_back();
						fromArgPool = true;
					}
					else
					{
						continue;
					}
					_temps[result.id] = Placement{ PlacementKind::Register, reg, isFloat };
					held.push_back(Held{ reg, isFloat, fromArgPool, lastUse[result.id] });
				}
			}
		}

		// ---- frame slots for everything left ------------------------------------------------------

		for (u32 i = 0; i < localCount; ++i)
		{
			if (_locals[i].kind == PlacementKind::Register)
				continue;
			if (!localReferenced[i])
			{
				_locals[i] = Placement{ PlacementKind::None, 0, false };
				continue;
			}
			_locals[i] = Placement{ PlacementKind::Slot, static_cast<u32>(_slots.size()), localSlots[i].isFloat };
			_slots.push_back(FrameSlotInfo{ localSlots[i].sizeInBytes, localSlots[i].isFloat });
		}

		std::vector<u32> spilled;
		for (usize t = 0; t < tempCount; ++t)
		{
			if (!tempReferenced[t])
			{
				_temps[t] = Placement{ PlacementKind::None, 0, false };
				continue;
			}
			if (_temps[t].kind == PlacementKind::Slot)
				spilled.push_back(static_cast<u32>(t));
		}

		if (!options.registerAllocation)
		{
			// The simplified rule: one permanent field per temporary, no sharing, no analysis.
			for (u32 t : spilled)
			{
				_temps[t] = Placement{ PlacementKind::Slot, static_cast<u32>(_slots.size()), false };
				_slots.push_back(FrameSlotInfo{ 4, false });
			}
		}
		else if (!spilled.empty())
		{
			// Interference between spilled temporaries, so one field can serve several of them.
			// Two interfere when both are live at the same program point - read off the same
			// liveness solution the register pass used, walked backwards through each block.
			std::vector<std::vector<bool>> interferes(tempCount, std::vector<bool>(tempCount, false));

			for (usize b = 0; b < function.blocks().size(); ++b)
			{
				std::span<IrInstr* const> instrs = function.blocks()[b]->instrs();
				std::vector<bool> live = liveness.liveOut[b];

				auto markPair = [&](u32 a, u32 c)
				{
					if (a != c)
					{
						interferes[a][c] = true;
						interferes[c][a] = true;
					}
				};
				auto markAllLive = [&](u32 value)
				{
					for (usize t = 0; t < tempCount; ++t)
						if (live[t])
							markPair(value, static_cast<u32>(t));
				};

				for (usize raw = instrs.size(); raw-- > 0;)
				{
					const IrInstr& instr = *instrs[raw];
					IrValue result = resultOf(instr);
					if (result.isValid() && result.id < tempCount)
					{
						// The result conflicts with everything live right after its definition, even
						// if it is itself dead: the slot it takes must not be one of theirs.
						markAllLive(result.id);
						live[result.id] = false;
					}
					forEachOperand(instr, [&](IrValue value)
					{
						if (value.isValid() && value.id < tempCount)
						{
							markAllLive(value.id);
							live[value.id] = true;
						}
					});
				}
			}

			u32 firstTempSlot = static_cast<u32>(_slots.size());
			std::vector<std::vector<u32>> occupants; // per shared slot, which temporaries already took it
			for (u32 t : spilled)
			{
				u32 chosen = ~0u;
				for (u32 candidate = 0; candidate < occupants.size(); ++candidate)
				{
					bool conflicts = std::any_of(occupants[candidate].begin(), occupants[candidate].end(),
						[&](u32 other) { return interferes[t][other]; });
					if (!conflicts)
					{
						chosen = candidate;
						break;
					}
				}
				if (chosen == ~0u)
				{
					chosen = static_cast<u32>(occupants.size());
					occupants.emplace_back();
					_slots.push_back(FrameSlotInfo{ 4, false });
				}
				occupants[chosen].push_back(t);
				_temps[t] = Placement{ PlacementKind::Slot, firstTempSlot + chosen, false };
			}
		}

		_needsFrame = !options.framelessLeaf
			|| !_slots.empty()
			|| _outgoingSlotCount > 0
			|| anyParamOnStack
			// A variadic function addresses its argument tail as [fp + N] (IrOpcode::VaStart), and
			// a frameless leaf has no fp of its own to measure that from - so the frameless rule
			// simply does not apply to one, however little else it would have put in a frame.
			|| function.isVariadic();
	}

	Placement ValuePlacement::temp(IrValue value) const
	{
		if (!value.isValid() || value.id >= _temps.size())
			return Placement{ PlacementKind::None, 0, false };
		return _temps[value.id];
	}

	Placement ValuePlacement::local(u32 localIndex) const
	{
		if (localIndex >= _locals.size())
			return Placement{ PlacementKind::None, 0, false };
		return _locals[localIndex];
	}

	std::optional<u32> ValuePlacement::virtualAddressLocal(IrValue value) const
	{
		if (!value.isValid() || value.id >= _virtualAddress.size())
			return std::nullopt;
		u32 local = _virtualAddress[value.id];
		if (local == ~0u)
			return std::nullopt;
		return local;
	}
}
