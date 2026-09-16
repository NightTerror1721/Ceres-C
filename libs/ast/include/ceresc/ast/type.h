#pragma once

#include <ceresc/support/types.h>
#include <ceresc/support/arena.h>
#include <variant>

// Type - the C subset's type system: Void, Char, Short, Int, UInt, Float, Bool, Pointer, Array,
// Struct.
//
// Unlike the AST node hierarchy in this same library, Type uses std::variant on purpose: it's a
// small, genuinely closed set that does not grow the way the AST does across phases. Carries
// sizeInBytes()/alignment()/isSigned() - alignment follows the same rule as CASM's own `struct`
// (each field aligned to its own size, see §8). See the architecture plan, §6.
//
// Float and Bool are V1 scope, not yet reflected in this list of members - ordinary phased work
// (the lexer already has KwFloat/KwBool/LiteralFloat/LiteralBool, see token.h), this variant just
// hasn't grown to include them yet. Float is `float` only (f32): it maps directly onto Ceres's
// native F32 register/DataType (see CeresASM's fregisters.h/data_type.h), so codegen is close to a
// straight passthrough. `double` (f64) is a different story and stays reserved for a version after
// v1: Ceres has no f64 support anywhere in the VM, so it would need real work (software emulation)
// instead of being free like float. Once the parser exists, it must reject `double` with a "not
// implemented in this version" diagnostic rather than trying to fit it in here early. `const` is a
// qualifier on a Type, not its own TypeKind member - how it attaches (a bit on Type vs. a wrapper)
// is still open, see §14.
//
// This variant is also narrower than the integer keywords the lexer already recognizes: `short`/
// `long`/`signed`/`unsigned` are meant to combine the way they do in real C (see token.h), which
// this enumeration doesn't reflect yet (no Long/ULong, and no separate signedness for Char/Short).
// It needs to grow when the parser's type-name grammar lands to cover every valid combination -
// and to reject the invalid ones (e.g. `short long`) - rather than staying stuck at today's shape.
//
// Implemented across Fase 2-4 of the phased plan (§13): the shape lands with the parser's
// type-name grammar, sizeInBytes()/alignment() with sema's struct layout.

namespace ceresc::ast
{
	class StructDecl;
	class EnumDecl;

	enum class TypeKind : u8
	{
		Void,
		Bool,
		Char,
		UChar,
		SChar,
		Short,
		UShort,
		Int,
		UInt,
		Long,
		ULong,
		Float,
		Double, // reserved for a version after v1 (Ceres has no f64 support anywhere)
		Pointer,
		Array,
		Struct,
		Union,
		Enum
	};

	class Type
	{
	public:
		using PayloadType = std::variant<std::monostate, const Type*, StructDecl*, EnumDecl*>;

	private:
		TypeKind _kind = TypeKind::Void;
		bool _const = false;
		bool _volatile = false;
		PayloadType _payload = std::monostate{};
		u32 _arraySize = 0; // only used for Array kind

	public:
		constexpr Type() noexcept = delete;
		constexpr Type(const Type&) noexcept = default;
		constexpr Type(Type&&) noexcept = default;
		constexpr ~Type() noexcept = default;

		constexpr Type& operator=(const Type&) noexcept = default;
		constexpr Type& operator=(Type&&) noexcept = default;

		bool operator==(const Type& other) const noexcept;

	private:
		constexpr explicit Type(TypeKind kind, bool isConst, bool isVolatile, PayloadType payload = std::monostate{}, u32 arraySize = 0) noexcept :
			_kind(kind), _const(isConst), _volatile(isVolatile), _payload(std::move(payload)), _arraySize(arraySize)
		{}

	public:
		constexpr TypeKind kind() const noexcept { return _kind; }
		constexpr bool isConst() const noexcept { return _const; }
		constexpr bool isVolatile() const noexcept { return _volatile; }
		constexpr u32 arraySize() const noexcept { return _arraySize; }

		constexpr const Type* arrayElementType() const noexcept { return std::get_if<const Type*>(&_payload) ? std::get<const Type*>(_payload) : nullptr; }
		constexpr StructDecl* structDecl() const noexcept { return std::get_if<StructDecl*>(&_payload) ? std::get<StructDecl*>(_payload) : nullptr; }
		constexpr EnumDecl* enumDecl() const noexcept { return std::get_if<EnumDecl*>(&_payload) ? std::get<EnumDecl*>(_payload) : nullptr; }

