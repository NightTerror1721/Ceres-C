#pragma once

#include "types.h"
#include <string_view>

// LiteralEncoding - the prefix a character or string literal was written with, and so the width of
// its code units and the type C gives it:
//
//   prefix  literal      char literal type          string element   units
//   (none)  'a' "a"      char                       char             bytes, the source's own
//   u8      u8'a' u8"a"  unsigned char (C23 char8_t) char (C17)       UTF-8 bytes
//   u       u'a' u"a"    unsigned short (char16_t)  unsigned short   UTF-16, surrogate pairs
//   U       U'a' U"a"    unsigned int (char32_t)    unsigned int     UTF-32
//   L       L'a' L"a"    int (wchar_t)              int              UTF-32
//
// A u8 STRING keeps C17's `char` elements rather than C23's `char8_t`, so `char s[] = u8"x"` and
// passing one to the string functions keep working; a u8 CHARACTER is C23's unsigned char.
//
// Living in libs/support because the lexer reads the prefix and the AST, sema and codegen all act
// on it, and the AST does not depend on the lexer.

namespace ceresc::support
{
	enum class LiteralEncoding : u8
	{
		Plain,
		Utf8,
		Utf16,
		Utf32,
		Wide,
	};

	// Bytes per code unit - one element of the literal's array.
	constexpr u32 codeUnitSize(LiteralEncoding encoding) noexcept
	{
		switch (encoding)
		{
			case LiteralEncoding::Utf16: return 2;
			case LiteralEncoding::Utf32:
			case LiteralEncoding::Wide: return 4;
			default: return 1;
		}
	}

	// The largest value one code unit holds, which bounds a `\x` or octal escape.
	constexpr u32 maxCodeUnit(LiteralEncoding encoding) noexcept
	{
		switch (codeUnitSize(encoding))
		{
			case 1: return 0xFFu;
			case 2: return 0xFFFFu;
			default: return 0xFFFFFFFFu;
		}
	}

	constexpr std::string_view literalPrefix(LiteralEncoding encoding) noexcept
	{
		switch (encoding)
		{
			case LiteralEncoding::Utf8: return "u8";
			case LiteralEncoding::Utf16: return "u";
			case LiteralEncoding::Utf32: return "U";
			case LiteralEncoding::Wide: return "L";
			default: return "";
		}
	}
}
