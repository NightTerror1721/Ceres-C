#pragma once

#include "ir_instr.h"
#include <ceresc/ast/type.h>
#include <ceresc/support/string_pool.h>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// BasicBlock and IrFunction - a function lowers to a list of basic blocks, each a linear sequence
// of IrInstr.
//
// Individual IrInstr are Arena-allocated (ir_instr.h, §4 - every AST node and every IR instruction
// goes through the same bump allocator, owned by whoever is building the IR - see IrBuilder), but
// BasicBlock/IrFunction/IrModule themselves are not: they need a genuinely mutable, growable
// structure while IrBuilder is still deciding how many blocks a function needs (a new block can be
// created mid-lowering for every if/while/for/switch/goto target), which the
// TriviallyDestructible/Arena::create<T> discipline the AST and IrInstr follow would not allow -
// exactly the same reasoning sema::Scope (libs/sema/symbol_table.h) gives for being an ordinary
// heap object instead of an arena one.
//
// See the architecture plan, §9.
//
// Implemented in Fase 5 of the phased plan (§13).

namespace ceresc::ast
{
	// Forward declaration only: IrStaticLocal keeps a VarDecl* so codegen can read its type and its
	// initializer, which is the same borrowing relationship IrFunction already has with ast::Type.
	// Including decl.h here would pull the whole AST into every consumer of the IR for one pointer.
	class VarDecl;
}

namespace ceresc::ir
{
	class BasicBlock
	{
	private:
		u32 _id;
		std::vector<IrInstr*> _instrs; // non-owning - each IrInstr lives in IrBuilder's Arena

	public:
		explicit BasicBlock(u32 id) noexcept : _id(id) {}
		BasicBlock(const BasicBlock&) = delete;
		BasicBlock(BasicBlock&&) = delete;
		~BasicBlock() = default;

		BasicBlock& operator=(const BasicBlock&) = delete;
		BasicBlock& operator=(BasicBlock&&) = delete;

	public:
		u32 id() const noexcept { return _id; }
		std::span<IrInstr* const> instrs() const noexcept { return _instrs; }

		// True once this block's last instruction is a Jump/CondJump/Return - a block only ever
		// falls into the next one through an explicit Jump, never implicitly (see the header
		// comment above), so IrBuilder consults this before appending a fallthrough jump of its own.
		bool isTerminated() const noexcept;

		void append(IrInstr* instr) { _instrs.push_back(instr); }

		// Swaps this block's whole instruction list for another one - how ir_optimizer.h's passes
		// rewrite a block (dropping a dead instruction, folding one into a Const, retargeting a
		// terminator, splicing an inlined callee in). Deliberately a wholesale replacement rather
		// than in-place mutation of an IrInstr: an IrInstr's payload is read-only by design
		// (ir_instr.h's `as<T>() const`), so a pass builds the replacement instruction in the same
		// Arena and swaps the list, instead of every consumer having to wonder whether the payload
		// it just read can change underneath it.
		void replaceInstrs(std::vector<IrInstr*> instrs) noexcept { _instrs = std::move(instrs); }
	};

	// A local frame slot's byte size and register bank - everything libs/codegen's frame_layout
	// needs to reserve and align the slot (Type::sizeInBytes(), libs/ast) without re-deriving it by
	// re-walking the FunctionDecl/VarDecl chain in a second, separate traversal that would have to
	// stay in lockstep with IrBuilder's own slot assignment order by construction alone. Recorded
	// once, at the same point IrBuilder already has the AST's resolved Type in hand (reserveParamSlots()/
	// newLocalSlot() below) - see the architecture plan, §10.
	struct IrLocalSlot
	{
		u32 sizeInBytes = 4;
		bool isFloat = false;
		bool isVolatile = false;
		bool preferRegister = false;
	};

	class IrFunction
	{
	private:
		std::string_view _name;
		const ast::Type* _returnType = nullptr;
		u32 _paramCount = 0;
		std::vector<IrLocalSlot> _localSlots; // includes params, at indices [0, paramCount) - see newLocalSlot()/reserveParamSlots()
		std::vector<std::unique_ptr<BasicBlock>> _blocks;
		u32 _nextTempId = 0;
		// Two facts about the DECLARATION that the optimizer cannot re-derive from the body:
		//
		//   externalLinkage - another object may call this, so unused-function elimination must keep
		//                     it even when nothing in this unit does. `static` is what takes it away.
		//   inlineHint      - the program asked for `inline`. It raises the size limit the inliner
		//                     applies, so the request means something rather than being decoration.
		bool _externalLinkage = true;
		bool _inlineHint = false;

