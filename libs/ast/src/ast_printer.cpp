#include <ceresc/ast/ast_printer.h>
#include <format>

namespace ceresc::ast
{
	namespace
	{
		constexpr std::string_view unaryOpSymbol(UnaryOp op) noexcept
		{
			switch (op)
			{
				case UnaryOp::AddressOf: return "&";
				case UnaryOp::Deref: return "*";
				case UnaryOp::Negate: return "-";
				case UnaryOp::LogicalNot: return "!";
				case UnaryOp::BitwiseNot: return "~";
				case UnaryOp::PreIncrement: return "++";
				case UnaryOp::PreDecrement: return "--";
				case UnaryOp::PostIncrement: return "post++";
				case UnaryOp::PostDecrement: return "post--";
			}
			return "<unknown-unary-op>";
		}

		constexpr std::string_view binaryOpSymbol(BinaryOp op) noexcept
		{
			switch (op)
			{
				case BinaryOp::LogicalOr: return "||";
				case BinaryOp::LogicalAnd: return "&&";
				case BinaryOp::BitOr: return "|";
				case BinaryOp::BitXor: return "^";
				case BinaryOp::BitAnd: return "&";
				case BinaryOp::Eq: return "==";
				case BinaryOp::Ne: return "!=";
				case BinaryOp::Lt: return "<";
				case BinaryOp::Le: return "<=";
				case BinaryOp::Gt: return ">";
				case BinaryOp::Ge: return ">=";
				case BinaryOp::Shl: return "<<";
				case BinaryOp::Shr: return ">>";
				case BinaryOp::Add: return "+";
				case BinaryOp::Sub: return "-";
				case BinaryOp::Mul: return "*";
				case BinaryOp::Div: return "/";
				case BinaryOp::Mod: return "%";
			}
			return "<unknown-binary-op>";
		}

		constexpr std::string_view assignOpSymbol(AssignOp op) noexcept
		{
			switch (op)
			{
				case AssignOp::Assign: return "=";
				case AssignOp::AddAssign: return "+=";
				case AssignOp::SubAssign: return "-=";
				case AssignOp::MulAssign: return "*=";
				case AssignOp::DivAssign: return "/=";
				case AssignOp::ModAssign: return "%=";
				case AssignOp::AndAssign: return "&=";
				case AssignOp::OrAssign: return "|=";
				case AssignOp::XorAssign: return "^=";
				case AssignOp::ShlAssign: return "<<=";
				case AssignOp::ShrAssign: return ">>=";
			}
			return "<unknown-assign-op>";
		}
	}

	std::string AstPrinter::print(Expr& root)
	{
		_output.clear();
		root.accept(*this);
		return _output;
	}

	std::string AstPrinter::print(Expr* root)
	{
		_output.clear();
		printChild(root);
		return _output;
	}

	void AstPrinter::printChild(Expr* child)
	{
		if (!child)
		{
			_output += "<null>";
			return;
		}
		child->accept(*this);
	}

	void AstPrinter::appendTypeName(const Type* type)
	{
		_output += typeName(type);
	}

	std::string AstPrinter::typeName(const Type* type)
	{
		if (!type)
			return "<null-type>";

		std::string prefix = type->isConst() ? "const " : "";
		switch (type->kind())
		{
			case TypeKind::Void: return prefix + "void";
			case TypeKind::Bool: return prefix + "bool";
			case TypeKind::Char: return prefix + "char";
			case TypeKind::UChar: return prefix + "unsigned char";
			case TypeKind::SChar: return prefix + "signed char";
			case TypeKind::Short: return prefix + "short";
			case TypeKind::UShort: return prefix + "unsigned short";
			case TypeKind::Int: return prefix + "int";
			case TypeKind::UInt: return prefix + "unsigned int";
			case TypeKind::Long: return prefix + "long";
			case TypeKind::ULong: return prefix + "unsigned long";
			case TypeKind::Float: return prefix + "float";
			case TypeKind::Double: return prefix + "double";
			case TypeKind::Pointer: return prefix + typeName(type->arrayElementType()) + "*";
			case TypeKind::Array: return prefix + typeName(type->arrayElementType()) + "[" + std::to_string(type->arraySize()) + "]";
			case TypeKind::Struct: return prefix + "struct";
			case TypeKind::Enum: return prefix + "enum";
		}
		return prefix + "<unknown-type>";
	}

	void AstPrinter::visit(IntLiteralExpr& node)
	{
		_output += std::to_string(node.value());
	}

	void AstPrinter::visit(FloatLiteralExpr& node)
	{
		_output += std::format("{}", node.value());
	}

	void AstPrinter::visit(CharLiteralExpr& node)
	{
		char c = node.value();
		_output += '\'';
		switch (c)
		{
			case '\n': _output += "\\n"; break;
			case '\t': _output += "\\t"; break;
			case '\0': _output += "\\0"; break;
			case '\\': _output += "\\\\"; break;
			case '\'': _output += "\\'"; break;
			default: _output += c; break;
		}
		_output += '\'';
	}

	void AstPrinter::visit(BoolLiteralExpr& node)
	{
		_output += node.value() ? "true" : "false";
	}

	void AstPrinter::visit(StringLiteralExpr& node)
	{
		_output += '"';
		_output += node.value().view();
		_output += '"';
	}

	void AstPrinter::visit(NameExpr& node)
	{
		_output += node.name();
	}

	void AstPrinter::visit(CallExpr& node)
	{
		_output += "(call ";
		printChild(node.callee());
		for (Expr* arg : node.args())
		{
			_output += ' ';
			printChild(arg);
		}
		_output += ')';
	}

	void AstPrinter::visit(UnaryExpr& node)
	{
		_output += '(';
		_output += unaryOpSymbol(node.op());
		_output += ' ';
		printChild(node.operand());
		_output += ')';
	}

	void AstPrinter::visit(BinaryExpr& node)
	{
		_output += '(';
		_output += binaryOpSymbol(node.op());
		_output += ' ';
		printChild(node.lhs());
		_output += ' ';
		printChild(node.rhs());
		_output += ')';
	}

	void AstPrinter::visit(AssignExpr& node)
	{
		_output += '(';
		_output += assignOpSymbol(node.op());
		_output += ' ';
		printChild(node.target());
		_output += ' ';
		printChild(node.value());
		_output += ')';
	}

	void AstPrinter::visit(IndexExpr& node)
	{
		_output += "(index ";
		printChild(node.array());
		_output += ' ';
		printChild(node.index());
		_output += ')';
	}

	void AstPrinter::visit(MemberExpr& node)
	{
		_output += '(';
		_output += node.isArrow() ? "->" : ".";
		_output += ' ';
		printChild(node.object());
		_output += ' ';
		_output += node.memberName();
		_output += ')';
	}

	void AstPrinter::visit(CastExpr& node)
	{
		_output += "(cast ";
		appendTypeName(node.targetType());
		_output += ' ';
		printChild(node.operand());
		_output += ')';
	}

	void AstPrinter::visit(SizeofExpr& node)
	{
		_output += "(sizeof ";
		if (node.isTypeArgument())
			appendTypeName(node.argumentType());
		else
			printChild(node.argumentExpr());
		_output += ')';
	}
}
