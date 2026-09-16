#include <ceresc/ast/type.h>
#include <ceresc/ast/decl.h>
#include <limits>

// sizeInBytes()/alignment()/isSigned() - declared in type.h, implemented here because the Struct
// case needs StructDecl's complete definition (type.h only forward-declares it, see decl.h's own
// header comment on why an actual #include would be a real cycle).
//
// Struct layout follows the exact rule CeresASM's own `struct` does (docs/23-Structs.md): fields
// in declaration order, each padded up to its own natural alignment, the total rounded up to the
// widest field's alignment. Primitive sizes/alignments mirror CASM's data_type.h
// (docs/11-Data-Types-and-Literals.md): u8/i8=1, u16/i16=2, u32/i32/f32=4, and a Ceres address
// (Pointer) is a u32, so 4 bytes as well. `long`/`unsigned long` are int-sized in this ABI - Ceres
// has no native 64-bit register (see type.h's own note), so there is no wider integer to give them.
// `double` is unreachable from a valid program (the parser rejects it - see type.h) and its size
// here is a placeholder, never actually relied on. Enum is always int-sized, same as most C ABIs -
// it does not need a StructDecl-style computed layout.
//
// Both walks take an explicit recursion depth and bail out past a small limit instead of
// recursing forever: a struct that (illegally) contains itself by value, directly or through
// another struct, would otherwise stack-overflow here. sema (libs/sema/type_layout.h) is what
// actually diagnoses that cycle as a semantic error; this is just a defensive backstop so calling
// sizeInBytes()/alignment() directly (e.g. from a test, or before sema has run) can never crash.

namespace ceresc::ast
{
	namespace
	{
		constexpr u32 MaxLayoutDepth = 64;

		u32 alignmentOf(const Type* type, u32 depth) noexcept;

		u32 sizeOf(const Type* type, u32 depth) noexcept
		{
			if (!type || depth > MaxLayoutDepth)
				return 0;

			switch (type->kind())
			{
				case TypeKind::Void: return 0;
				case TypeKind::Bool: return 1;
				case TypeKind::Char: return 1;
				case TypeKind::UChar: return 1;
				case TypeKind::SChar: return 1;
				case TypeKind::Short: return 2;
				case TypeKind::UShort: return 2;
				case TypeKind::Int: return 4;
				case TypeKind::UInt: return 4;
				case TypeKind::Long: return 4;
				case TypeKind::ULong: return 4;
				case TypeKind::Float: return 4;
				case TypeKind::Double: return 8;
				case TypeKind::Pointer: return 4;
				case TypeKind::Array:
				{
					// u32*u32 can overflow for a large element size times a large count (e.g. a
					// multi-dimensional array) - compute in u64 and report 0 (same "can't tell you
					// a real size" sentinel an incomplete struct already uses) rather than silently
					// wrapping to a small, wrong value that would flow straight into offsets/alloc math.
					const u64 bytes = static_cast<u64>(sizeOf(type->arrayElementType(), depth + 1)) * type->arraySize();
					return bytes > (std::numeric_limits<u32>::max)() ? 0 : static_cast<u32>(bytes);
				}
				case TypeKind::Enum: return 4;
				case TypeKind::Struct:
				case TypeKind::Union:
				{
					StructDecl* decl = type->structDecl();
					if (!decl || !decl->isComplete())
						return 0;

					u32 size = 0;
					u32 maxAlign = 1;
					for (const FieldDecl& field : decl->fields())
					{
						if (!field.type)
							continue;
						u32 fieldAlign = alignmentOf(field.type, depth + 1);
						size = decl->isUnion() ? std::max(size, sizeOf(field.type, depth + 1))
							: alignUp(size, fieldAlign) + sizeOf(field.type, depth + 1);
						maxAlign = fieldAlign > maxAlign ? fieldAlign : maxAlign;
					}
					return alignUp(size, maxAlign);
				}
			}
			return 0;
		}

		u32 alignmentOf(const Type* type, u32 depth) noexcept
		{
			if (!type || depth > MaxLayoutDepth)
				return 1;

			switch (type->kind())
			{
				case TypeKind::Void: return 1;
				case TypeKind::Bool: case TypeKind::Char: case TypeKind::UChar: case TypeKind::SChar: return 1;
				case TypeKind::Short: case TypeKind::UShort: return 2;
				case TypeKind::Int: case TypeKind::UInt: case TypeKind::Long: case TypeKind::ULong: return 4;
				case TypeKind::Float: return 4;
				case TypeKind::Double: return 8;
				case TypeKind::Pointer: return 4;
				case TypeKind::Array: return alignmentOf(type->arrayElementType(), depth + 1);
				case TypeKind::Enum: return 4;
				case TypeKind::Struct:
				case TypeKind::Union:
				{
					StructDecl* decl = type->structDecl();
					if (!decl || !decl->isComplete())
						return 1;

					u32 maxAlign = 1;
					for (const FieldDecl& field : decl->fields())
					{
						if (!field.type)
							continue;
						u32 fieldAlign = alignmentOf(field.type, depth + 1);
						maxAlign = fieldAlign > maxAlign ? fieldAlign : maxAlign;
					}
					return maxAlign;
				}
			}
			return 1;
		}
	}