	public:
		IrFunction(std::string_view name, const ast::Type* returnType) noexcept :
			_name(name), _returnType(returnType)
		{}
		IrFunction(const IrFunction&) = delete;
		IrFunction(IrFunction&&) = delete;
		~IrFunction() = default;

		IrFunction& operator=(const IrFunction&) = delete;
		IrFunction& operator=(IrFunction&&) = delete;

	public:
		std::string_view name() const noexcept { return _name; }
		const ast::Type* returnType() const noexcept { return _returnType; }
		u32 paramCount() const noexcept { return _paramCount; }
		u32 localCount() const noexcept { return static_cast<u32>(_localSlots.size()); }
		std::span<const IrLocalSlot> localSlots() const noexcept { return _localSlots; }
		std::span<const std::unique_ptr<BasicBlock>> blocks() const noexcept { return _blocks; }
		bool hasExternalLinkage() const noexcept { return _externalLinkage; }
		bool isInlineHint() const noexcept { return _inlineHint; }
		void setLinkage(bool external, bool inlineHint) noexcept { _externalLinkage = external; _inlineHint = inlineHint; }

		BasicBlock& createBlock()
		{
			_blocks.push_back(std::make_unique<BasicBlock>(static_cast<u32>(_blocks.size())));
			return *_blocks.back();
		}

		IrValue newTemp() noexcept { return IrValue{ _nextTempId++ }; }
		u32 tempCount() const noexcept { return _nextTempId; }

		// Called exactly once, right after construction, before any real local's slot is reserved -
		// reserves frame slots [0, params.size()) for the function's own parameters, in declaration
		// order, so IrBuilder can map a Param to a slot by its plain position instead of tracking one
		// separately.
		void reserveParamSlots(std::span<const IrLocalSlot> params)
		{
			_paramCount = static_cast<u32>(params.size());
			_localSlots.assign(params.begin(), params.end());
		}

		// Reserves the next local frame slot (see ir_instr.h's IrFrameAddrPayload::localIndex) and
		// returns its index. Called once per local VarDecl as IrBuilder encounters it in the body.
		// Slots are never reused across sibling blocks even when their lifetimes cannot overlap
		// (e.g. two `{ int x; }` blocks that are never live at the same time): this phase does no
		// stack-slot coalescing, exactly the same "simplest thing that works" call §10 makes for
		// register allocation.
		u32 newLocalSlot(u32 sizeInBytes, bool isFloat, bool isVolatile = false, bool preferRegister = false)
		{
			_localSlots.push_back(IrLocalSlot{ sizeInBytes, isFloat, isVolatile, preferRegister });
			return static_cast<u32>(_localSlots.size() - 1);
		}

		// Widens an already-reserved slot so it can also hold a second local of a different type -
		// what IrBuilder's scope-based slot reuse needs when the local now taking over a dead
		// sibling scope's slot is wider than the one that had it (see IrBuilder's own
		// `reuseLocalSlot` note, ir_builder.cpp). Never narrows: a slot only ever grows to the
		// widest thing that has lived in it, exactly like a union's size.
		void widenLocalSlot(u32 index, u32 sizeInBytes, bool isFloat) noexcept
		{
			IrLocalSlot& slot = _localSlots[index];
			if (sizeInBytes > slot.sizeInBytes)
				slot.sizeInBytes = sizeInBytes;
			// f32 fields are exactly one word. A wider reused slot must use an integer-word array,
			// otherwise codegen would declare only four bytes for a larger object.
			if (slot.sizeInBytes == 4)
				slot.isFloat = isFloat;
			else
				slot.isFloat = false;
		}

		// Drops every block whose index in blocks() has `keep[i] == false` - unreachable-block
		// elimination (ir_optimizer.h). Block ids are deliberately NOT renumbered: a BasicBlock* in
		// some surviving terminator's payload keeps pointing at the same block, and codegen's
		// `.L<id>` labels stay stable, so the numbering just develops gaps. Only ever called with
		// blocks nothing reachable branches to, which is what makes dropping them safe at all.
		void retainBlocks(const std::vector<bool>& keep)
		{
			std::vector<std::unique_ptr<BasicBlock>> remaining;
			remaining.reserve(_blocks.size());
			for (usize i = 0; i < _blocks.size(); ++i)
			{
				if (keep.size() <= i || keep[i])
					remaining.push_back(std::move(_blocks[i]));
			}
			_blocks = std::move(remaining);
		}
	};

