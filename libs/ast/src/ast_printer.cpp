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

	std::string AstPrinter::print(Stmt& root)
	{
		_output.clear();
		root.accept(*this);
		return _output;
	}

	std::string AstPrinter::print(Decl& root)
	{
		_output.clear();
		root.accept(*this);
		return _output;
	}

	std::string AstPrinter::print(TranslationUnit& root)
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

	std::string AstPrinter::print(Stmt* root)
	{
		_output.clear();
		printChild(root);
		return _output;
	}

	std::string AstPrinter::print(Decl* root)
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

	void AstPrinter::printChild(Stmt* child)
	{
		if (!child)
		{
			_output += "<null>";
			return;
		}
		child->accept(*this);
	}

	void AstPrinter::printChild(Decl* child)
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
		if (type->isVolatile())
			prefix += "volatile ";
		if (type->isRestrict())
			prefix += "restrict ";
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
			case TypeKind::Pointer:
			{
				// A pointer's OWN qualifiers are written after the star, not before it: `int* const`
				// is a const pointer to ordinary int, while `const int*` is a pointer to const int,
				// and those are different types. Folding both into one leading prefix would print
				// the two identically - which is exactly how a qualifier landing on the wrong side
				// of the star stays invisible in `--emit-ast`.
				std::string suffix;
				if (type->isConst()) suffix += " const";
				if (type->isVolatile()) suffix += " volatile";
				if (type->isRestrict()) suffix += " restrict";

				// A pointer to a function is the one case where the star does not go at the end:
				// `int (*)(int)` and `int *(int)` are a pointer to a function and a function
				// returning a pointer, and only the parentheses tell them apart. That is C's
				// declarator syntax, which is why this is written as a declarator rather than as a
				// left-to-right name.
				const Type* pointee = type->arrayElementType();
				if (pointee && pointee->isFunction())
					return functionTypeName(pointee, "*" + suffix);

				return typeName(pointee) + "*" + suffix;
			}
			case TypeKind::Array: return prefix + typeName(type->arrayElementType()) + "[" + std::to_string(type->arraySize()) + "]";
			case TypeKind::Struct: return prefix + "struct " + std::string(type->structDecl() ? type->structDecl()->name() : std::string_view("<anonymous>"));
			case TypeKind::Union: return prefix + "union " + std::string(type->structDecl() ? type->structDecl()->name() : std::string_view("<anonymous>"));
			case TypeKind::Enum: return prefix + "enum " + std::string(type->enumDecl() ? type->enumDecl()->name() : std::string_view("<anonymous>"));
			case TypeKind::Function: return functionTypeName(type, {});
		}
		return prefix + "<unknown-type>";
	}

	// `RETURN (INNER)(PARAMS)` - C's declarator shape, where INNER is whatever wraps the function:
	// empty for the function type itself (`int (int)`), "*" for a pointer to one (`int (*)(int)`).
	// Written as one function because the parentheses around INNER are exactly what distinguishes
	// the two, and printing either without them produces a different type's name.
	std::string AstPrinter::functionTypeName(const Type* type, std::string_view inner)
	{
		const FunctionTypeInfo* info = type ? type->functionInfo() : nullptr;
		if (!info)
			return "<null-function-type>";

		std::string result = typeName(info->returnType);
		// The parentheses exist to bind `inner` tighter than the parameter list. With nothing to
		// bind, they would only be noise: a bare function type is `int (int)`, not `int ()(int)`.
		result += ' ';
		if (!inner.empty())
		{
			result += '(';
			result += inner;
			result += ')';
		}
		result += '(';
		bool first = true;
		for (const Type* param : info->params())
		{
			if (!first)
				result += ", ";
			first = false;
			result += typeName(param);
		}
		if (info->isVariadic)
			result += first ? "..." : ", ...";
		else if (first)
			result += "void";
		result += ')';
		return result;
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

	void AstPrinter::visit(AlignofExpr& node)
	{
		_output += "(alignof ";
		appendTypeName(node.argumentType());
		_output += ')';
	}

	void AstPrinter::visit(MachineOpExpr& node)
	{
		_output += '(';
		_output += machineOpName(node.op());
		_output += ')';
	}

	void AstPrinter::visit(VaExpr& node)
	{
		_output += '(';
		_output += vaOpName(node.op());
		_output += ' ';
		printChild(node.list());
		if (node.op() == VaOp::Arg)
		{
			_output += ' ';
			appendTypeName(node.argumentType());
		}
		else if (node.second())
		{
			_output += ' ';
			printChild(node.second());
		}
		_output += ')';
	}

	void AstPrinter::visit(TernaryExpr& node)
	{
		_output += "(?: ";
		printChild(node.cond());
		_output += ' ';
		printChild(node.thenExpr());
		_output += ' ';
		printChild(node.elseExpr());
		_output += ')';
	}

	void AstPrinter::visit(InitListExpr& node)
	{
		// `(init-list 1 2 3)` - deliberately the same flat shape as (call ...) above, so a nested
		// list prints as a nested one: `{ { 1, 2 }, { 3 } }` is `(init-list (init-list 1 2)
		// (init-list 3))`, which is exactly the nesting the braces have to match (see expr.h).
		_output += "(init-list";
		for (Expr* element : node.elements())
		{
			_output += ' ';
			printChild(element);
		}
		_output += ')';
	}

	// ---- statements ------------------------------------------------------------------------------

	void AstPrinter::visit(EmptyStmt&)
	{
		_output += "(empty)";
	}

	void AstPrinter::visit(ExprStmt& node)
	{
		_output += "(expr-stmt ";
		printChild(node.expr());
		_output += ')';
	}

	void AstPrinter::visit(DeclStmt& node)
	{
		_output += "(decl-stmt ";
		printChild(node.decl());
		_output += ')';
	}

	void AstPrinter::visit(CompoundStmt& node)
	{
		_output += "(block";
		for (Stmt* stmt : node.stmts())
		{
			_output += ' ';
			printChild(stmt);
		}
		_output += ')';
	}

	void AstPrinter::visit(IfStmt& node)
	{
		_output += "(if ";
		printChild(node.cond());
		_output += ' ';
		printChild(node.thenStmt());
		if (node.elseStmt())
		{
			_output += ' ';
			printChild(node.elseStmt());
		}
		_output += ')';
	}

	void AstPrinter::visit(WhileStmt& node)
	{
		_output += "(while ";
		printChild(node.cond());
		_output += ' ';
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(DoWhileStmt& node)
	{
		_output += "(do-while ";
		printChild(node.body());
		_output += ' ';
		printChild(node.cond());
		_output += ')';
	}

	void AstPrinter::visit(ForStmt& node)
	{
		_output += "(for ";
		printChild(node.init());
		_output += ' ';
		printChild(node.cond());
		_output += ' ';
		printChild(node.increment());
		_output += ' ';
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(ReturnStmt& node)
	{
		_output += "(return ";
		printChild(node.value());
		_output += ')';
	}

	void AstPrinter::visit(BreakStmt&)
	{
		_output += "(break)";
	}

	void AstPrinter::visit(ContinueStmt&)
	{
		_output += "(continue)";
	}

	void AstPrinter::visit(SwitchStmt& node)
	{
		_output += "(switch ";
		printChild(node.cond());
		_output += ' ';
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(CaseStmt& node)
	{
		_output += "(case ";
		printChild(node.value());
		_output += ' ';
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(DefaultStmt& node)
	{
		_output += "(default ";
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(GotoStmt& node)
	{
		_output += "(goto ";
		_output += node.label();
		_output += ')';
	}

	void AstPrinter::visit(LabelStmt& node)
	{
		_output += "(label ";
		_output += node.label();
		_output += ' ';
		printChild(node.body());
		_output += ')';
	}

	// ---- declarations -----------------------------------------------------------------------------

	void AstPrinter::visit(VarDecl& node)
	{
		_output += "(var ";
		_output += node.name();
		_output += ' ';
		appendTypeName(node.type());
		_output += ' ';
		printChild(node.initializer());
		_output += ')';
	}

	void AstPrinter::visit(FunctionDecl& node)
	{
		_output += node.isInterruptHandler() ? "(interrupt-func " : "(func ";
		_output += node.name();
		_output += ' ';
		appendTypeName(node.returnType());
		_output += " (params";
		for (const Param& param : node.params())
		{
			_output += " (";
			appendTypeName(param.type);
			_output += ' ';
			_output += param.name;
			_output += ')';
		}
		if (node.isVariadic())
			_output += " ...";
		_output += ") ";
		printChild(node.body());
		_output += ')';
	}

	void AstPrinter::visit(StructDecl& node)
	{
		_output += node.isUnion() ? "(union " : "(struct ";
		_output += node.name();
		if (!node.isComplete())
		{
			_output += " <incomplete>)";
			return;
		}
		_output += " (fields";
		for (const FieldDecl& field : node.fields())
		{
			_output += " (";
			appendTypeName(field.type);
			_output += ' ';
			_output += field.name;
			_output += ')';
		}
		_output += "))";
	}

	void AstPrinter::visit(EnumDecl& node)
	{
		_output += "(enum ";
		_output += node.name();
		if (!node.isComplete())
		{
			_output += " <incomplete>)";
			return;
		}
		_output += " (enumerators";
		for (const EnumeratorDecl& enumerator : node.enumerators())
		{
			_output += " (";
			_output += enumerator.name;
			if (enumerator.value)
			{
				_output += ' ';
				printChild(enumerator.value);
			}
			_output += ')';
		}
		_output += "))";
	}

	void AstPrinter::visit(TypedefDecl& node)
	{
		_output += "(typedef ";
		_output += node.name();
		_output += ' ';
		appendTypeName(node.underlyingType());
		_output += ')';
	}

	void AstPrinter::visit(InterruptVectorDecl& node)
	{
		_output += "(interrupt-vector ";
		printChild(node.number());
		_output += ' ';
		_output += node.name();
		_output += ')';
	}

	void AstPrinter::visit(TranslationUnit& node)
	{
		_output += "(unit";
		for (Decl* decl : node.decls())
		{
			_output += ' ';
			printChild(decl);
		}
		_output += ')';
	}
}
