//===-- Sema/SemaCheerp.cpp - Cheerp Sema utilities -------------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2019-2020 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/AST/Type.h"
#include "clang/Sema/SemaCheerp.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include <string>
#include <unordered_map>

bool cheerp::isInAnyNamespace(const clang::Decl* decl)
{
	auto context = decl->getDeclContext();
	while (context->isTransparentContext())
	{
		//This serves to skip things like: extern "C" that are a context but that are transparent for namespace rules
		context = context->getParent();
	}
	return (context->getParent() != NULL);
}

void cheerp::checkDestructor(clang::CXXRecordDecl* Record, clang::Sema& sema, bool& shouldContinue)
{
	using namespace clang;
	//TODO: expand this check
	if (Record->hasNonTrivialDestructor())
	{
		sema.Diag(Record->getLocation(), diag::err_cheerp_jsexport_with_non_trivial_destructor);
		shouldContinue = false;
	}
}

void cheerp::checkCouldBeJsExported(clang::CXXRecordDecl* Record, clang::Sema& sema, bool& shouldContinue)
{
	//class or struct
	using namespace clang;

	if (isInAnyNamespace(Record))
		sema.Diag(Record->getLocation(), diag::err_cheerp_jsexport_on_namespace);

	if (Record->isDynamicClass())
		sema.Diag(Record->getLocation(), diag::err_cheerp_attribute_on_virtual_class) << Record->getAttr<JsExportAttr>();

	if (Record->getNumBases())
		sema.Diag(Record->getLocation(), diag::err_cheerp_jsexport_inheritance);
	else
		assert(Record->getNumVBases() == 0);

	checkDestructor(Record, sema, shouldContinue);
}

void cheerp::checkParameters(clang::FunctionDecl* FD, clang::Sema& sema)
{
	for (auto it : FD->parameters())
	{
		if (it->hasDefaultArg())
			sema.Diag(it->getLocation(), clang::diag::err_cheerp_jsexport_with_default_arg) << FD->getParent();

		checkCouldBeParameterOfJsExported(it->getOriginalType(), &*it, sema);
	}
}

void cheerp::checkCouldBeParameterOfJsExported(const clang::QualType& Ty, clang::Decl* FD, clang::Sema& sema, const bool isParameter)
{
	using namespace cheerp;
	using namespace clang;

	const llvm::StringRef where = isParameter ? "parameter" : "return";

	//TODO: have to be checked again, may be possible to be more restrictive on some things while permitting others

	switch (classifyType(Ty.getTypePtr()))
	{
		case TypeKind::Function:
		case TypeKind::FunctionPointer:
		case TypeKind::IntMax32Bit:
		case TypeKind::FloatingPoint:
		{
			//Good!
			return;
		}
		case TypeKind::Pointer:
		case TypeKind::Reference:
		{
			break;
		}
		case TypeKind::Void:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "void" << where;
			return;
		}
		case TypeKind::JsExportable:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "naked JsExportable types" << where;
			return;
		}
		case TypeKind::NamespaceClient:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "naked Client types" << where;
			return;
		}
		case TypeKind::Other:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "unknown types" << where;
			return;
		}
		case TypeKind::IntGreater32Bit:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "greater than 32bit integers" << where;
			return;
		}
		default:
		{
			Ty->dump();
			llvm_unreachable("Should have been caught earlier");
		}
	}

	const clang::QualType& Ty2 = Ty.getTypePtr()->getPointeeType();

	if (!isParameter && Ty2.isConstQualified())
	{
		//TODO: is possible in practice to have const-jsexported elements, but for now keep it at no
		sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "const-qualified pointer or reference" << where;
	}

	switch (classifyType(Ty2.getTypePtr()))
	{
		case TypeKind::Void:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "void*" << where;
			return;
		}
		case TypeKind::IntMax32Bit:
		case TypeKind::FloatingPoint:
		case TypeKind::NamespaceClient:
		case TypeKind::JsExportable:
		{
			//Good!
			return;
		}
		case TypeKind::Other:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) <<
				"pointers to unknown (neither client namespace nor jsexportable) types" << where;
			return;
		}
		case TypeKind::IntGreater32Bit:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "pointers to integer bigger than 32 bits" << where;
			return;
		}
		case TypeKind::Pointer:
		case TypeKind::FunctionPointer:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "pointers to pointer" << where;
			return;
		}
		case TypeKind::Reference:
		{
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "pointers to references" << where;
			return;
		}
		default:
		{
			llvm_unreachable("Should have been caught earlier");
		}
	}
	llvm_unreachable("Should have been caught earlier");
}