	u32 Type::sizeInBytes() const noexcept { return sizeOf(this, 0); }
	u32 Type::alignment() const noexcept { return alignmentOf(this, 0); }

	bool Type::operator==(const Type& other) const noexcept
	{
		if (_kind != other._kind || _const != other._const || _volatile != other._volatile || _arraySize != other._arraySize)
			return false;
		const Type* element = arrayElementType();
		const Type* otherElement = other.arrayElementType();
		if (element || otherElement)
			return element && otherElement && *element == *otherElement;
		return _payload == other._payload;
	}

	bool Type::isSigned() const noexcept
	{
		switch (_kind)
		{
			case TypeKind::Char:
			case TypeKind::SChar:
			case TypeKind::Short:
			case TypeKind::Int:
			case TypeKind::Long:
			case TypeKind::Enum: // int-sized and int-valued, same as integerPromote() in sema.cpp
				return true;
			default:
				return false;
		}
	}

	// ---- qualifier changes -----------------------------------------------------------------------
	//
	// A scalar maps to one of the statics declared alongside it in type.h, so `const int` is the
	// same object everywhere and costs no arena at all. A compound type (pointer, array, struct,
	// enum) carries a payload, so it needs a copy - there is no static to reach for.

	const Type* Type::withConst(support::Arena& arena, const Type* type) noexcept
	{
		if (!type || type->isConst())
			return type;
		switch (type->kind())
		{
			case TypeKind::Void:   return type; // `const void` is not a thing you can have one of
			case TypeKind::Bool:   return type->isVolatile() ? &Type::ConstVolatileBool : &Type::ConstBool;
			case TypeKind::Char:   return type->isVolatile() ? &Type::ConstVolatileChar : &Type::ConstChar;
			case TypeKind::UChar:  return type->isVolatile() ? &Type::ConstVolatileUChar : &Type::ConstUChar;
			case TypeKind::SChar:  return type->isVolatile() ? &Type::ConstVolatileSChar : &Type::ConstSChar;
			case TypeKind::Short:  return type->isVolatile() ? &Type::ConstVolatileShort : &Type::ConstShort;
			case TypeKind::UShort: return type->isVolatile() ? &Type::ConstVolatileUShort : &Type::ConstUShort;
			case TypeKind::Int:    return type->isVolatile() ? &Type::ConstVolatileInt : &Type::ConstInt;
			case TypeKind::UInt:   return type->isVolatile() ? &Type::ConstVolatileUInt : &Type::ConstUInt;
			case TypeKind::Long:   return type->isVolatile() ? &Type::ConstVolatileLong : &Type::ConstLong;
			case TypeKind::ULong:  return type->isVolatile() ? &Type::ConstVolatileULong : &Type::ConstULong;
			case TypeKind::Float:  return type->isVolatile() ? &Type::ConstVolatileFloat : &Type::ConstFloat;
			case TypeKind::Double: return type->isVolatile() ? &Type::ConstVolatileDouble : &Type::ConstDouble;
			default:
				break;
		}
		if (type->isArray())
			return makeArray(arena, withConst(arena, type->arrayElementType()), type->arraySize(), false, type->isVolatile());
		return makeCompound(arena, type->kind(), true, type->isVolatile(), type->_payload, type->_arraySize);
	}

	const Type* Type::withoutQualifiers(support::Arena& arena, const Type* type) noexcept
	{
		if (!type || (!type->isConst() && !type->isVolatile()))
			return type;
		switch (type->kind())
		{
			case TypeKind::Void:   return &Type::Void;
			case TypeKind::Bool:   return &Type::Bool;
			case TypeKind::Char:   return &Type::Char;
			case TypeKind::UChar:  return &Type::UChar;
			case TypeKind::SChar:  return &Type::SChar;
			case TypeKind::Short:  return &Type::Short;
			case TypeKind::UShort: return &Type::UShort;
			case TypeKind::Int:    return &Type::Int;
			case TypeKind::UInt:   return &Type::UInt;
			case TypeKind::Long:   return &Type::Long;
			case TypeKind::ULong:  return &Type::ULong;
			case TypeKind::Float:  return &Type::Float;
			case TypeKind::Double: return &Type::Double;
			default:
				break;
		}
		return makeCompound(arena, type->kind(), false, false, type->_payload, type->_arraySize);
	}
}
