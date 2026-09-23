#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/decl.h>
#include <algorithm>
#include <vector>

namespace ceresc::sema
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

	namespace
	{
		// True if `type` is, or (recursively, by value - through a struct field or an array
		// element) contains, `target`. A Pointer field never recurses into its pointee here, only
		// Struct/Array do - see the header comment for why that's exactly the line C draws.
		//
		// `onPath` is the set of StructDecls currently being expanded on this call's own recursion
		// stack: re-entering one of them means a by-value cycle exists that doesn't involve
		// `target` (some other struct's problem to report) - treated as "no path to target through
		// here" so the search can unwind instead of recursing forever.
		//
		// `noCycleMemo` is every StructDecl this call has already fully expanded with a confirmed
		// negative result. Without it, a struct composed like `struct S62 { struct S63 a, b; }`
		// (the same field type repeated, nested many levels deep) re-explores identical subtrees
		// once per sibling field - exponential in nesting depth. Memoizing "already know this one
		// doesn't reach target" bounds the whole walk to one visit per distinct StructDecl instead.
		// Together the two replace the fixed recursion-depth cap this function used to have: a
		// depth cap can only ever approximate an answer (and silently reports "no cycle" once the
		// limit is hit, the wrong default for a validator - see the header's own history), while
		// tracking exactly what's in progress vs. already resolved is both exact and, since a
		// program only ever declares finitely many distinct structs, still guaranteed to terminate.
		bool containsByValue(const ast::Type* type, const ast::StructDecl* target,
			std::vector<const ast::StructDecl*>& onPath, std::vector<const ast::StructDecl*>& noCycleMemo)
		{
			if (!type)
				return false;

			if (type->isArray())
				return containsByValue(type->arrayElementType(), target, onPath, noCycleMemo);

			if (!type->isStruct())
				return false;

			ast::StructDecl* decl = type->structDecl();
			if (decl == target)
				return true;
			if (!decl || !decl->isComplete())
				return false;
			if (std::find(noCycleMemo.begin(), noCycleMemo.end(), decl) != noCycleMemo.end())
				return false;
			if (std::find(onPath.begin(), onPath.end(), decl) != onPath.end())
				return false;

			onPath.push_back(decl);
			bool found = false;
			for (const ast::FieldDecl& field : decl->fields())
			{
				if (containsByValue(field.type, target, onPath, noCycleMemo))
				{
					found = true;
					break;
				}
			}
			onPath.pop_back();

			if (!found)
				noCycleMemo.push_back(decl);
			return found;
		}

		// Unwraps `[N]` (possibly nested, `T[N][M]`) down to the element type an array of it would
		// actually store - a field's real "is this storable/does it recurse" question is about
		// that element type, not the Array wrapper itself.
		const ast::Type* unwrapArrays(const ast::Type* type) noexcept
		{
			while (type && type->isArray())
				type = type->arrayElementType();
			return type;
		}

		// True when `decl`'s LAST field is a flexible array member - an unsized array (`int a[]`),
		// which the parser leaves as an Array of size 0. Its size does not count towards the struct's
		// own (type.cpp's layout already gives it zero bytes), and it is the only place such a field
		// may appear.
		bool hasFlexibleArrayMember(const ast::StructDecl* decl) noexcept
		{
			if (!decl || !decl->isComplete() || decl->fields().empty())
				return false;
			const ast::Type* last = decl->fields().back().type;
			return last && last->isArray() && last->arraySize() == 0;
		}

		// True when `type` is, or contains by value (through arrays and nested struct fields), a
		// struct with a flexible array member. A pointer never counts: pointing AT such a struct is
		// exactly what the member is for. `onPath` breaks a by-value cycle the same way
		// containsByValue() does, and `noCycleMemo` is the same negative-result memo - without it a
		// DAG-shaped hierarchy (`struct S1 { struct S0 a, b; }; struct S2 { struct S1 a, b; }; ...`)
		// re-explores each shared sub-struct once per sibling, exponential in nesting depth.
		bool containsFlexibleArrayMemberByValue(const ast::Type* type,
			std::vector<const ast::StructDecl*>& onPath, std::vector<const ast::StructDecl*>& noCycleMemo) noexcept
		{
			if (!type)
				return false;
			if (type->isArray())
				return containsFlexibleArrayMemberByValue(type->arrayElementType(), onPath, noCycleMemo);
			if (!type->isStruct())
				return false;

			const ast::StructDecl* decl = type->structDecl();
			if (!decl || !decl->isComplete())
				return false;
			if (hasFlexibleArrayMember(decl))
				return true;
			if (std::find(noCycleMemo.begin(), noCycleMemo.end(), decl) != noCycleMemo.end())
				return false;
			if (std::find(onPath.begin(), onPath.end(), decl) != onPath.end())
				return false;

			onPath.push_back(decl);
			bool found = false;
			for (const ast::FieldDecl& field : decl->fields())
			{
				if (containsFlexibleArrayMemberByValue(field.type, onPath, noCycleMemo))
				{
					found = true;
					break;
				}
			}
			onPath.pop_back();

			if (!found)
				noCycleMemo.push_back(decl);
			return found;
		}
	}

	u32 fieldOffset(const ast::StructDecl& decl, u32 fieldIndex) noexcept
	{
		if (decl.isUnion())
			return 0;
		u32 offset = 0;
		u32 index = 0;
		for (const ast::FieldDecl& field : decl.fields())
		{
			if (!field.type)
			{
				++index;
				continue;
			}
			offset = alignUp(offset, field.type->alignment());
			if (index == fieldIndex)
				return offset;
			offset += field.type->sizeInBytes();
			++index;
		}
		return offset;
	}

	bool validateStructLayout(support::DiagnosticEngine& diagnostics, const ast::StructDecl& decl) noexcept
	{
		if (!decl.isComplete())
			return true;

		std::vector<const ast::StructDecl*> onPath;
		std::vector<const ast::StructDecl*> noCycleMemo;
		std::vector<const ast::StructDecl*> famPath;
		std::vector<const ast::StructDecl*> famMemo;

		bool ok = true;
		std::span<const ast::FieldDecl> fields = decl.fields();
		for (usize index = 0; index < fields.size(); ++index)
		{
			const ast::FieldDecl& field = fields[index];
			if (!field.type || field.type->isVoid())
			{
				diagnostics.error(DiagId::FieldTypeVoid, field.location, "field '{}' declared with incomplete type 'void'", field.name);
				ok = false;
				continue;
			}

			if (field.type->isArray() && field.type->arraySize() == 0)
			{
				// A flexible array member is never legal in a union (C11 6.7.2.1p18): a union's
				// members all start at offset 0, so a tail array has nowhere to go.
				if (decl.isUnion())
				{
					diagnostics.error(DiagId::FlexibleArrayMemberInUnion, field.location,
						"flexible array member '{}' is not allowed in a union", field.name);
					ok = false;
					continue;
				}
				// It is also only ever the LAST member of a struct. The parser allows an unsized
				// field anywhere so this can say which one is wrong, rather than the parser
				// rejecting the syntax outright.
				if (index + 1 != fields.size())
				{
					diagnostics.error(DiagId::FlexibleArrayMemberNotLast, field.location,
						"flexible array member '{}' must be the last member of 'struct {}'", field.name, decl.name());
					ok = false;
					continue;
				}
			}

			const ast::Type* elementType = unwrapArrays(field.type);
			if (!elementType)
				continue;

			// A struct with a flexible array member has no complete size, so it cannot be embedded
			// by value or be an element of an array - only pointed at (C11 6.7.2.1p18).
			if (containsFlexibleArrayMemberByValue(field.type, famPath, famMemo))
			{
				diagnostics.error(DiagId::FlexibleArrayMemberInAggregate, field.location,
					"field '{}' embeds a struct with a flexible array member, which cannot be held by value", field.name);
				ok = false;
				continue;
			}

			if (elementType->isEnum())
			{
				ast::EnumDecl* nestedEnum = elementType->enumDecl();
				if (!nestedEnum || !nestedEnum->isComplete())
				{
					diagnostics.error(DiagId::FieldIncompleteEnum, field.location, "field '{}' has incomplete type 'enum {}'", field.name,
						nestedEnum ? nestedEnum->name() : std::string_view("<anonymous>"));
					ok = false;
				}
				continue;
			}

			if (!elementType->isStruct())
				continue;

			ast::StructDecl* nested = elementType->structDecl();
			if (!nested || !nested->isComplete())
			{
				diagnostics.error(DiagId::FieldIncompleteStruct, field.location, "field '{}' has incomplete type 'struct {}'", field.name,
					nested ? nested->name() : std::string_view("<anonymous>"));
				ok = false;
				continue;
			}

			if (containsByValue(field.type, &decl, onPath, noCycleMemo))
			{
				diagnostics.error(DiagId::FieldByValueCycle, field.location, "field '{}' creates an illegal by-value cycle in 'struct {}' (use a pointer instead)",
					field.name, decl.name());
				ok = false;
			}
		}
		return ok;
	}

	bool arrayElementHasFlexibleArrayMember(const ast::Type* type) noexcept
	{
		if (!type || !type->isArray())
			return false;
		std::vector<const ast::StructDecl*> onPath;
		std::vector<const ast::StructDecl*> noCycleMemo;
		return containsFlexibleArrayMemberByValue(type, onPath, noCycleMemo);
	}
}
