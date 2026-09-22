#pragma once

#include "types.h"
#include <optional>
#include <string>
#include <string_view>

// DiagnosticId - the stable identity of a KIND of diagnostic, independent of the wording of any
// one message.
//
// Every error and warning this compiler can emit is in this list exactly once, and every call site
// names one. Two sites that mean the same thing share an id even when they word it differently -
// the five places sema says "used type '{}' where arithmetic or pointer type is required" are one
// ConditionNotScalar, and the six places the parser runs out of arena are one OutOfMemory - because
// what the id classifies is the SITUATION, not the sentence.
//
// ---- the code -------------------------------------------------------------------------------
//
// A diagnostic prints its id as a four-digit code with a severity letter: `error[E3023]`,
// `warning[W2001]`. The first digit is the phase that found it, which is what makes a code
// readable without a table:
//
//   0xxx  lexer          characters -> tokens
//   1xxx  preprocessor   #include/#define and the directives
//   2xxx  parser         tokens -> tree
//   3xxx  sema           types, symbols, layout
//   4xxx  codegen        IR -> CASM text
//   5xxx  ir             tree -> IR
//
// Errors and warnings are numbered independently inside a phase, so E3001 and W3001 both exist and
// mean different things. The enum value carries the severity in its top bit to keep the two apart
// while leaving the printed number exactly what the ranges above say it is - see kWarningBit.
//
// A code, once handed out, is never reused for something else: it is what a program's
// `#pragma warning(disable: ...)` names (docs/11-Diagnostics.md), and what anything searching for
// an explanation of a message would search for.

namespace ceresc::support
{
	// The top bit of a DiagnosticId's value says "warning", so the remaining fifteen can be the
	// four-digit code itself. Without it, E3001 and W3001 would have to be the same enumerator.
	inline constexpr u16 kDiagnosticWarningBit = 0x8000;

	enum class DiagnosticId : u16
	{
		None = 0,

		// ---- 0xxx  lexer ----------------------------------------------------------------------
		UnterminatedBlockComment      = 1,
		IntegerLiteralTooLarge        = 2,
		FloatLiteralOutOfRange        = 3,
		HexLiteralHasNoDigits         = 4,
		BinaryLiteralHasNoDigits      = 5,
		UnterminatedEscapeSequence    = 6,
		HexEscapeHasNoDigits          = 7,
		UnknownEscapeSequence         = 8,
		EmptyCharacterLiteral         = 9,
		UnterminatedCharacterLiteral  = 10,
		MultiCharacterLiteral         = 11,
		UnterminatedStringLiteral     = 12,
		StringPoolExhausted           = 13,
		UnexpectedCharacter           = 14,

		// ---- 1xxx  preprocessor ---------------------------------------------------------------
		IfExpressionSyntax              = 1001,
		MalformedMacroInvocation        = 1002,
		MacroArgumentCount              = 1003,
		IncludeCycle                    = 1004,
		IncludeTooDeep                  = 1005,
		CannotOpenFile                  = 1006,
		ConditionalExpectsMacroName     = 1007,
		ElifWithoutIf                   = 1008,
		ElseWithoutIf                   = 1009,
		EndifWithoutIf                  = 1010,
		IncludeExpectsTarget            = 1011,
		IncludeNotFound                 = 1012,
		DefineExpectsName               = 1013,
		InvalidMacroParameterList       = 1014,
		UnterminatedMacroParameterList  = 1015,
		UndefExpectsName                = 1016,
		UserError                       = 1017, // whatever `#error` was given, verbatim
		UnsupportedDirective            = 1018,
		UnterminatedConditional         = 1019,

		MacroExpansionByteLimit = kDiagnosticWarningBit | 1001,
		MacroExpansionPassLimit = kDiagnosticWarningBit | 1002,
		UserWarning             = kDiagnosticWarningBit | 1003, // whatever `#warning` was given
		UnknownPragma           = kDiagnosticWarningBit | 1004,
		InvalidWarningPragma    = kDiagnosticWarningBit | 1005,
		UncontrollableDiagnostic= kDiagnosticWarningBit | 1006,

