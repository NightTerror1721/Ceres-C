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
	};

	class IrFunction
	{
	private:
		std::string_view _name;
		const ast::Type* _returnType = nullptr;
		u32 _paramCount = 0;
		u32 _localCount = 0; // includes params - see newLocalSlot()/reserveParamSlots()
		std::vector<std::unique_ptr<BasicBlock>> _blocks;
		u32 _nextTempId = 0;

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
		u32 localCount() const noexcept { return _localCount; }
		std::span<const std::unique_ptr<BasicBlock>> blocks() const noexcept { return _blocks; }

		BasicBlock& createBlock()
		{
			_blocks.push_back(std::make_unique<BasicBlock>(static_cast<u32>(_blocks.size())));
			return *_blocks.back();
		}

		IrValue newTemp() noexcept { return IrValue{ _nextTempId++ }; }

		// Called exactly once, right after construction, before any real local's slot is reserved -
		// reserves frame slots [0, paramCount) for the function's own parameters, in declaration
		// order, so IrBuilder can map a Param to a slot by its plain position instead of tracking one
		// separately.
		void reserveParamSlots(u32 paramCount) noexcept
		{
			_paramCount = paramCount;
			_localCount = paramCount;
		}

		// Reserves the next local frame slot (see ir_instr.h's IrFrameAddrPayload::localIndex) and
		// returns its index. Called once per local VarDecl as IrBuilder encounters it in the body.
		// Slots are never reused across sibling blocks even when their lifetimes cannot overlap
		// (e.g. two `{ int x; }` blocks that are never live at the same time): this phase does no
		// stack-slot coalescing, exactly the same "simplest thing that works" call §10 makes for
		// register allocation.
		u32 newLocalSlot() noexcept { return _localCount++; }
	};

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

	// The result of lowering one whole TranslationUnit: one IrFunction per FunctionDecl definition
	// (a prototype with no body lowers to nothing - see ir_builder.cpp), plus the string literal
	// table every GlobalAddr referencing a string constant points into.
	class IrModule
	{
	private:
		std::vector<std::unique_ptr<IrFunction>> _functions;
		std::vector<IrGlobalString> _stringLiterals;

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

		IrFunction& addFunction(std::string_view name, const ast::Type* returnType)
		{
			_functions.push_back(std::make_unique<IrFunction>(name, returnType));
			return *_functions.back();
		}

		void addStringLiteral(std::string_view name, support::PooledString value)
		{
			_stringLiterals.push_back(IrGlobalString{ name, value });
		}
	};
}