	// The blocks control can reach from the one at `blockIndex`. Shared by every CFG walk over a
	// function (ir_optimizer.cpp's reachability, libs/codegen's liveness) so they all agree on one
	// subtle case: a block WITHOUT a terminator falls through to the next one in order, which is
	// exactly how codegen emits them - it never inserts a jump between consecutive blocks.
	inline std::vector<BasicBlock*> successorsOf(const IrFunction& function, usize blockIndex)
	{
		std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
		std::span<IrInstr* const> instrs = blocks[blockIndex]->instrs();

		if (!instrs.empty())
		{
			const IrInstr& last = *instrs.back();
			switch (last.opcode())
			{
				case IrOpcode::Jump: return { last.as<IrJumpPayload>().target };
				case IrOpcode::CondJump:
				{
					const IrCondJumpPayload& p = last.as<IrCondJumpPayload>();
					if (p.trueTarget == p.falseTarget)
						return { p.trueTarget };
					return { p.trueTarget, p.falseTarget };
				}
				case IrOpcode::Return: return {};
				default: break;
			}
		}
		if (blockIndex + 1 < blocks.size())
			return { blocks[blockIndex + 1].get() };
		return {};
	}

	// A string literal's synthesized global label and the interned value it names - see
	// ir_builder.cpp's visit(StringLiteralExpr&). Owned by IrModule so the same `name` a GlobalAddr
	// (ir_instr.h) or an IrPrinter/codegen consumer sees can be resolved back to the actual bytes to
	// emit as `.rodata` (§10) - IrInstr/IrFunction never carry a literal's content themselves, only
	// its label.
	struct IrGlobalString
	{
		std::string_view name;
		support::PooledString value;
	};

	// A `static` local: spelled inside a function, but stored like a file-scope variable, because it
	// has to outlive the call. Lowering turns every reference to it into a GlobalAddr of `name`, and
	// codegen emits the storage from `decl` exactly as it emits a real global - the only differences
	// are that `name` carries the function's name too (so two functions may each have their own
	// `count`) and that it is never `global`, since C gives it no linkage at all.
	struct IrStaticLocal
	{
		std::string_view name;      // the file-scope symbol, e.g. "main.calls"
		const ast::VarDecl* decl;   // the declaration its type and initializer come from
	};

	// The result of lowering one whole TranslationUnit: one IrFunction per FunctionDecl definition
	// (a prototype with no body lowers to nothing - see ir_builder.cpp), plus the string literal
	// table every GlobalAddr referencing a string constant points into.
	class IrModule
	{
	private:
		std::vector<std::unique_ptr<IrFunction>> _functions;
		std::vector<IrGlobalString> _stringLiterals;
		std::vector<IrStaticLocal> _staticLocals;

	public:
		IrModule() = default;
		IrModule(const IrModule&) = delete;
		IrModule(IrModule&&) = default;
		~IrModule() = default;

		IrModule& operator=(const IrModule&) = delete;
		IrModule& operator=(IrModule&&) = default;

	public:
		std::span<const std::unique_ptr<IrFunction>> functions() const noexcept { return _functions; }
		std::span<const IrGlobalString> stringLiterals() const noexcept { return _stringLiterals; }
		std::span<const IrStaticLocal> staticLocals() const noexcept { return _staticLocals; }

		IrFunction& addFunction(std::string_view name, const ast::Type* returnType)
		{
			_functions.push_back(std::make_unique<IrFunction>(name, returnType));
			return *_functions.back();
		}

		void addStringLiteral(std::string_view name, support::PooledString value)
		{
			_stringLiterals.push_back(IrGlobalString{ name, value });
		}

		void addStaticLocal(std::string_view name, const ast::VarDecl* decl)
		{
			_staticLocals.push_back(IrStaticLocal{ name, decl });
		}

		// Drops every function whose index in functions() has `keep[i] == false` - unused-function
		// elimination (ir_optimizer.h), which is only sound because a Ceres-C program is one
		// self-contained translation unit whose single externally reachable symbol is `main`. Note
		// that string literals are NOT pruned alongside: a literal only ever referenced by a dropped
		// function stays in `.rodata`, costing a few bytes rather than risking a dangling reference.
		void retainFunctions(const std::vector<bool>& keep)
		{
			std::vector<std::unique_ptr<IrFunction>> remaining;
			remaining.reserve(_functions.size());
			for (usize i = 0; i < _functions.size(); ++i)
			{
				if (keep.size() <= i || keep[i])
					remaining.push_back(std::move(_functions[i]));
			}
			_functions = std::move(remaining);
		}
	};
}