		// ---- 2xxx  parser ---------------------------------------------------------------------
		MachineBuiltinTakesNoArguments = 2001,
		ExpectedToken                  = 2002,
		OutOfMemory                    = 2003,
		ExpectedMemberName             = 2004,
		ExpectedExpression             = 2005,
		DuplicateQualifier             = 2006,
		ConflictingStorageClass        = 2007,
		StorageClassOnParameter        = 2008,
		StorageClassInTypeName         = 2009,
		NameIsNotAType                 = 2010,
		RestrictRequiresPointer        = 2011,
		ExpectedTypeName               = 2012,
		ExpectedTagName                = 2013,
		RedefinitionOfTag              = 2014,
		FieldCannotBeFunction          = 2015,
		ExpectedEnumTagName            = 2016,
		ExpectedEnumeratorName         = 2017,
		ExpectedLabelName              = 2018,
		ExpectedDeclaration            = 2019,
		ExpectedInterruptHandlerName   = 2020,
		EmptyInitializerList           = 2021,
		TrailingCommaInInitializerList = 2022,
		InterruptOnNonFunction         = 2023,
		InlineOnNonFunction            = 2024,
		AutoOnFunction                 = 2025,
		RegisterOnFunction             = 2026,
		InlineOnPrototype              = 2027,
		InterruptWithInline            = 2028,
		EllipsisNeedsNamedParameter    = 2029,
		EllipsisMustBeLast             = 2030,
		ExpectedArraySize              = 2031,
		InvalidArraySize               = 2032,
		ExpectedIdentifier             = 2033,
		FunctionReturnsFunction        = 2034,
		FunctionReturnsArray           = 2035,
		InvalidArrayElementType        = 2036,
		ArraySizeRequired              = 2037,
		ExpectedStaticAssertMessage    = 2038,
		ExpectedAsmLabel               = 2039,
		ExpectedDesignator             = 2040,
		ExpectedAttributeName          = 2041,
		InvalidAttributeArgument       = 2042,
		PackedNotSupported             = 2043,
		ExtendedAsmNotSupported        = 2044,
		AsmOutsideFunction             = 2045,
		ExpectedGenericAssociation     = 2046,

		CappedTypeWidth = kDiagnosticWarningBit | 2001,
		AttributeIgnored = kDiagnosticWarningBit | 2002,