void cheerp::checkCouldReturnBeJsExported(const clang::QualType& Ty, clang::FunctionDecl* FD, clang::Sema& sema)
{
	using namespace cheerp;
	using namespace clang;

	switch (classifyType(Ty.getTypePtr()))
	{
		case TypeKind::Void:
		{
			//No return is fine (while no parameter is not)
			return;
		}
		case TypeKind::Function:
		case TypeKind::FunctionPointer:
		{
			//Returning a function pointer could be OK in certain cases, but we have to check whether the function itself could be jsexported
			//In general the answer is no
			//TODO: possibly relax this check (or maybe not since it's actually complex, will require checking that every possible return value is jsExported
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_parameter_or_return) << "function pointers" << "return";
		}
		default:
			break;
	}

	//In all other cases, if something could be a parameter it could also work as return
	checkCouldBeParameterOfJsExported(Ty, FD, sema, /*isParameter*/false);
}

cheerp::TypeKind cheerp::classifyType(const clang::Type* Ty)
{
	using namespace cheerp;
	if (Ty->isReferenceType())
	{
		if (clang::cast<clang::ReferenceType>(Ty)->getPointeeType()->isFunctionType())
			return TypeKind::FunctionPointer;
		return TypeKind::Reference;
	}
	if (Ty->isIntegerType())
	{
		const clang::BuiltinType* builtin = (const clang::BuiltinType*) Ty;
		switch (builtin->getKind())
		{
			case clang::BuiltinType::LongLong:
			case clang::BuiltinType::ULongLong:
				return TypeKind::IntGreater32Bit;
			default:
				return TypeKind::IntMax32Bit;
		}
	}
	if (Ty->isRealFloatingType())
	{
		return TypeKind::FloatingPoint;
	}
	if (Ty->isFunctionType())
	{
		return TypeKind::Function;
	}
	if (Ty->isFunctionPointerType())
	{
		return TypeKind::FunctionPointer;
	}
	if (Ty->isPointerType())
	{
		return TypeKind::Pointer;
	}

	const clang::CXXRecordDecl* Record = Ty->getAsCXXRecordDecl();
	if (Record == NULL)
	{
		return TypeKind::Void;
	}
	if (clang::AnalysisDeclContext::isInClientNamespace(Record))
	{
		return TypeKind::NamespaceClient;
	}
	using namespace clang;
	if (Record->hasAttr<JsExportAttr>())
	{
		//The actual check about whether it is actually JsExportable is done somewhere else
		return TypeKind::JsExportable;
	}
	return TypeKind::Other;
}

void cheerp::checkFunction(clang::FunctionDecl* FD, clang::Sema& sema)
{
	sema.cheerpSemaData.addFunction(FD);
}

void cheerp::CheerpSemaData::addFunction(clang::FunctionDecl* FD)
{
	using namespace cheerp;
	using namespace clang;
	const bool isJsExport = FD->hasAttr<JsExportAttr>();

	if (isa<CXXMethodDecl>(FD))
	{
		addMethod((CXXMethodDecl*)FD, isJsExport);
	}
	else if (isJsExport)
	{
		checkFunctionToBeJsExported(FD, /*isMethod*/false, sema);
	}
}

bool cheerp::isTemplate(clang::FunctionDecl* FD)
{
	return FD->getTemplatedKind() != clang::FunctionDecl::TemplatedKind::TK_NonTemplate;
}

void cheerp::checkFunctionToBeJsExported(clang::FunctionDecl* FD, bool isMethod, clang::Sema& sema)
{
	using namespace cheerp;
	using namespace clang;

	if (isMethod)
	{
		//Method specific checks
		if (FD->hasAttr<AsmJSAttr>())
			sema.Diag(FD->getLocation(), diag::err_cheerp_incompatible_attributes) << FD->getAttr<AsmJSAttr>() << "method" << FD <<
					"[[cheerp::jsexport]]" << "method" << FD;

		if (FD->isOverloadedOperator())
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_with_operators);
	}
	else
	{
		//Free function specific checks
		if (isInAnyNamespace(FD))
			sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_namespace);
	}


	if (isTemplate(FD))
		sema.Diag(FD->getLocation(), diag::err_cheerp_jsexport_on_function_template);

	if (!isa<CXXConstructorDecl>(FD))
		checkCouldReturnBeJsExported(FD->getReturnType(), FD, sema);

	checkParameters(FD, sema);
}

void cheerp::CheerpSemaData::addMethod(clang::CXXMethodDecl* method, const bool isJsExport)
{
	using namespace clang;
	clang::CXXRecordDecl* record = method->getParent();

	if (isJsExport)
	{
		if (!record->hasAttr<JsExportAttr>())
		{
			sema.Diag(method->getLocation(), diag::err_cheerp_jsexport_on_method_of_not_jsexported_class) << record->getLocation();
			return;
		}
	}

	classData.emplace(record, CheerpSemaClassData(record, sema)).first->second.addMethod(method);
}

void cheerp::CheerpSemaData::checkRecord(clang::CXXRecordDecl* record)
{
	//Here all checks about external feasibility of jsexporting a class/struct have to be performed (eg. checking for name clashes against other functions)

	classData.emplace(record, CheerpSemaClassData(record, sema)).first->second.checkRecord();
}

