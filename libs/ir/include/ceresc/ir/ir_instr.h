#pragma once

// IrInstr - the three-address-code opcode set: Const, BinOp, UnOp, Cmp, Copy, FrameAddr,
// GlobalAddr, Load, Store, Param, Call, Jump, CondJump, Return.
//
// Deliberately not SSA and with no dominator tree - a BasicBlock is a flat, linear list of these,
// terminated by a jump or a return. There is no setcc-equivalent in the CASM ISA: a comparison
// used as a value gets synthesized by codegen (libs/codegen) as a short branch, not lowered here.
// See the architecture plan, §9.
//
// Implemented in Fase 5 of the phased plan (§13).

namespace ceresc::ir
{
}