		// ---- 3xxx  sema -----------------------------------------------------------------------
		Redefinition                     = 3001,
		RedefinitionOfLabel              = 3002,
		RedefinitionAsDifferentKind      = 3003,
		RedefinitionOfFunction           = 3004,
		ConflictingTypes                 = 3005,
		ConflictingLinkage               = 3006,
		VoidVariable                     = 3007,
		UnnamedParameterInDefinition     = 3008,
		EnumeratorNotConstant            = 3009,
		StringInitializerNeedsCharArray  = 3010,
		StringInitializerTooLong         = 3011,
		ArrayNeedsListInitializer        = 3012,
		IncompatibleInitializer          = 3013,
		InitializerListSize              = 3014,
		MissingInnerBraces               = 3015,
		IncompleteTypeInitializer        = 3016,
		StaticInitializerNotConstant     = 3017,
		InitializerListOutsideVariable   = 3018,
		ExternInitializerInBlock         = 3019,
		AutoOutsideBlock                 = 3020,
		RegisterOutsideBlock             = 3021,
		RegisterOnArray                  = 3022,
		UndeclaredIdentifier             = 3023,
		UndeclaredLabel                  = 3024,
		ArgumentCount                    = 3025,
		IncompatibleArgument             = 3026,
		IntegerFloatArgument             = 3027,
		CalledObjectNotAFunction         = 3028,
		AddressOfNonLValue               = 3029,
		AddressOfRegisterVariable        = 3030,
		IndirectionRequiresPointer       = 3031,
		InvalidUnaryOperand              = 3032,
		NotAssignable                    = 3033,
		AssignToConst                    = 3034,
		IncrementInvalidType             = 3035,
		InvalidLogicalOperands           = 3036,
		IncompatibleComparison           = 3037,
		InvalidBinaryOperands            = 3038,
		InvalidShiftOperands             = 3039,
		CompoundAssignToStructType       = 3040,
		IncompatibleAssignment           = 3041,
		SubscriptOnNonPointer            = 3042,
		SubscriptNotInteger              = 3043,
		MemberBaseNotPointerToStruct     = 3044,
		MemberBaseNotStruct              = 3045,
		NoSuchMember                     = 3046,
		InvalidCast                      = 3047,
		ConditionNotScalar               = 3048,
		IncompatibleTernaryOperands      = 3049,
		ReturnValueFromVoid              = 3050,
		IncompatibleReturnType           = 3051,
		MissingReturnValue               = 3052,
		BreakOutsideLoopOrSwitch         = 3053,
		ContinueOutsideLoop              = 3054,
		SwitchConditionNotInteger        = 3055,
		CaseOutsideSwitch                = 3056,
		CaseNotConstant                  = 3057,
		DuplicateCaseValue               = 3058,
		DefaultOutsideSwitch             = 3059,
		MultipleDefaultLabels            = 3060,
		AggregateThroughEllipsis         = 3061,
		VoidThroughEllipsis              = 3062,
		VaListOperandType                = 3063,
		VaStartOutsideVariadic           = 3064,
		VaStartLastParameter             = 3065,
		VaArgType                        = 3066,
		InterruptHandlerAddressTaken     = 3067,
		InterruptHandlerCalled           = 3068,
		InterruptHandlerReturnType       = 3069,
		InterruptHandlerTakesNoParameters= 3070,
		MainCannotBeInterrupt            = 3071,
		InterruptNumberNotConstant       = 3072,
		InterruptVectorIsReset           = 3073,
		InterruptNumberOutOfRange        = 3074,
		InterruptVectorTargetNotAFunction= 3075,
		InterruptVectorTargetNotInterrupt= 3076,
		InterruptVectorAlreadyBound      = 3077,
		FieldTypeVoid                    = 3078,
		FieldIncompleteEnum              = 3079,
		FieldIncompleteStruct            = 3080,
		FieldByValueCycle                = 3081,
		SizeofIncompleteArray            = 3082,
		StaticAssertNotConstant          = 3083,
		StaticAssertFailed               = 3084,
		AsmLabelNotAtFileScope           = 3085,
		InvalidAsmLabel                  = 3086,
		ConflictingAsmLabel              = 3087,
		InvalidDesignator                = 3088,
		DesignatorIndexNotConstant       = 3089,
		DesignatorIndexOutOfRange        = 3090,
		UnionMemberDesignator            = 3091,
		InvalidCompoundLiteralType       = 3092,
		DuplicateGenericAssociation      = 3093,
		MultipleGenericDefaults          = 3094,
		NoMatchingGenericAssociation     = 3095,

		ConstWithoutInitializer = kDiagnosticWarningBit | 3001,
		NoReturnFunctionReturns = kDiagnosticWarningBit | 3002,

		// ---- 4xxx  codegen --------------------------------------------------------------------
		RegisterAddressEscaped         = 4001,
		MalformedIrCall                = 4002,
		GlobalInitializerNotConstant   = 4003,
		ReservedCasmWord               = 4004,
		MissingDeclarationForFunction  = 4005,
		AddressConstantInStaticInitializer = 4006,
		ReservedNamePrefix             = 4007,

		// ---- 5xxx  ir -------------------------------------------------------------------------
		CompoundAssignToStruct = 5001
	};

	constexpr bool isWarningId(DiagnosticId id) noexcept
	{
		return (static_cast<u16>(id) & kDiagnosticWarningBit) != 0;
	}

	// The four-digit number a code prints, without its severity letter - what a
	// `#pragma warning(disable: 2001)` writes.
	constexpr u16 diagnosticNumber(DiagnosticId id) noexcept
	{
		return static_cast<u16>(static_cast<u16>(id) & ~kDiagnosticWarningBit);
	}

	// "E3023" / "W2001", or empty for None - a diagnostic with no id prints no code rather than a
	// meaningless one.
	std::string diagnosticCode(DiagnosticId id);

	// The warning a `#pragma warning(...)` number names, if any. Empty for a number that belongs to
	// an error (which cannot be turned off - a program that does not compile does not compile) or
	// to nothing at all.
	std::optional<DiagnosticId> warningWithNumber(u16 number) noexcept;
}