		constexpr bool isVoid() const noexcept { return _kind == TypeKind::Void; }
		constexpr bool isBool() const noexcept { return _kind == TypeKind::Bool; }
		constexpr bool isChar() const noexcept { return _kind == TypeKind::Char; }
		constexpr bool isUChar() const noexcept { return _kind == TypeKind::UChar; }
		constexpr bool isSChar() const noexcept { return _kind == TypeKind::SChar; }
		constexpr bool isShort() const noexcept { return _kind == TypeKind::Short; }
		constexpr bool isUShort() const noexcept { return _kind == TypeKind::UShort; }
		constexpr bool isInt() const noexcept { return _kind == TypeKind::Int; }
		constexpr bool isUInt() const noexcept { return _kind == TypeKind::UInt; }
		constexpr bool isLong() const noexcept { return _kind == TypeKind::Long; }
		constexpr bool isULong() const noexcept { return _kind == TypeKind::ULong; }
		constexpr bool isFloat() const noexcept { return _kind == TypeKind::Float; }
		constexpr bool isDouble() const noexcept { return _kind == TypeKind::Double; }

		constexpr bool isPointer() const noexcept { return _kind == TypeKind::Pointer; }
		constexpr bool isArray() const noexcept { return _kind == TypeKind::Array; }
		constexpr bool isStruct() const noexcept { return _kind == TypeKind::Struct; }
		constexpr bool isUnion() const noexcept { return _kind == TypeKind::Union; }
		constexpr bool isAggregate() const noexcept { return isStruct() || isUnion(); }
		constexpr bool isEnum() const noexcept { return _kind == TypeKind::Enum; }

		// Not constexpr: the Struct case walks StructDecl::fields(), which needs the complete
		// class (only forward-declared here - see the header comment above), so these three are
		// implemented in type.cpp instead of inline. sizeInBytes()/alignment() follow the same
		// rule CASM's own struct layout does (each field aligned to its own size, the total
		// rounded up to the widest field - see §8 and libs/sema/type_layout.h's own note), which
		// is why this only becomes meaningful once sema's struct-layout work exists to define that
		// rule for the codebase; a Struct type on an incomplete StructDecl reports sizeInBytes() 0
		// and alignment() 1 (alignmentOf()'s `1` doubles as its "alignment unknown" answer).
		u32 sizeInBytes() const noexcept;
		u32 alignment() const noexcept;
		bool isSigned() const noexcept;

	private:
		static forceinline const Type* makeCompound(support::Arena& arena, TypeKind kind, bool isConst, bool isVolatile, PayloadType payload, u32 arraySize = 0) noexcept
		{
			return arena.create<Type>(std::move(Type(kind, isConst, isVolatile, std::move(payload), arraySize)));
		}

	public:
		static const Type* makePointer(support::Arena& arena, const Type* pointeeType, bool isConst = false, bool isVolatile = false) noexcept
		{
			return makeCompound(arena, TypeKind::Pointer, isConst, isVolatile, pointeeType);
		}
		static const Type* makeArray(support::Arena& arena, const Type* elementType, u32 arraySize, bool isConst = false, bool isVolatile = false) noexcept
		{
			return makeCompound(arena, TypeKind::Array, isConst, isVolatile, elementType, arraySize);
		}
		static const Type* makeStruct(support::Arena& arena, StructDecl* structDecl, bool isConst = false, bool isVolatile = false) noexcept
		{
			return makeCompound(arena, TypeKind::Struct, isConst, isVolatile, structDecl);
		}
		static const Type* makeUnion(support::Arena& arena, StructDecl* unionDecl, bool isConst = false, bool isVolatile = false) noexcept
		{
			return makeCompound(arena, TypeKind::Union, isConst, isVolatile, unionDecl);
		}
		static const Type* makeEnum(support::Arena& arena, EnumDecl* enumDecl, bool isConst = false, bool isVolatile = false) noexcept
		{
			return makeCompound(arena, TypeKind::Enum, isConst, isVolatile, enumDecl);
		}

		// `type` with its const qualifier set, without changing anything else. Returns `type` itself
		// when it is already const, and one of the ConstXxx statics above for a scalar, so the common
		// cases allocate nothing; only a qualified pointer/array/struct/enum needs a new Type.
		//
		// Implemented in type.cpp rather than inline: the scalar mapping is a switch over every
		// TypeKind, which is a lot of code to put in a header for something no caller inlines.
		static const Type* withConst(support::Arena& arena, const Type* type) noexcept;

		// The same type with every qualifier dropped - what a comparison that should ignore const
		// asks for. `int` and `const int` are different types to operator==, which is right for
		// assignability but wrong for "do these two pointers point at the same thing".
		static const Type* withoutQualifiers(support::Arena& arena, const Type* type) noexcept;

