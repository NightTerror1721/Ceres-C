#pragma once

#include <ceresc/ir/ir_function.h>
#include <string>
#include <string_view>

// IrPrinter - renders an IrModule/IrFunction as the readable three-address text §9 describes
// (`%t = op %a, %b`, `L3:`, `jmp L1`, ...).
//
// Two reasons this exists, mirroring AstPrinter's own (libs/ast/ast_printer.h): it is the
// compiler's own --emit-ir debugging output, and it is what tests/ir compares against - neither
// IrInstr nor BasicBlock has a std::formatter, so a test can't CHECK_EQ two functions' IR directly.
// Printing both to text and comparing strings is the same trick test_lexer.cpp/AstPrinter already
// use for tokens/AST nodes.
//
// Implemented in Fase 5 of the phased plan (§13), alongside IrBuilder.

namespace ceresc::ir
{
	class IrPrinter final
	{
	private:
		std::string _output;

	public:
		IrPrinter() = default;
		IrPrinter(const IrPrinter&) = delete;
		IrPrinter(IrPrinter&&) = delete;
		~IrPrinter() = default;

		IrPrinter& operator=(const IrPrinter&) = delete;
		IrPrinter& operator=(IrPrinter&&) = delete;

	public:
		// Resets internal state and returns the printed form of `module`/`function`.
		std::string print(const IrModule& module);
		std::string print(const IrFunction& function);

	private:
		void printFunction(const IrFunction& function);
		void printBlock(const BasicBlock& block);
		void printInstr(const IrInstr& instr);

	public:
		static std::string valueName(IrValue value);
		static std::string blockName(const BasicBlock& block);
		static std::string_view opName(IrBinOp op) noexcept;
		static std::string_view opName(IrUnOp op) noexcept;
		static std::string_view predName(IrCmpPredicate predicate) noexcept;
		static std::string_view sizeName(IrMemSize size) noexcept;
	};
}
