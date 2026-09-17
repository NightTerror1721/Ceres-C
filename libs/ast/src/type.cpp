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
// has no native 64-bit register (see type.h's own note), so there is no wider integer to give them,
// and neither `long long` nor `double` reaches here as a kind of its own for the same reason: the
// parser caps both and hands back one of these. Enum is always int-sized, same as most C ABIs - it
// does not need a StructDecl-style computed layout.
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
				case TypeKind::Pointer: return 4;
				// A function type is not an OBJECT type: nothing holds one, so it has no size. Zero is
				// the same answer Void gives, and for the same reason. A POINTER to one is four bytes
				// like every other address, which is the case a program can actually declare.
				case TypeKind::Function: return 0;
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
				// Nothing holds a function, so nothing has to align one - 1 is alignmentOf()'s own
				// "no constraint" answer, the same one Void gets.
				case TypeKind::Function: return 1;
				case TypeKind::Bool: case TypeKind::Char: case TypeKind::UChar: case TypeKind::SChar: return 1;
				case TypeKind::Short: case TypeKind::UShort: return 2;
				case TypeKind::Int: case TypeKind::UInt: case TypeKind::Long: case TypeKind::ULong: return 4;
				case TypeKind::Float: return 4;
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
		if (_kind != other._kind || _const != other._const || _volatile != other._volatile || _restrict != other._restrict || _arraySize != other._arraySize)
			return false;

		// Two function types are the same type when their signatures are, which has to be compared
		// structurally: each `int(int)` written in a program builds its own FunctionTypeInfo, so
		// comparing the pointers would make a prototype and its definition disagree.
		if (_kind == TypeKind::Function)
		{
			const FunctionTypeInfo* self = functionInfo();
			const FunctionTypeInfo* otherInfo = other.functionInfo();
			if (!self || !otherInfo)
				return self == otherInfo;
			if (self->isVariadic != otherInfo->isVariadic || self->paramCount != otherInfo->paramCount)
				return false;
			if (!self->returnType || !otherInfo->returnType || !(*self->returnType == *otherInfo->returnType))
				return false;
			for (u32 i = 0; i < self->paramCount; ++i)
			{
				const Type* a = self->paramTypes[i];
				const Type* b = otherInfo->paramTypes[i];
				if (!a || !b || !(*a == *b))
					return false;
			}
			return true;
		}

		const Type* element = arrayElementType();
		const Type* otherElement = other.arrayElementType();
		if (element || otherElement)
			return element && otherElement && *element == *otherElement;
		return _payload == other._payload;
	}

	const Type* Type::makeFunction(support::Arena& arena, const Type* returnType,
		std::span<const Type* const> paramTypes, bool isVariadic) noexcept
	{
		const Type** stored = nullptr;
		if (!paramTypes.empty())
		{
			void* memory = arena.allocate(sizeof(const Type*) * paramTypes.size(), alignof(const Type*));
			if (!memory)
				return nullptr;
			stored = static_cast<const Type**>(memory);
			for (usize i = 0; i < paramTypes.size(); ++i)
				stored[i] = paramTypes[i];
		}

		FunctionTypeInfo info;
		info.returnType = returnType;
		info.paramTypes = stored;
		info.paramCount = static_cast<u32>(paramTypes.size());
		info.isVariadic = isVariadic;

		// Never qualified: C leaves a qualifier on a function type undefined, and there is nothing
		// for one to mean when the type names no object.
		return makeCompound(arena, TypeKind::Function, false, false, false,
			static_cast<const FunctionTypeInfo*>(arena.create<FunctionTypeInfo>(std::move(info))));
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
			default:
				break;
		}
		if (type->isArray())
			return makeArray(arena, withConst(arena, type->arrayElementType()), type->arraySize(), false, type->isVolatile());
		return makeCompound(arena, type->kind(), true, type->isVolatile(), type->isRestrict(), type->_payload, type->_arraySize);
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
			default:
				break;
		}
		return makeCompound(arena, type->kind(), false, false, false, type->_payload, type->_arraySize);
	}

	const Type* Type::withVolatile(support::Arena& arena, const Type* type) noexcept
	{
		if (!type || type->isVolatile()) return type;
		switch (type->kind())
		{
			case TypeKind::Bool: return type->isConst() ? &Type::ConstVolatileBool : &Type::VolatileBool;
			case TypeKind::Char: return type->isConst() ? &Type::ConstVolatileChar : &Type::VolatileChar;
			case TypeKind::UChar: return type->isConst() ? &Type::ConstVolatileUChar : &Type::VolatileUChar;
			case TypeKind::SChar: return type->isConst() ? &Type::ConstVolatileSChar : &Type::VolatileSChar;
			case TypeKind::Short: return type->isConst() ? &Type::ConstVolatileShort : &Type::VolatileShort;
			case TypeKind::UShort: return type->isConst() ? &Type::ConstVolatileUShort : &Type::VolatileUShort;
			case TypeKind::Int: return type->isConst() ? &Type::ConstVolatileInt : &Type::VolatileInt;
			case TypeKind::UInt: return type->isConst() ? &Type::ConstVolatileUInt : &Type::VolatileUInt;
			case TypeKind::Long: return type->isConst() ? &Type::ConstVolatileLong : &Type::VolatileLong;
			case TypeKind::ULong: return type->isConst() ? &Type::ConstVolatileULong : &Type::VolatileULong;
			case TypeKind::Float: return type->isConst() ? &Type::ConstVolatileFloat : &Type::VolatileFloat;
			default: return makeCompound(arena, type->kind(), type->isConst(), true, type->isRestrict(), type->_payload, type->_arraySize);
		}
	}

	const Type* Type::withRestrict(support::Arena& arena, const Type* type) noexcept
	{
		return !type || type->isRestrict() ? type : makeCompound(arena, type->kind(), type->isConst(), type->isVolatile(), true, type->_payload, type->_arraySize);
	}
}