	public:
		static const Type Void;
		static const Type Bool, ConstBool, VolatileBool, ConstVolatileBool;
		static const Type Char, ConstChar, VolatileChar, ConstVolatileChar;
		static const Type UChar, ConstUChar, VolatileUChar, ConstVolatileUChar;
		static const Type SChar, ConstSChar, VolatileSChar, ConstVolatileSChar;
		static const Type Short, ConstShort, VolatileShort, ConstVolatileShort;
		static const Type UShort, ConstUShort, VolatileUShort, ConstVolatileUShort;
		static const Type Int, ConstInt, VolatileInt, ConstVolatileInt;
		static const Type UInt, ConstUInt, VolatileUInt, ConstVolatileUInt;
		static const Type Long, ConstLong, VolatileLong, ConstVolatileLong;
		static const Type ULong, ConstULong, VolatileULong, ConstVolatileULong;
		static const Type Float, ConstFloat, VolatileFloat, ConstVolatileFloat;
		static const Type Double, ConstDouble, VolatileDouble, ConstVolatileDouble;
	};

	inline constexpr Type Type::Void{ TypeKind::Void, false, false };
	inline constexpr Type Type::Bool{ TypeKind::Bool, false, false };
	inline constexpr Type Type::ConstBool{ TypeKind::Bool, true, false };
	inline constexpr Type Type::VolatileBool{ TypeKind::Bool, false, true };
	inline constexpr Type Type::ConstVolatileBool{ TypeKind::Bool, true, true };
	inline constexpr Type Type::Char{ TypeKind::Char, false, false };
	inline constexpr Type Type::ConstChar{ TypeKind::Char, true, false };
	inline constexpr Type Type::VolatileChar{ TypeKind::Char, false, true };
	inline constexpr Type Type::ConstVolatileChar{ TypeKind::Char, true, true };
	inline constexpr Type Type::UChar{ TypeKind::UChar, false, false };
	inline constexpr Type Type::ConstUChar{ TypeKind::UChar, true, false };
	inline constexpr Type Type::VolatileUChar{ TypeKind::UChar, false, true };
	inline constexpr Type Type::ConstVolatileUChar{ TypeKind::UChar, true, true };
	inline constexpr Type Type::SChar{ TypeKind::SChar, false, false };
	inline constexpr Type Type::ConstSChar{ TypeKind::SChar, true, false };
	inline constexpr Type Type::VolatileSChar{ TypeKind::SChar, false, true };
	inline constexpr Type Type::ConstVolatileSChar{ TypeKind::SChar, true, true };
	inline constexpr Type Type::Short{ TypeKind::Short, false, false };
	inline constexpr Type Type::ConstShort{ TypeKind::Short, true, false };
	inline constexpr Type Type::VolatileShort{ TypeKind::Short, false, true };
	inline constexpr Type Type::ConstVolatileShort{ TypeKind::Short, true, true };
	inline constexpr Type Type::UShort{ TypeKind::UShort, false, false };
	inline constexpr Type Type::ConstUShort{ TypeKind::UShort, true, false };
	inline constexpr Type Type::VolatileUShort{ TypeKind::UShort, false, true };
	inline constexpr Type Type::ConstVolatileUShort{ TypeKind::UShort, true, true };
	inline constexpr Type Type::Int{ TypeKind::Int, false, false };
	inline constexpr Type Type::ConstInt{ TypeKind::Int, true, false };
	inline constexpr Type Type::VolatileInt{ TypeKind::Int, false, true };
	inline constexpr Type Type::ConstVolatileInt{ TypeKind::Int, true, true };
	inline constexpr Type Type::UInt{ TypeKind::UInt, false, false };
	inline constexpr Type Type::ConstUInt{ TypeKind::UInt, true, false };
	inline constexpr Type Type::VolatileUInt{ TypeKind::UInt, false, true };
	inline constexpr Type Type::ConstVolatileUInt{ TypeKind::UInt, true, true };
	inline constexpr Type Type::Long{ TypeKind::Long, false, false };
	inline constexpr Type Type::ConstLong{ TypeKind::Long, true, false };
	inline constexpr Type Type::VolatileLong{ TypeKind::Long, false, true };
	inline constexpr Type Type::ConstVolatileLong{ TypeKind::Long, true, true };
	inline constexpr Type Type::ULong{ TypeKind::ULong, false, false };
	inline constexpr Type Type::ConstULong{ TypeKind::ULong, true, false };
	inline constexpr Type Type::VolatileULong{ TypeKind::ULong, false, true };
	inline constexpr Type Type::ConstVolatileULong{ TypeKind::ULong, true, true };
	inline constexpr Type Type::Float{ TypeKind::Float, false, false };
	inline constexpr Type Type::ConstFloat{ TypeKind::Float, true, false };
	inline constexpr Type Type::VolatileFloat{ TypeKind::Float, false, true };
	inline constexpr Type Type::ConstVolatileFloat{ TypeKind::Float, true, true };
	inline constexpr Type Type::Double{ TypeKind::Double, false, false };
	inline constexpr Type Type::ConstDouble{ TypeKind::Double, true, false };
	inline constexpr Type Type::VolatileDouble{ TypeKind::Double, false, true };
	inline constexpr Type Type::ConstVolatileDouble{ TypeKind::Double, true, true };

	static_assert(TriviallyDestructible<Type>, "Type must be trivially destructible");
}