template <class T>
void cheerp::CheerpSemaClassData::Interface::addToInterface(T* item, clang::Sema& sema)
{
	using namespace clang;
	const bool isJsExport = item->template hasAttr<JsExportAttr>();
	const bool isPublic = item->getAccess() == AS_public;

	if (isJsExport && !isPublic)
	{
		sema.Diag(item->getLocation(), diag::err_cheerp_jsexport_on_non_public_member);
		return;
	}

	if (isJsExport)
	{
		if (isPublicInterface)
			clear();
		isPublicInterface = false;
		insert(item);
	}
	else if (isPublic && isPublicInterface)
	{
		insert(item);
	}
};

void cheerp::CheerpSemaClassData::checkRecord()
{
	using namespace std;
	using namespace clang;

	assert(recordDecl->hasAttr<JsExportAttr>());

	bool shouldContinue = true;
	//Here all checks regarding internal feasibility of jsexporting a class/struct have to be performed
	checkCouldBeJsExported(recordDecl, sema, shouldContinue);

	if (!shouldContinue)
		return;

	//Add all the methods. This is needed since recordDecl->methods() would skip templated methods non instantiated, but we already collected them earlier
	for (auto method : recordDecl->methods())
	{
		addMethod(method);
	}

	//There are 2 possible interfaces: the implicit one, based on the public member, and the explicit one, based on the member tagged [[cheerp::jsexport]]
	Interface interface;

	for (auto method : declared_methods)
	{
		interface.addToInterface<CXXMethodDecl>(method, sema);
	}

	const bool isPublicInterface = interface.isPublicInterface;

	if (isPublicInterface)
	{
		for (auto decl : recordDecl->decls())
		{
			if (decl->isImplicit())
				continue;
			if (decl->getAccess() == AS_public && isa<RecordDecl>(decl))
				sema.Diag(decl->getLocation(), diag::err_cheerp_jsexport_inner_RecordDecl);
		}
	}

	int JsExportedConstructors = 0;
	std::unordered_map<std::string, const clang::CXXMethodDecl*> JsExportedMethodNames;
	std::unordered_map<std::string, const clang::CXXMethodDecl*> staticJsExportedMethodNames;

	staticJsExportedMethodNames.emplace("promise", nullptr);
	staticJsExportedMethodNames.emplace("valueOf", nullptr);
	bool isAnyNonStatic = false;

	for (auto method : interface.methods)
	{
		checkFunctionToBeJsExported(method, /*isMethod*/true, sema);

		if (isa<CXXConstructorDecl>(method))
		{
			isAnyNonStatic = true;
			++JsExportedConstructors;
			continue;
		}
		if (!method->isStatic())
		{
			isAnyNonStatic = true;
		}

		const auto& name = method->getNameInfo().getAsString();
		const auto pair = method->isStatic() ?
			staticJsExportedMethodNames.emplace(name, method) :
			JsExportedMethodNames.emplace(name, method);
		if (pair.second == false)
		{
			if (name == "promise" && method->isStatic())
				sema.Diag(method->getLocation(), diag::err_cheerp_jsexport_promise_static_method);
			if (name == "valueOf" && method->isStatic())
				sema.Diag(method->getLocation(), diag::err_cheerp_jsexport_valueOf_static_method);
			else
			{
				sema.Diag(method->getLocation(), diag::err_cheerp_jsexport_same_name_methods) << method;
				sema.Diag(pair.first->second->getLocation(), diag::note_previous_definition);
			}
		}
	}

	if (JsExportedConstructors != 1)
	{
		if (JsExportedConstructors == 0 && isPublicInterface)
			sema.Diag(recordDecl->getLocation(), diag::err_cheerp_jsexport_on_class_without_constructor);
		else if (JsExportedConstructors > 1)
			sema.Diag(recordDecl->getLocation(), diag::err_cheerp_jsexport_on_class_with_multiple_user_defined_constructor);
	}

	if (!isAnyNonStatic)
	{
		sema.Diag(recordDecl->getLocation(), diag::err_cheerp_jsexport_only_static);
	}

	for (auto method : interface.methods)
	{
		method->addAttr(JsExportAttr::CreateImplicit(sema.Context));
	}
}

void cheerp::CheerpSemaClassData::addMethod(clang::CXXMethodDecl* method)
{
	//Methods here are only added to the classes/struct, and only later they are checked
	//They have to be added here since this is the only moment that will capture templated methods (that possibly will never be instantiated)
	//But the actual checks have to be performed later when all methods are known
	declared_methods.insert(method);
}

bool cheerp::shouldBeJsExported(const clang::Decl *D, const bool isMethod)
{
	return D->hasAttr<clang::JsExportAttr>() && (clang::isa<clang::CXXMethodDecl>(D) == isMethod);
}

