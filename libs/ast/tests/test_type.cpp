#include <ceresc/ast/decl.h>
#include <ceresc/support/arena.h>

#include "framework.h"

// Type::sizeInBytes()/alignment()/isSigned() (type.cpp) - covers the edge branches
// libs/sema/tests/test_sema.cpp only exercises indirectly through whole checked programs:
// primitive/pointer sizes, an incomplete StructDecl's 0/1 sentinel, the struct-layout padding
// rule itself, the array-size overflow guard, and the MaxLayoutDepth backstop against a
// (semantically illegal, but structurally constructible) self-referential-by-value struct.

using namespace ceresc;
using namespace ceresc::ast;

namespace
{
	support::SourceLocation loc() { return support::SourceLocation(support::SourceId::make(1), 1, 1, 0); }
}

TEST(type, primitive_sizes_and_alignments)
{
	CHECK_EQ(Type::Bool.sizeInBytes(), 1u);
	CHECK_EQ(Type::Char.sizeInBytes(), 1u);
	CHECK_EQ(Type::Short.sizeInBytes(), 2u);
	CHECK_EQ(Type::Int.sizeInBytes(), 4u);
	CHECK_EQ(Type::Long.sizeInBytes(), 4u); // int-sized in this ABI - Ceres has no 64-bit register
	CHECK_EQ(Type::Float.sizeInBytes(), 4u);

	CHECK_EQ(Type::Bool.alignment(), 1u);
	CHECK_EQ(Type::Short.alignment(), 2u);
	CHECK_EQ(Type::Int.alignment(), 4u);
}

TEST(type, pointer_is_always_four_bytes)
{
	support::Arena arena;
	const Type* intPtr = Type::makePointer(arena, &Type::Int);
	CHECK_EQ(intPtr->sizeInBytes(), 4u);
	CHECK_EQ(intPtr->alignment(), 4u);
}

TEST(type, withConst_preserves_volatile_and_qualifies_array_elements)
{
	support::Arena arena;
	CHECK(Type::withConst(arena, &Type::VolatileInt) == &Type::ConstVolatileInt);

	const Type* array = Type::makeArray(arena, &Type::Int, 3);
	const Type* qualified = Type::withConst(arena, array);
	CHECK(qualified->isArray());
	CHECK(!qualified->isConst());
	CHECK(qualified->arrayElementType()->isConst());
}

TEST(type, withConst_on_void_keeps_the_qualifier)
{
	// `const void*` is what memcpy's source and memcmp's operands are. withConst() used to hand
	// `void` back unchanged, so every `const void*` silently became `void*`.
	support::Arena arena;
	const Type* constVoid = Type::withConst(arena, &Type::Void);
	CHECK(constVoid->isVoid());
	CHECK(constVoid->isConst());
	CHECK(!(*constVoid == Type::Void));
	CHECK(!Type::Void.isConst());
	CHECK(Type::withoutQualifiers(arena, constVoid) == &Type::Void);
}

TEST(type, structurally_equal_compound_types_compare_equal)
{
	support::Arena arena;
	const Type* first = Type::makePointer(arena, Type::makePointer(arena, &Type::Int));
	const Type* second = Type::makePointer(arena, Type::makePointer(arena, &Type::Int));
	CHECK(*first == *second);
}

TEST(type, isSigned_matches_C_signedness)
{
	CHECK(Type::Char.isSigned());
	CHECK(Type::SChar.isSigned());
	CHECK(Type::Short.isSigned());
	CHECK(Type::Int.isSigned());
	CHECK(Type::Long.isSigned());

	CHECK(!Type::UChar.isSigned());
	CHECK(!Type::UShort.isSigned());
	CHECK(!Type::UInt.isSigned());
	CHECK(!Type::ULong.isSigned());
	CHECK(!Type::Bool.isSigned());
	CHECK(!Type::Float.isSigned());
}

TEST(type, isSigned_treats_enum_as_signed_int_matching_integerPromote)
{
	// An enum is int-sized and int-valued (see type.cpp's own note), and sema's integerPromote()
	// widens it straight to the signed Type::Int - isSigned() must agree, not silently say false.
	support::Arena arena;
	EnumDecl* enumDecl = arena.create<EnumDecl>(loc(), std::string_view("Color"));
	const Type* colorType = Type::makeEnum(arena, enumDecl);
	CHECK(colorType->isSigned());
}

TEST(type, incomplete_struct_reports_zero_size_and_unit_alignment)
{
	support::Arena arena;
	StructDecl* structDecl = arena.create<StructDecl>(loc(), std::string_view("Node"));
	const Type* nodeType = Type::makeStruct(arena, structDecl);
	CHECK_EQ(nodeType->sizeInBytes(), 0u);
	CHECK_EQ(nodeType->alignment(), 1u);
}

TEST(type, complete_struct_follows_CASMs_pad_to_own_align_round_to_widest_rule)
{
	// char c (1 byte) + int i (4 bytes, so `i` pads to offset 4) + short s (2 bytes, offset 8) =
	// 10 raw bytes, rounded up to the widest field's alignment (4) = 12. Same rule
	// docs/23-Structs.md documents for CASM's own `struct`.
	support::Arena arena;
	StructDecl* structDecl = arena.create<StructDecl>(loc(), std::string_view("Mixed"));
	FieldDecl fields[] = {
		FieldDecl{ &Type::Char, std::string_view("c"), loc() },
		FieldDecl{ &Type::Int, std::string_view("i"), loc() },
		FieldDecl{ &Type::Short, std::string_view("s"), loc() },
	};
	structDecl->setFields(std::span<const FieldDecl>(fields, 3));

	const Type* mixedType = Type::makeStruct(arena, structDecl);
	CHECK_EQ(mixedType->sizeInBytes(), 12u);
	CHECK_EQ(mixedType->alignment(), 4u);
}

TEST(type, array_size_is_element_size_times_count)
{
	support::Arena arena;
	const Type* tenInts = Type::makeArray(arena, &Type::Int, 10);
	CHECK_EQ(tenInts->sizeInBytes(), 40u);
	CHECK_EQ(tenInts->alignment(), 4u);
}

TEST(type, array_size_overflow_reports_zero_instead_of_wrapping)
{
	// 4 bytes/element * 0xFFFFFFFF elements overflows u32 many times over - sizeInBytes() must
	// report the same "can't tell you a real size" sentinel an incomplete struct uses (0), not a
	// wrapped, silently-small value that would flow straight into offset/allocation math.
	support::Arena arena;
	const Type* hugeArray = Type::makeArray(arena, &Type::Int, 0xFFFFFFFFu);
	CHECK_EQ(hugeArray->sizeInBytes(), 0u);
}

TEST(type, self_referential_by_value_struct_does_not_crash_sizeInBytes)
{
	// `struct Node { struct Node inner; };` is semantically illegal (sema::validateStructLayout()
	// is what actually diagnoses it - see test_sema.cpp), but the parser can still construct this
	// shape, so Type::sizeInBytes()/alignment() must survive being called on it directly (e.g. by
	// a test, or anything that runs before sema) instead of recursing forever.
	support::Arena arena;
	StructDecl* node = arena.create<StructDecl>(loc(), std::string_view("Node"));
	const Type* selfType = Type::makeStruct(arena, node);
	FieldDecl fields[] = { FieldDecl{ selfType, std::string_view("inner"), loc() } };
	node->setFields(std::span<const FieldDecl>(fields, 1));

	CHECK_EQ(selfType->sizeInBytes(), 0u);
	CHECK_EQ(selfType->alignment(), 1u);
}
