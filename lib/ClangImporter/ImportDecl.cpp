//===--- ImportDecl.cpp - Import Clang Declarations -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements support for importing Clang declarations into Swift.
//
//===----------------------------------------------------------------------===//

#include "ImporterImpl.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclVisitor.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace swift;

/// \brief Set the declaration context of each variable within the given
/// patterns to \p dc.
static void setVarDeclContexts(ArrayRef<Pattern *> patterns, DeclContext *dc) {
  for (auto pattern : patterns) {
    auto pat = pattern->getSemanticsProvidingPattern();
    if (auto named = dyn_cast<NamedPattern>(pat))
      named->getDecl()->setDeclContext(dc);
    if (auto tuple = dyn_cast<TuplePattern>(pat)) {
      for (auto elt : tuple->getFields())
        setVarDeclContexts(elt.getPattern(), dc);
    }
  }
}

/// \brief Map a well-known C type to a swift type from the standard library.
///
/// \param IsError set to true when we know the corresponding swift type name,
/// but we could not find it.  (For example, the type was not defined in the
/// standard library or the required standard library module was not imported.)
/// This should be a hard error, we don't want to map the type only sometimes.
///
/// \returns A pair of a swift type and its name that corresponds to a given
/// C type.
static std::pair<Type, StringRef>
getSwiftStdlibType(const clang::TypedefNameDecl *D,
                   Identifier Name,
                   ClangImporter::Implementation &Impl,
                   bool *IsError) {
  *IsError = false;

  MappedCTypeKind CTypeKind;
  unsigned Bitwidth;
  StringRef SwiftModuleName;
  bool IsSwiftModule; // True if SwiftModuleName == "swift".
  StringRef SwiftTypeName;
  MappedLanguages Languages;
  bool CanBeMissing;

  do {
#define MAP_TYPE(C_TYPE_NAME, C_TYPE_KIND, C_TYPE_BITWIDTH,     \
                 SWIFT_MODULE_NAME, SWIFT_TYPE_NAME, LANGUAGES, \
                 CAN_BE_MISSING)                                \
    if (Name.str() == C_TYPE_NAME) {                               \
      CTypeKind = MappedCTypeKind::C_TYPE_KIND;                    \
      Bitwidth = C_TYPE_BITWIDTH;                                  \
      if (StringRef("swift") == SWIFT_MODULE_NAME)                 \
        IsSwiftModule = true;                                      \
      else {                                                       \
        IsSwiftModule = false;                                     \
        SwiftModuleName = SWIFT_MODULE_NAME;                       \
      }                                                            \
      SwiftTypeName = SWIFT_TYPE_NAME;                             \
      Languages = MappedLanguages::LANGUAGES;                      \
      CanBeMissing = CAN_BE_MISSING;                               \
      break;                                                       \
    }
#include "MappedTypes.def"

    // We did not find this type, thus it is not mapped.
    return std::make_pair(Type(), "");
  } while(0);

  clang::ASTContext &ClangCtx = Impl.getClangASTContext();

  if (Languages != MappedLanguages::All) {
    if ((unsigned(Languages) & unsigned(MappedLanguages::ObjC1)) != 0 &&
        !ClangCtx.getLangOpts().ObjC1)
      return std::make_pair(Type(), "");
  }

  auto ClangType = D->getUnderlyingType();

  // If the C type does not have the expected size, don't import it as a stdlib
  // type.
  if (Bitwidth != 0 &&
      Bitwidth != ClangCtx.getTypeSize(ClangType))
    return std::make_pair(Type(), "");

  // Chceck other expected properties of the C type.
  switch(CTypeKind) {
  case MappedCTypeKind::UnsignedInt:
    if (!ClangType->isUnsignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::SignedInt:
    if (!ClangType->isSignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::FloatIEEEsingle:
  case MappedCTypeKind::FloatIEEEdouble:
  case MappedCTypeKind::FloatX87DoubleExtended: {
    if (!ClangType->isFloatingType())
      return std::make_pair(Type(), "");

    const llvm::fltSemantics &Sem = ClangCtx.getFloatTypeSemantics(ClangType);
    switch(CTypeKind) {
    case MappedCTypeKind::FloatIEEEsingle:
      assert(Bitwidth == 32 && "FloatIEEEsingle should be 32 bits wide");
      if (&Sem != &APFloat::IEEEsingle)
        return std::make_pair(Type(), "");
      break;

    case MappedCTypeKind::FloatIEEEdouble:
      assert(Bitwidth == 64 && "FloatIEEEsingle should be 64 bits wide");
      if (&Sem != &APFloat::IEEEdouble)
        return std::make_pair(Type(), "");
      break;

    case MappedCTypeKind::FloatX87DoubleExtended:
      assert(Bitwidth == 80 && "FloatIEEEsingle should be 80 bits wide");
      if (&Sem != &APFloat::x87DoubleExtended)
        return std::make_pair(Type(), "");
      break;

    default:
      llvm_unreachable("should see only floating point types here");
    }
    }
    break;

  case MappedCTypeKind::ObjCBool:
    if (!ClangCtx.hasSameType(ClangType, ClangCtx.ObjCBuiltinBoolTy))
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::ObjCSel:
    if (auto PT = ClangType->getAs<clang::PointerType>()) {
      if (!PT->getPointeeType()->isSpecificBuiltinType(
                                  clang::BuiltinType::ObjCSel))
        return std::make_pair(Type(), "");
    }
    break;
  }

  Module *M;
  if (IsSwiftModule)
    M = Impl.getSwiftModule();
  else
    M = Impl.getNamedModule(SwiftModuleName);
  if (!M) {
    // User did not import the library module that contains the type we want to
    // substitute.
    *IsError = true;
    return std::make_pair(Type(), "");
  }

  Type SwiftType = Impl.getNamedSwiftType(M, SwiftTypeName);
  if (!SwiftType && !CanBeMissing) {
    // The required type is not defined in the standard library.
    *IsError = true;
    return std::make_pair(Type(), "");
  }
  return std::make_pair(SwiftType, SwiftTypeName);
}

namespace {
  typedef ClangImporter::Implementation::EnumKind EnumKind;

  /// \brief Convert Clang declarations into the corresponding Swift
  /// declarations.
  class SwiftDeclConverter
    : public clang::ConstDeclVisitor<SwiftDeclConverter, Decl *>
  {
    ClangImporter::Implementation &Impl;
    bool forwardDeclaration = false;

  public:
    explicit SwiftDeclConverter(ClangImporter::Implementation &impl)
      : Impl(impl) { }

    bool hadForwardDeclaration() const {
      return forwardDeclaration;
    }

    Decl *VisitDecl(const clang::Decl *decl) {
      return nullptr;
    }

    Decl *VisitTranslationUnitDecl(const clang::TranslationUnitDecl *decl) {
      // Note: translation units are handled specially by importDeclContext.
      return nullptr;
    }

    Decl *VisitNamespaceDecl(const clang::NamespaceDecl *decl) {
      // FIXME: Implement once Swift has namespaces.
      return nullptr;
    }

    Decl *VisitUsingDirectiveDecl(const clang::UsingDirectiveDecl *decl) {
      // Never imported.
      return nullptr;
    }

    Decl *VisitNamespaceAliasDecl(const clang::NamespaceAliasDecl *decl) {
      // FIXME: Implement once Swift has namespaces.
      return nullptr;
    }

    Decl *VisitLabelDecl(const clang::LabelDecl *decl) {
      // Labels are function-local, and therefore never imported.
      return nullptr;
    }

    Decl *VisitTypedefNameDecl(const clang::TypedefNameDecl *Decl) {
      auto Name = Impl.importName(Decl->getDeclName());
      if (Name.empty())
        return nullptr;

      auto DC = Impl.importDeclContextOf(Decl);
      if (!DC)
        return nullptr;

      Type SwiftType;
      if (Decl->getDeclContext()->getRedeclContext()->isTranslationUnit()) {
        bool IsError;
        StringRef StdlibTypeName;
        std::tie(SwiftType, StdlibTypeName) =
            getSwiftStdlibType(Decl, Name, Impl, &IsError);

        if (IsError)
          return nullptr;

        if (SwiftType) {
          // Note that this typedef-name is special.
          Impl.SpecialTypedefNames.insert(Decl);

          if (Name.str() == StdlibTypeName) {
            // Don't create an extra typealias in the imported module because
            // doing so will cause ambiguity between the name in the imported
            // module and the same name in the 'swift' module.
            return SwiftType->castTo<StructType>()->getDecl();
          }
        }
      }

      if (!SwiftType)
        SwiftType = Impl.importType(Decl->getUnderlyingType(),
                                    ImportTypeKind::Normal);

      if (!SwiftType)
        return nullptr;

      auto Loc = Impl.importSourceLoc(Decl->getLocation());
      return new (Impl.SwiftContext) TypeAliasDecl(
                                      Impl.importSourceLoc(Decl->getLocStart()),
                                      Name,
                                      Loc,
                                      TypeLoc::withoutLoc(SwiftType),
                                      DC,
                                      { });
    }

    Decl *
    VisitUnresolvedUsingTypenameDecl(const clang::UnresolvedUsingTypenameDecl *decl) {
      // Note: only occurs in templates.
      return nullptr;
    }

    /// \brief Create a constructor that initializes a struct from its members.
    ConstructorDecl *createValueConstructor(StructDecl *structDecl,
                                            ArrayRef<Decl *> members) {
      auto &context = Impl.SwiftContext;

      // FIXME: Name hack.
      auto name = context.getIdentifier("init");

      // Create the 'self' declaration.
      auto selfType = structDecl->getDeclaredTypeInContext();
      auto selfMetaType = MetaTypeType::get(selfType, context);
      auto selfName = context.getIdentifier("self");
      auto selfDecl = new (context) VarDecl(SourceLoc(), selfName, selfType,
                                            structDecl);

      // Construct the set of parameters from the list of members.
      SmallVector<Pattern *, 4> paramPatterns;
      SmallVector<TuplePatternElt, 8> patternElts;
      SmallVector<TupleTypeElt, 8> tupleElts;
      SmallVector<VarDecl *, 8> params;
      for (auto member : members) {
        if (auto var = dyn_cast<VarDecl>(member)) {
          if (var->isComputed())
            continue;
          
          auto param = new (context) VarDecl(SourceLoc(), var->getName(),
                                             var->getType(), structDecl);
          params.push_back(param);
          Pattern *pattern = new (context) NamedPattern(param);
          pattern->setType(var->getType());
          auto tyLoc = TypeLoc::withoutLoc(var->getType());
          pattern = new (context) TypedPattern(pattern, tyLoc);
          pattern->setType(var->getType());
          paramPatterns.push_back(pattern);
          patternElts.push_back(TuplePatternElt(pattern));
          tupleElts.push_back(TupleTypeElt(var->getType(), var->getName()));
        }
      }
      auto paramPattern = TuplePattern::create(context, SourceLoc(), patternElts,
                                               SourceLoc());
      auto paramTy = TupleType::get(tupleElts, context);
      paramPattern->setType(paramTy);

      // Create the constructor
      auto constructor = new (context) ConstructorDecl(name, SourceLoc(),
                                                       paramPattern,
                                                       paramPattern,
                                                       selfDecl,
                                                       nullptr, structDecl);

      // Set the constructor's type.
      auto fnTy = FunctionType::get(paramTy, selfType, context);
      auto allocFnTy = FunctionType::get(selfMetaType, fnTy, context);
      auto initFnTy = FunctionType::get(selfType, fnTy, context);
      constructor->setType(allocFnTy);
      constructor->setInitializerType(initFnTy);

      // Fix the declaration contexts.
      selfDecl->setDeclContext(constructor);
      setVarDeclContexts(paramPatterns, constructor);

      // Assign all of the member variables appropriately.
      SmallVector<BraceStmt::ExprStmtOrDecl, 4> stmts;
      unsigned paramIdx = 0;
      for (auto member : members) {
        auto var = dyn_cast<VarDecl>(member);
        if (!var || var->isComputed())
          continue;

        // Construct left-hand side.
        Expr *lhs = new (context) DeclRefExpr(selfDecl, SourceLoc(),
                                              /*Implicit=*/true);
        lhs = new (context) MemberRefExpr(lhs, SourceLoc(), var, SourceLoc(),
                                          /*Implicit=*/true);

        // Construct right-hand side.
        auto param = params[paramIdx++];
        auto rhs = new (context) DeclRefExpr(param, SourceLoc(),
                                             /*Implicit=*/true);

        // Add assignment.
        stmts.push_back(new (context) AssignExpr(lhs, SourceLoc(), rhs,
                                                 /*Implicit=*/true));
      }

      // Create the function body.
      auto body = BraceStmt::create(context, SourceLoc(), stmts, SourceLoc());
      constructor->setBody(body);

      // Add this as an external definition.
      Impl.SwiftContext.addedExternalDecl(constructor);

      // We're done.
      return constructor;
    }

    Decl *VisitEnumDecl(const clang::EnumDecl *decl) {
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }
      
      Identifier name;
      if (decl->getDeclName())
        name = Impl.importName(decl->getDeclName());
      else if (decl->getTypedefNameForAnonDecl())
        name =Impl.importName(decl->getTypedefNameForAnonDecl()->getDeclName());

      if (name.empty())
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Create the enum declaration and record it.
      Decl *result;
      EnumDecl *enumDecl = nullptr;
      switch (Impl.classifyEnum(decl)) {
      case EnumKind::Constants: {
        // There is no declaration. Rather, the type is mapped to the
        // underlying type.
        return nullptr;
      }

      case EnumKind::Options: {
        auto structDecl = new (Impl.SwiftContext)
          StructDecl(SourceLoc(), name, SourceLoc(), { }, nullptr, dc);
        structDecl->computeType();

        // Compute the underlying type of the enumeration.
        auto underlyingType = Impl.importType(decl->getIntegerType(),
                                              ImportTypeKind::Normal);
        if (!underlyingType)
          return nullptr;

        // Create a variable to store the underlying value.
        auto varName = Impl.SwiftContext.getIdentifier("value");
        auto var = new (Impl.SwiftContext) VarDecl(SourceLoc(), varName,
                                                     underlyingType,
                                                     structDecl);

        // Create a pattern binding to describe the variable.
        Pattern * varPattern = new (Impl.SwiftContext) NamedPattern(var);
        varPattern->setType(var->getType());
        varPattern
          = new (Impl.SwiftContext) TypedPattern(
                                      varPattern,
                                      TypeLoc::withoutLoc(var->getType()));
        varPattern->setType(var->getType());
        
        auto patternBinding
          = new (Impl.SwiftContext) PatternBindingDecl(SourceLoc(),
                                                       varPattern,
                                                       nullptr, structDecl);

        // Create a constructor to initialize that value from a value of the
        // underlying type.
        Decl *varDecl = var;
        auto constructor = createValueConstructor(structDecl, {&varDecl, 1});

        // Set the members of the struct.
        Decl *members[3] = { constructor, patternBinding, var };
        structDecl->setMembers(
          Impl.SwiftContext.AllocateCopy(ArrayRef<Decl *>(members, 3)),
          SourceRange());

        result = structDecl;
        break;
      }

      case EnumKind::Enum:
        enumDecl = new (Impl.SwiftContext)
          EnumDecl(Impl.importSourceLoc(decl->getLocStart()),
                    name,
                    Impl.importSourceLoc(decl->getLocation()),
                    { }, nullptr, dc);
        result = enumDecl;
        break;
      }
      Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
      result->setClangNode(decl->getCanonicalDecl());

      // Import each of the enumerators.
      SmallVector<Decl *, 4> members;
      for (auto ec = decl->enumerator_begin(), ecEnd = decl->enumerator_end();
           ec != ecEnd; ++ec) {
        auto ood = Impl.importDecl(*ec);
        if (!ood)
          continue;

        members.push_back(ood);
      }

      // FIXME: Source range isn't totally accurate because Clang lacks the
      // location of the '{'.
      // FIXME: Eventually, we'd like to be able to do this for structs as well,
      // but we need static variables first.
      if (enumDecl) {
        enumDecl->setMembers(Impl.SwiftContext.AllocateCopy(members),
                                Impl.importSourceRange(clang::SourceRange(
                                                       decl->getLocation(),
                                                       decl->getRBraceLoc())));
      }
      
      return result;
    }

    Decl *VisitRecordDecl(const clang::RecordDecl *decl) {
      // FIXME: Skip unions for now. We can't properly map them to Swift unions,
      // because they aren't discriminated in any way. We could map them to
      // structs, but that would make them very, very unsafe to use.
      if (decl->isUnion())
        return nullptr;

      // FIXME: Skip Microsoft __interfaces.
      if (decl->isInterface())
        return nullptr;

      // The types of anonymous structs or unions are never imported; their
      // fields are dumped directly into the enclosing class.
      if (decl->isAnonymousStructOrUnion())
        return nullptr;

      // FIXME: Figure out how to deal with incomplete types, since that
      // notion doesn't exist in Swift.
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      Identifier name;
      if (decl->getDeclName())
        name = Impl.importName(decl->getDeclName());
      else if (decl->getTypedefNameForAnonDecl())
        name =Impl.importName(decl->getTypedefNameForAnonDecl()->getDeclName());

      if (name.empty())
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Create the struct declaration and record it.
      auto result = new (Impl.SwiftContext)
                      StructDecl(Impl.importSourceLoc(decl->getLocStart()),
                                 name,
                                 Impl.importSourceLoc(decl->getLocation()),
                                 { }, nullptr, dc);
      result->computeType();
      Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
      result->setClangNode(decl->getCanonicalDecl());

      // FIXME: Figure out what to do with superclasses in C++. One possible
      // solution would be to turn them into members and add conversion
      // functions.

      // Import each of the members.
      SmallVector<Decl *, 4> members;
      for (auto m = decl->decls_begin(), mEnd = decl->decls_end();
           m != mEnd; ++m) {
        auto nd = dyn_cast<clang::NamedDecl>(*m);
        if (!nd)
          continue;

        // Skip anonymous structs or unions; they'll be dealt with via the
        // IndirectFieldDecls.
        if (auto field = dyn_cast<clang::FieldDecl>(nd))
          if (field->isAnonymousStructOrUnion())
            continue;

        auto member = Impl.importDecl(nd);
        if (!member)
          continue;

        members.push_back(member);
      }

      // FIXME: Source range isn't totally accurate because Clang lacks the
      // location of the '{'.
      result->setMembers(Impl.SwiftContext.AllocateCopy(members),
                         Impl.importSourceRange(clang::SourceRange(
                                                  decl->getLocation(),
                                                  decl->getRBraceLoc())));
      
      // Add the struct decl to ExternalDefinitions so that IRGen can emit
      // metadata for it.
      // FIXME: There might be better ways to do this.
      Impl.SwiftContext.addedExternalDecl(result);
      
      return result;
    }

    Decl *VisitClassTemplateSpecializationDecl(
                 const clang::ClassTemplateSpecializationDecl *decl) {
      // FIXME: We could import specializations, but perhaps only as unnamed
      // structural types.
      return nullptr;
    }

    Decl *VisitClassTemplatePartialSpecializationDecl(
                 const clang::ClassTemplatePartialSpecializationDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitTemplateTypeParmDecl(const clang::TemplateTypeParmDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitEnumConstantDecl(const clang::EnumConstantDecl *decl) {
      auto &context = Impl.SwiftContext;
      
      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      auto clangEnum = cast<clang::EnumDecl>(decl->getDeclContext());
      switch (Impl.classifyEnum(clangEnum)) {
      case EnumKind::Constants: {
        // The enumeration was simply mapped to an integral type. Create a
        // constant with that integral type.

        // FIXME: These should be able to end up in a record, but Swift
        // can't represent that now.
        auto clangDC = clangEnum->getDeclContext();
        while (!clangDC->isFileContext())
          clangDC = clangDC->getParent();

        // The context where the constant will be introduced.
        auto dc = Impl.importDeclContext(clangDC);
        if (!dc)
          return nullptr;

        // Enumeration type.
        auto &clangContext = Impl.getClangASTContext();
        auto type = Impl.importType(clangContext.getTagDeclType(clangEnum),
                                    ImportTypeKind::Normal);
        if (!type)
          return nullptr;
        // FIXME: Importing the type will can recursively revisit this same
        // EnumConstantDecl. Short-circuit out if we already emitted the import
        // for this decl.
        auto known = Impl.ImportedDecls.find(decl->getCanonicalDecl());
        if (known != Impl.ImportedDecls.end())
          return known->second;

        // Create the global constant.
        auto result = Impl.createConstant(name, dc, type,
                                          clang::APValue(decl->getInitVal()),
                                          ConstantConvertKind::Coerce);
        Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
        return result;
      }
          
      case EnumKind::Options: {
        // The enumeration was mapped to a struct containining the integral
        // type. Create a constant with that struct type.

        // FIXME: These should be able to end up in a record, but Swift
        // can't represent that now.
        auto clangDC = clangEnum->getDeclContext();
        while (!clangDC->isFileContext())
          clangDC = clangDC->getParent();

        auto dc = Impl.importDeclContext(clangDC);
        if (!dc)
          return nullptr;

        // Import the enumeration type.
        auto enumType = Impl.importType(
                          Impl.getClangASTContext().getTagDeclType(clangEnum),
                          ImportTypeKind::Normal);
        if (!enumType)
          return nullptr;
        // FIXME: Importing the type will can recursively revisit this same
        // EnumConstantDecl. Short-circuit out if we already emitted the import
        // for this decl.
        auto known = Impl.ImportedDecls.find(decl->getCanonicalDecl());
        if (known != Impl.ImportedDecls.end())
          return known->second;

        // Create the global constant.
        auto result = Impl.createConstant(name, dc, enumType,
                                          clang::APValue(decl->getInitVal()),
                                          ConstantConvertKind::Construction);
        Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
        return result;
      }

      case EnumKind::Enum: {
        // The enumeration was mapped to a Swift enum. Create an element of
        // that enum.
        auto dc = Impl.importDeclContextOf(decl);
        if (!dc)
          return nullptr;

        // FIXME: Importing the type will can recursively revisit this same
        // EnumConstantDecl. Short-circuit out if we already emitted the import
        // for this decl.
        auto known = Impl.ImportedDecls.find(decl->getCanonicalDecl());
        if (known != Impl.ImportedDecls.end())
          return known->second;

        // FIXME: Import the raw type from the enum element decl.
        auto element
          = new (context) EnumElementDecl(SourceLoc(),
                                          name, TypeLoc(),
                                          SourceLoc(), TypeLoc(),
                                          SourceLoc(), nullptr,
                                          dc);

        // Give the enum element the appropriate type.
        auto theEnum = cast<EnumDecl>(dc);
        auto argTy = MetaTypeType::get(theEnum->getDeclaredType(), context);
        element->overwriteType(FunctionType::get(argTy,
                                                 theEnum->getDeclaredType(),
                                                 context));
        Impl.ImportedDecls[decl->getCanonicalDecl()] = element;
        return element;
      }
      }
    }


    Decl *
    VisitUnresolvedUsingValueDecl(const clang::UnresolvedUsingValueDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitIndirectFieldDecl(const clang::IndirectFieldDecl *decl) {
      // Check whether the context of any of the fields in the chain is a
      // union. If so, don't import this field.
      for (auto f = decl->chain_begin(), fEnd = decl->chain_end(); f != fEnd;
           ++f) {
        if (auto record = dyn_cast<clang::RecordDecl>((*f)->getDeclContext())) {
          if (record->isUnion())
            return nullptr;
        }
      }

      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      auto type = Impl.importType(decl->getType(), ImportTypeKind::Normal);
      if (!type)
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Map this indirect field to a Swift variable.
      return new (Impl.SwiftContext)
               VarDecl(Impl.importSourceLoc(decl->getLocStart()),
                       name, type, dc);
    }

    Decl *VisitFunctionDecl(const clang::FunctionDecl *decl) {
      decl = decl->getMostRecentDecl();
      if (!decl->hasPrototype()) {
        // We can't import a function without a prototype.
        return nullptr;
      }

      // FIXME: We can't IRgen inline functions, so don't import them.
      if (decl->isInlined() || decl->hasAttr<clang::AlwaysInlineAttr>()) {
        return nullptr;
      }

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Import the function type. If we have parameters, make sure their names
      // get into the resulting function type.
      SmallVector<Pattern *, 4> argPatterns;
      SmallVector<Pattern *, 4> bodyPatterns;
      Type type = Impl.importFunctionType(decl->getResultType(),
                                          { decl->param_begin(),
                                            decl->param_size() },
                                          decl->isVariadic(),
                                          argPatterns, bodyPatterns);
      if (!type)
        return nullptr;

      auto resultTy = type->castTo<FunctionType>()->getResult();
      auto loc = Impl.importSourceLoc(decl->getLocation());

      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      // FIXME: Poor location info.
      auto nameLoc = Impl.importSourceLoc(decl->getLocation());
      auto result = FuncDecl::create(
          Impl.SwiftContext, SourceLoc(), loc, name, nameLoc,
          /*GenericParams=*/nullptr, type, argPatterns, bodyPatterns,
          TypeLoc::withoutLoc(resultTy), dc);
      result->setBodyResultType(resultTy);
      setVarDeclContexts(argPatterns, result);
      setVarDeclContexts(bodyPatterns, result);
      return result;
    }

    Decl *VisitCXXMethodDecl(const clang::CXXMethodDecl *decl) {
      // FIXME: Import C++ member functions as methods.
      return nullptr;
    }

    Decl *VisitFieldDecl(const clang::FieldDecl *decl) {
      // Fields are imported as variables.
      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      auto type = Impl.importType(decl->getType(), ImportTypeKind::Normal);
      if (!type)
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      auto result = new (Impl.SwiftContext)
                      VarDecl(Impl.importSourceLoc(decl->getLocation()),
                              name, type, dc);

      // Handle attributes.
      if (decl->hasAttr<clang::IBOutletAttr>())
        result->getMutableAttrs().IBOutlet = true;
      // FIXME: Handle IBOutletCollection.

      return result;
    }

    Decl *VisitObjCIvarDecl(const clang::ObjCIvarDecl *decl) {
      // FIXME: Deal with fact that a property and an ivar can have the same
      // name.
      return VisitFieldDecl(decl);
    }

    Decl *VisitObjCAtDefsFieldDecl(const clang::ObjCAtDefsFieldDecl *decl) {
      // @defs is an anachronism; ignore it.
      return nullptr;
    }

    Decl *VisitVarDecl(const clang::VarDecl *decl) {
      // FIXME: Swift does not have static variables in structs/classes yet.
      if (decl->getDeclContext()->isRecord())
        return nullptr;

      // Variables are imported as... variables.
      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      auto type = Impl.importType(decl->getType(), ImportTypeKind::Normal);
      if (!type)
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      return new (Impl.SwiftContext)
               VarDecl(Impl.importSourceLoc(decl->getLocation()),
                       name, type, dc);
    }

    Decl *VisitImplicitParamDecl(const clang::ImplicitParamDecl *decl) {
      // Parameters are never directly imported.
      return nullptr;
    }

    Decl *VisitParmVarDecl(const clang::ParmVarDecl *decl) {
      // Parameters are never directly imported.
      return nullptr;
    }

    Decl *
    VisitNonTypeTemplateParmDecl(const clang::NonTypeTemplateParmDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitTemplateDecl(const clang::TemplateDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitUsingDecl(const clang::UsingDecl *decl) {
      // Using declarations are not imported.
      return nullptr;
    }

    Decl *VisitUsingShadowDecl(const clang::UsingShadowDecl *decl) {
      // Using shadow declarations are not imported; rather, name lookup just
      // looks through them.
      return nullptr;
    }

    Decl *VisitObjCMethodDecl(const clang::ObjCMethodDecl *decl) {
      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      return VisitObjCMethodDecl(decl, dc);
    }

    Decl *VisitObjCMethodDecl(const clang::ObjCMethodDecl *decl, DeclContext *dc) {
      auto loc = Impl.importSourceLoc(decl->getLocStart());

      // The name of the method is the first part of the selector.
      auto name
        = Impl.importName(decl->getSelector().getIdentifierInfoForSlot(0));
      if (name.empty())
        return nullptr;

      assert(dc->getDeclaredTypeOfContext() && "Method in non-type context?");

      // Add the implicit 'self' parameter patterns.
      SmallVector<Pattern *, 4> argPatterns;
      SmallVector<Pattern *, 4> bodyPatterns;
      auto selfTy = getSelfTypeForContext(dc);
      if (decl->isClassMethod())
        selfTy = MetaTypeType::get(selfTy, Impl.SwiftContext);
      auto selfName = Impl.SwiftContext.getIdentifier("self");
      auto selfVar = new (Impl.SwiftContext) VarDecl(SourceLoc(), selfName,
                                                     selfTy,
                                                     Impl.firstClangModule);
      Pattern *selfPat = new (Impl.SwiftContext) NamedPattern(selfVar);
      selfPat->setType(selfVar->getType());
      selfPat
        = new (Impl.SwiftContext) TypedPattern(selfPat,
                                               TypeLoc::withoutLoc(selfTy));
      selfPat->setType(selfVar->getType());
      argPatterns.push_back(selfPat);
      bodyPatterns.push_back(selfPat);
      
      // Import the type that this method will have.
      auto type = Impl.importFunctionType(decl->getResultType(),
                                          { decl->param_begin(),
                                            decl->param_size() },
                                          decl->isVariadic(),
                                          argPatterns,
                                          bodyPatterns,
                                          decl->getSelector());
      if (!type)
        return nullptr;

      auto resultTy = type->castTo<FunctionType>()->getResult();

      // Add the 'self' parameter to the function type.
      type = FunctionType::get(selfTy, type, Impl.SwiftContext);

      // FIXME: Related result type?

      // FIXME: Poor location info.
      auto nameLoc = Impl.importSourceLoc(decl->getLocation());
      auto result = FuncDecl::create(
          Impl.SwiftContext, SourceLoc(), loc, name, nameLoc,
          /*GenericParams=*/nullptr, type, argPatterns, bodyPatterns,
          TypeLoc::withoutLoc(resultTy), dc);
      result->setBodyResultType(resultTy);

      setVarDeclContexts(argPatterns, result);
      setVarDeclContexts(bodyPatterns, result);

      // Mark this as an Objective-C method.
      result->getMutableAttrs().ObjC = true;
      result->setIsObjC(true);

      // Mark class methods as static.
      if (decl->isClassMethod())
        result->setStatic();

      // If this method overrides another method, mark it as such.

      // FIXME: We'll eventually have to deal with having multiple overrides
      // in Swift.
      if (auto selfClassTy = selfTy->getAs<ClassType>()) {
        if (auto superTy = selfClassTy->getDecl()->getSuperclass()) {
          auto superDecl = superTy->castTo<ClassType>()->getDecl();
          if (auto superObjCClass = dyn_cast_or_null<clang::ObjCInterfaceDecl>(
                                      superDecl->getClangDecl())) {
            if (auto superObjCMethod = superObjCClass->lookupMethod(
                                         decl->getSelector(),
                                         decl->isInstanceMethod())) {
              // We found a method that we've overridden. Import it.
              FuncDecl *superMethod = nullptr;
              if (isa<clang::ObjCProtocolDecl>(
                    superObjCMethod->getDeclContext())) {
                superMethod = cast_or_null<FuncDecl>(
                                Impl.importMirroredDecl(superObjCMethod,
                                                        superDecl));
              } else {
                superMethod = cast_or_null<FuncDecl>(
                                Impl.importDecl(superObjCMethod));
              }
              
              if (superMethod) {
                // FIXME: Proper type checking here!
                result->setOverriddenDecl(superMethod);
              }
            }
          }
        }
      }

      // Handle attributes.
      if (decl->hasAttr<clang::IBActionAttr>())
        result->getMutableAttrs().IBAction = true;

      // Check whether there's some special method to import.
      result->setClangNode(decl->getCanonicalDecl());
      if (!Impl.ImportedDecls[decl->getCanonicalDecl()])
        Impl.ImportedDecls[decl->getCanonicalDecl()] = result;

      if (decl->getMethodFamily() != clang::OMF_init ||
          !isReallyInitMethod(decl)) {
        importSpecialMethod(result, dc);
      }
      return result;
    }

  private:
    /// Check whether the given name starts with the given word.
    static bool startsWithWord(StringRef name, StringRef word) {
      if (name.size() < word.size()) return false;
      return ((name.size() == word.size() || !islower(name[word.size()])) &&
              name.startswith(word));
    }

    /// Determine whether the given Objective-C method, which Clang classifies
    /// as an init method, is considered an init method in Swift.
    static bool isReallyInitMethod(const clang::ObjCMethodDecl *method) {
      if (!method->isInstanceMethod())
        return false;

      auto selector = method->getSelector();
      auto first = selector.getIdentifierInfoForSlot(0);
      if (!first) return false;

      return startsWithWord(first->getName(), "init");
    }

    /// \brief Given an imported method, try to import it as some kind of
    /// special declaration, e.g., a constructor or subscript.
    Decl *importSpecialMethod(Decl *decl, DeclContext *dc) {
      // Check whether there's a method associated with this declaration.
      auto objcMethod
        = dyn_cast_or_null<clang::ObjCMethodDecl>(decl->getClangDecl());
      if (!objcMethod)
        return nullptr;

      // Only consider Objective-C methods...
      switch (objcMethod->getMethodFamily()) {
      case clang::OMF_None:
        // Check for one of the subscripting selectors.
        if (objcMethod->isInstanceMethod() &&
            (objcMethod->getSelector() == Impl.objectAtIndexedSubscript ||
             objcMethod->getSelector() == Impl.setObjectAtIndexedSubscript ||
             objcMethod->getSelector() == Impl.objectForKeyedSubscript ||
             objcMethod->getSelector() == Impl.setObjectForKeyedSubscript))
          return importSubscript(decl, objcMethod, dc);
          
        return nullptr;

      case clang::OMF_init:
        // An init instance method can be a constructor.
        if (isReallyInitMethod(objcMethod))
          return importConstructor(decl, objcMethod, dc);
        return nullptr;

      case clang::OMF_new:
      case clang::OMF_alloc:
      case clang::OMF_autorelease:
      case clang::OMF_copy:
      case clang::OMF_dealloc:
      case clang::OMF_finalize:
      case clang::OMF_mutableCopy:
      case clang::OMF_performSelector:
      case clang::OMF_release:
      case clang::OMF_retain:
      case clang::OMF_retainCount:
      case clang::OMF_self:
        // None of these methods have special consideration.
        return nullptr;
      }
    }

    /// \brief Given an imported method, try to import it as a constructor.
    ///
    /// Objective-C methods in the 'init' family are imported as
    /// constructors in Swift, enabling the 'new' syntax, e.g.,
    ///
    /// \code
    /// new NSArray(1024) // in objc: [[NSArray alloc] initWithCapacity:1024]
    /// \endcode
    ConstructorDecl *importConstructor(Decl *decl,
                                       const clang::ObjCMethodDecl *objcMethod,
                                       DeclContext *dc) {
      // Figure out the type of the container.
      auto containerTy = dc->getDeclaredTypeOfContext();
      assert(containerTy && "Method in non-type context?");

      // Only methods in the 'init' family can become constructors.
      FuncDecl *alloc = nullptr;
      switch (objcMethod->getMethodFamily()) {
      case clang::OMF_alloc:
      case clang::OMF_autorelease:
      case clang::OMF_copy:
      case clang::OMF_dealloc:
      case clang::OMF_finalize:
      case clang::OMF_mutableCopy:
      case clang::OMF_None:
      case clang::OMF_performSelector:
      case clang::OMF_release:
      case clang::OMF_retain:
      case clang::OMF_retainCount:
      case clang::OMF_self:
      case clang::OMF_new:
        llvm_unreachable("Caller did not filter non-constructor methods");

      case clang::OMF_init: {
        assert(isReallyInitMethod(objcMethod) && "Caller didn't filter");

        // Make sure we have a usable 'alloc' method. Otherwise, we can't
        // build this constructor anyway.
        const clang::ObjCInterfaceDecl *interface;
        if (isa<clang::ObjCProtocolDecl>(objcMethod->getDeclContext())) {
          // For a protocol method, look into the context in which we'll be
          // mirroring the method to find 'alloc'.
          // FIXME: Part of the mirroring hack.
          auto classDecl = containerTy->getClassOrBoundGenericClass();
          if (!classDecl)
            return nullptr;

          interface = dyn_cast_or_null<clang::ObjCInterfaceDecl>(
                        classDecl->getClangDecl());
        } else {
          // For non-protocol methods, just look for the interface.
          interface = objcMethod->getClassInterface();
        }

        // If we couldn't find a class, we're done.
        if (!interface)
          return nullptr;

        // Form the Objective-C selector for alloc.
        auto &clangContext = Impl.getClangASTContext();
        auto allocId = &clangContext.Idents.get("alloc");
        auto allocSel = clangContext.Selectors.getNullarySelector(allocId);

        // Find the 'alloc' class method.
        auto allocMethod = interface->lookupClassMethod(allocSel);
        if (!allocMethod)
          return nullptr;

        // Import the 'alloc' class method.
        alloc = cast_or_null<FuncDecl>(Impl.importDecl(allocMethod));
        if (!alloc)
          return nullptr;
        break;
      }
      }

      // FIXME: Hack.
      auto loc = decl->getLoc();
      auto name = Impl.SwiftContext.getIdentifier("init");

      // Add the implicit 'self' parameter patterns.
      SmallVector<Pattern *, 4> argPatterns;
      SmallVector<Pattern *, 4> bodyPatterns;
      auto selfTy = getSelfTypeForContext(dc);
      auto selfMetaTy = MetaTypeType::get(selfTy, Impl.SwiftContext);
      auto selfName = Impl.SwiftContext.getIdentifier("self");
      auto selfMetaVar = new (Impl.SwiftContext) VarDecl(SourceLoc(), selfName,
                                                         selfMetaTy,
                                                         Impl.firstClangModule);
      Pattern *selfPat = new (Impl.SwiftContext) NamedPattern(selfMetaVar);
      selfPat->setType(selfMetaTy);
      selfPat
        = new (Impl.SwiftContext) TypedPattern(selfPat,
                                               TypeLoc::withoutLoc(selfMetaTy));
      selfPat->setType(selfMetaTy);

      argPatterns.push_back(selfPat);
      bodyPatterns.push_back(selfPat);

      // Import the type that this method will have.
      auto type = Impl.importFunctionType(objcMethod->getResultType(),
                                          { objcMethod->param_begin(),
                                            objcMethod->param_size() },
                                          objcMethod->isVariadic(),
                                          argPatterns,
                                          bodyPatterns,
                                          objcMethod->getSelector(),
                                          /*isConstructor=*/true);
      assert(type && "Type has already been successfully converted?");

      // A constructor returns an object of the type, not 'id'.
      // This is effectively implementing related-result-type semantics.
      // FIXME: Perhaps actually check whether the routine has a related result
      // type?
      type = FunctionType::get(type->castTo<FunctionType>()->getInput(),
                               selfTy, Impl.SwiftContext);

      // Add the 'self' parameter to the function types.
      Type allocType = FunctionType::get(selfMetaTy, type, Impl.SwiftContext);
      Type initType = FunctionType::get(selfTy, type, Impl.SwiftContext);

      VarDecl *selfVar = new (Impl.SwiftContext) VarDecl(SourceLoc(),
                                                          selfName, selfTy, dc);

      // Create the actual constructor.
      auto result = new (Impl.SwiftContext) ConstructorDecl(name, loc,
                                                            argPatterns.back(),
                                                            bodyPatterns.back(),
                                                            selfVar,
                                                            /*GenericParams=*/0,
                                                            dc);
      result->setType(allocType);
      result->setInitializerType(initType);
      result->setIsObjC(true);
      result->setClangNode(objcMethod);
      
      selfVar->setDeclContext(result);
      setVarDeclContexts(argPatterns, result);
      setVarDeclContexts(bodyPatterns, result);

      // Create the call to alloc that allocates 'self'.
      {
        // FIXME: Use the 'self' of metaclass type rather than a metatype
        // expression.
        Expr* initExpr = new (Impl.SwiftContext) MetatypeExpr(nullptr, loc,
                                                              selfMetaTy);

        // For an 'init' method, we need to call alloc first.
        Expr *allocRef = new (Impl.SwiftContext) DeclRefExpr(alloc, loc,
                                                             /*Implicit=*/true);

        auto allocCall = new (Impl.SwiftContext) DotSyntaxCallExpr(allocRef,
                                                                   loc,
                                                                   initExpr);
        auto emptyTuple
          = new (Impl.SwiftContext) TupleExpr(loc, {}, nullptr, loc,
                                              /*hasTrailingClosure=*/false,
                                              /*Implicit=*/true);
        initExpr = new (Impl.SwiftContext) CallExpr(allocCall, emptyTuple,
                                                    /*Implicit=*/true);

        // Cast the result of the alloc call to the (metatype) 'self'.
        // FIXME: instancetype should make this unnecessary.
        auto cast = new (Impl.SwiftContext) UnconditionalCheckedCastExpr(
                                             initExpr,
                                             SourceLoc(),
                                             SourceLoc(),
                                             TypeLoc::withoutLoc(selfTy));
        cast->setImplicit();
        cast->setCastKind(CheckedCastKind::Downcast);
        initExpr = cast;

        result->setAllocSelfExpr(initExpr);
      }
      
      // Inform the context that we have external definitions.
      Impl.SwiftContext.addedExternalDecl(result);

      return result;
    }

    /// \brief Retrieve the single variable described in the given pattern.
    ///
    /// This routine assumes that the pattern is something very simple
    /// like (x : type) or (x).
    VarDecl *getSingleVar(Pattern *pattern) {
      pattern = pattern->getSemanticsProvidingPattern();
      if (auto tuple = dyn_cast<TuplePattern>(pattern)) {
        pattern = tuple->getFields()[0].getPattern()
                    ->getSemanticsProvidingPattern();
      }

      return cast<NamedPattern>(pattern)->getDecl();
    }

    /// \brief Add the implicit 'self' pattern to the given list of patterns.
    ///
    /// \param selfTy The type of the 'self' parameter.
    ///
    /// \param args The set of arguments 
    VarDecl *addImplicitSelfParameter(Type selfTy,
                                      SmallVectorImpl<Pattern *> &args) {
      auto selfName = Impl.SwiftContext.getIdentifier("self");
      auto selfVar = new (Impl.SwiftContext) VarDecl(SourceLoc(), selfName,
                                                     selfTy,
                                                     Impl.firstClangModule);
      Pattern *selfPat = new (Impl.SwiftContext) NamedPattern(selfVar);
      selfPat->setType(selfVar->getType());
      selfPat = new (Impl.SwiftContext) TypedPattern(
                                          selfPat,
                                          TypeLoc::withoutLoc(selfTy));
      selfPat->setType(selfVar->getType());
      args.push_back(selfPat);

      return selfVar;
    }

    /// \brief Build a thunk for an Objective-C getter.
    ///
    /// \param getter The Objective-C getter method.
    ///
    /// \param dc The declaration context into which the thunk will be added.
    ///
    /// \param indices If non-null, the indices for a subscript getter. Null
    /// indicates that we're generating a getter thunk for a property getter.
    ///
    /// \returns The getter thunk.
    FuncDecl *buildGetterThunk(FuncDecl *getter, DeclContext *dc,
                               Pattern *indices) {
      auto &context = Impl.SwiftContext;
      auto loc = getter->getLoc();

      // Figure out the element type, by looking through 'self' and the normal
      // parameters.
      auto elementTy
        = getter->getType()->castTo<FunctionType>()->getResult()
            ->castTo<FunctionType>()->getResult();

      // Form the argument patterns.
      SmallVector<Pattern *, 3> getterArgs;

      // 'self'
      addImplicitSelfParameter(dc->getDeclaredTypeOfContext(), getterArgs);

      // index, for subscript operations.
      if (indices) {
        // Clone the indices for the thunk.
        indices = indices->clone(context);
        auto pat = TuplePattern::create(context, loc, TuplePatternElt(indices),
                                        loc);
        pat->setType(TupleType::get(TupleTypeElt(indices->getType(),
                                                 indices->getBoundName()),
                                    context));
        getterArgs.push_back(pat);
      }

      // empty tuple
      getterArgs.push_back(TuplePattern::create(context, loc, { }, loc));
      getterArgs.back()->setType(TupleType::getEmpty(context));

      // Form the type of the getter.
      auto getterType = elementTy;
      for (auto it = getterArgs.rbegin(), itEnd = getterArgs.rend();
           it != itEnd; ++it) {
        getterType = FunctionType::get((*it)->getType(),
                                       getterType,
                                       context);
      }

      // Create the getter thunk.
      auto thunk = FuncDecl::create(context, SourceLoc(), getter->getLoc(),
                                    Identifier(), SourceLoc(), nullptr,
                                    getterType, getterArgs, getterArgs,
                                    TypeLoc::withoutLoc(elementTy),
                                    getter->getDeclContext());
      thunk->setBodyResultType(elementTy);

      setVarDeclContexts(getterArgs, thunk);

      thunk->setIsObjC(true);
      return thunk;
    }

    /// \brief Build a thunk for an Objective-C setter.
    ///
    /// \param setter The Objective-C setter method.
    ///
    /// \param dc The declaration context into which the thunk will be added.
    ///
    /// \param indices If non-null, the indices for a subscript setter. Null
    /// indicates that we're generating a setter thunk for a property setter.
    ///
    /// \returns The getter thunk.
    FuncDecl *buildSetterThunk(FuncDecl *setter, DeclContext *dc,
                               Pattern *indices) {
      auto &context = Impl.SwiftContext;
      auto loc = setter->getLoc();
      auto tuple = cast<TuplePattern>(setter->getBodyParamPatterns()[1]);

      // Objective-C subscript setters are imported with a function type
      // such as:
      //
      //   (this) -> (value, index) -> ()
      //
      // while Swift subscript setters are curried as
      //
      //   (this) -> (index)(value) -> ()
      //
      // Build a setter thunk with the latter signature that maps to the
      // former.
      //
      // Property setters are similar, but don't have indices.

      // Form the argument patterns.
      SmallVector<Pattern *, 3> setterArgs;

      // 'self'
      addImplicitSelfParameter(dc->getDeclaredTypeOfContext(), setterArgs);

      // index, for subscript operations.
      if (indices) {
        // Clone the indices for the thunk.
        indices = indices->clone(context);
        auto pat = TuplePattern::create(context, loc, TuplePatternElt(indices),
                                        loc);
        pat->setType(TupleType::get(TupleTypeElt(indices->getType(),
                                                 indices->getBoundName()),
                                    context));
        setterArgs.push_back(pat);
      }

      // value
      auto valuePattern = tuple->getFields()[0].getPattern()->clone(context);
      setterArgs.push_back(TuplePattern::create(context, loc,
                                                TuplePatternElt(valuePattern),
                                                loc));
      setterArgs.back()->setType(
        TupleType::get(TupleTypeElt(valuePattern->getType(),
                                    valuePattern->getBoundName()),
                       context));

      // Form the type of the setter.
      auto setterType = TupleType::getEmpty(context);
      for (auto it = setterArgs.rbegin(), itEnd = setterArgs.rend();
           it != itEnd; ++it) {
        setterType = FunctionType::get((*it)->getType(),
                                       setterType,
                                       context);
      }

      // Create the setter thunk.
      auto thunk = FuncDecl::create(
          context, SourceLoc(), setter->getLoc(), Identifier(), SourceLoc(),
          nullptr, setterType, setterArgs, setterArgs,
          TypeLoc::withoutLoc(TupleType::getEmpty(context)), dc);
      thunk->setBodyResultType(TupleType::getEmpty(context));

      setVarDeclContexts(setterArgs, thunk);

      thunk->setIsObjC(true);
      return thunk;
    }
    
    /// \brief Given either the getter or setter for a subscript operation,
    /// create the Swift subscript declaration.
    SubscriptDecl *importSubscript(Decl *decl,
                                   const clang::ObjCMethodDecl *objcMethod,
                                   DeclContext *dc) {
      assert(objcMethod->isInstanceMethod() && "Caller must filter");

      // FIXME: Can we do this for protocol methods as well?
      auto interface = objcMethod->getClassInterface();
      if (!interface)
        return nullptr;

      FuncDecl *getter = nullptr, *setter = nullptr;
      if (objcMethod->getSelector() == Impl.objectAtIndexedSubscript) {
        getter = cast<FuncDecl>(decl);

        // Find the setter
        if (auto objcSetter = interface->lookupInstanceMethod(
                                Impl.setObjectAtIndexedSubscript)) {
          setter = cast_or_null<FuncDecl>(Impl.importDecl(objcSetter));

          // Don't allow static setters.
          if (setter && setter->isStatic())
            setter = nullptr;
        }
      } else if (objcMethod->getSelector() == Impl.setObjectAtIndexedSubscript){
        setter = cast<FuncDecl>(decl);

        // Find the getter.
        if (auto objcGetter = interface->lookupInstanceMethod(
                                Impl.objectAtIndexedSubscript)) {
          getter = cast_or_null<FuncDecl>(Impl.importDecl(objcGetter));

          // Don't allow static getters.
          if (getter && getter->isStatic())
            return nullptr;
        }

        // FIXME: Swift doesn't have write-only subscripting.
        if (!getter)
          return nullptr;
      } else if (objcMethod->getSelector() == Impl.objectForKeyedSubscript) {
        getter = cast<FuncDecl>(decl);

        // Find the setter
        if (auto objcSetter = interface->lookupInstanceMethod(
                                Impl.setObjectForKeyedSubscript)) {
          setter = cast_or_null<FuncDecl>(Impl.importDecl(objcSetter));

          // Don't allow static setters.
          if (setter && setter->isStatic())
            setter = nullptr;
        }
      } else if (objcMethod->getSelector() == Impl.setObjectForKeyedSubscript) {
        setter = cast<FuncDecl>(decl);

        // Find the getter.
        if (auto objcGetter = interface->lookupInstanceMethod(
                                Impl.objectForKeyedSubscript)) {
          getter = cast_or_null<FuncDecl>(Impl.importDecl(objcGetter));

          // Don't allow static getters.
          if (getter && getter->isStatic())
            return nullptr;
        }

        // FIXME: Swift doesn't have write-only subscripting.
        if (!getter)
          return nullptr;

      } else {
        llvm_unreachable("Unknown getter/setter selector");
      }

      // Check whether we've already created a subscript operation for
      // this getter/setter pair.
      if (auto subscript = Impl.Subscripts[{getter, setter}])
        return subscript;

      // Compute the element type, looking through the implicit 'self'
      // parameter and the normal function parameters.
      auto elementTy
        = getter->getType()->castTo<AnyFunctionType>()->getResult()
            ->castTo<AnyFunctionType>()->getResult();

      // Check the form of the getter.
      FuncDecl *getterThunk = nullptr;
      Pattern *getterIndices = nullptr;
      auto &context = Impl.SwiftContext;

      // Find the getter indices and make sure they match.
      {
        auto tuple =
            dyn_cast<TuplePattern>(getter -> getArgParamPatterns()[1]);
        if (tuple && tuple->getFields().size() != 1)
          return nullptr;

        getterIndices = tuple->getFields()[0].getPattern();
      }

      // Check the form of the setter.
      FuncDecl *setterThunk = nullptr;
      Pattern *setterIndices = nullptr;
      if (setter) {
        auto tuple = dyn_cast<TuplePattern>(setter->getBodyParamPatterns()[1]);
        if (!tuple)
          return nullptr;

        if (tuple->getFields().size() != 2)
          return nullptr;

        // The setter must accept elements of the same type as the getter
        // returns.
        // FIXME: Adjust C++ references?
        auto setterElementTy = tuple->getFields()[0].getPattern()->getType();
        if (!elementTy->isEqual(setterElementTy))
          return nullptr;

        setterIndices = tuple->getFields()[1].getPattern();

        // The setter must use the same indices as the getter.
        // FIXME: Adjust C++ references?
        // FIXME: Special case for NSDictionary, which uses 'id' for the getter
        // but 'id <NSCopying>' for the setter.
        if (!setterIndices->getType()->isEqual(getterIndices->getType()))
          return nullptr;
      }

      if (getter && getterIndices)
        getterThunk = buildGetterThunk(getter, dc, getterIndices);
      if (setter && setterIndices)
        setterThunk = buildSetterThunk(setter, dc, setterIndices);

      // Build the subscript declaration.
      auto argPatterns =
          getterThunk->getArgParamPatterns()[1]->clone(context);
      auto name = context.getIdentifier("__subscript");
      auto subscript
        = new (context) SubscriptDecl(name, decl->getLoc(), argPatterns,
                                      decl->getLoc(),
                                      TypeLoc::withoutLoc(elementTy),
                                      SourceRange(), getterThunk, setterThunk,
                                      dc);
      setVarDeclContexts(argPatterns, subscript->getDeclContext());

      subscript->setType(FunctionType::get(subscript->getIndices()->getType(),
                                           subscript->getElementType(),
                                           context));
      getterThunk->makeGetter(subscript);
      if (setterThunk)
        setterThunk->makeSetter(subscript);
      subscript->setIsObjC(true);

      // Determine whether this subscript operation overrides another subscript
      // operation.
      // FIXME: This ends up looking in the superclass for entirely bogus
      // reasons. Fix it.
      auto containerTy = dc->getDeclaredTypeInContext();
      SmallVector<ValueDecl *, 2> lookup;
      Impl.firstClangModule->lookupQualified(containerTy, name,
                                             NL_QualifiedDefault, nullptr,
                                             lookup);
      Type unlabeledIndices;
      for (auto result : lookup) {
        auto parentSub = dyn_cast<SubscriptDecl>(result);
        if (!parentSub)
          continue;

        // Compute the type of indices for our own subscript operation, lazily.
        if (!unlabeledIndices) {
          unlabeledIndices = subscript->getIndices()->getType()
                               ->getUnlabeledType(Impl.SwiftContext);
        }

        // Compute the type of indices for the subscript we found.
        auto parentUnlabeledIndices = parentSub->getIndices()->getType()
                                       ->getUnlabeledType(Impl.SwiftContext);
        if (!unlabeledIndices->isEqual(parentUnlabeledIndices))
          continue;

        // The index types match. This is an override, so mark it as such.
        subscript->setOverriddenDecl(parentSub);
        if (auto parentGetter = parentSub->getGetter()) {
          if (getterThunk)
              getterThunk->setOverriddenDecl(parentGetter);
        }
        if (auto parentSetter = parentSub->getSetter()) {
          if (setterThunk)
            setterThunk->setOverriddenDecl(parentSetter);
        }

        // FIXME: Eventually, deal with multiple overrides.
        break;
      }

      // Note that we've created this subscript.
      Impl.Subscripts[{getter, setter}] = subscript;
      Impl.Subscripts[{getterThunk, nullptr}] = subscript;
      return subscript;
    }

  public:
    /// \brief Retrieve the type of 'self' for the given context.
    Type getSelfTypeForContext(DeclContext *dc) {
      // For a protocol, the type is 'Self'.
      if (auto proto = dyn_cast<ProtocolDecl>(dc)) {
        return proto->getSelf()->getDeclaredType();
      }

      return dc->getDeclaredTypeOfContext();
    }

    // Import the given Objective-C protocol list and return a context-allocated
    // ArrayRef that can be passed to the declaration.
    MutableArrayRef<ProtocolDecl *>
    importObjCProtocols(Decl *decl,
                        const clang::ObjCProtocolList &clangProtocols) {
      SmallVector<ProtocolDecl *, 4> protocols;
      llvm::SmallPtrSet<ProtocolDecl *, 4> knownProtocols;
      if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
        nominal->getImplicitProtocols(protocols);
        knownProtocols.insert(protocols.begin(), protocols.end());
      }

      for (auto cp = clangProtocols.begin(), cpEnd = clangProtocols.end();
           cp != cpEnd; ++cp) {
        if (auto proto = cast_or_null<ProtocolDecl>(Impl.importDecl(*cp))) {
          if (knownProtocols.insert(proto))
            protocols.push_back(proto);
        }
      }

      // FIXME: We should be synthesizing protocol conformances as well.
      return Impl.SwiftContext.AllocateCopy(protocols);
    }

    /// Import members of the given Objective-C container and add them to the
    /// list of corresponding Swift members.
    void importObjCMembers(const clang::ObjCContainerDecl *decl,
                           DeclContext *swiftContext,
                           SmallVectorImpl<Decl *> &members) {
      llvm::SmallPtrSet<Decl *, 4> knownMembers;
      for (auto m = decl->decls_begin(), mEnd = decl->decls_end();
           m != mEnd; ++m) {
        auto nd = dyn_cast<clang::NamedDecl>(*m);
        if (!nd)
          continue;

        auto member = Impl.importDecl(nd);
        if (!member)
          continue;

        // If this member is a method that is a getter or setter for a property
        // that was imported, don't add it to the list of members so it won't
        // be found by name lookup. This eliminates the ambiguity between
        // property names and getter names (by choosing to only have a
        // variable).
        if (auto objcMethod = dyn_cast<clang::ObjCMethodDecl>(nd)) {
          if (auto property = objcMethod->findPropertyDecl())
            if (Impl.importDecl(
                  const_cast<clang::ObjCPropertyDecl *>(property)))
              continue;

          // If there is a special declaration associated with this member,
          // add it now.
          if (auto special = importSpecialMethod(member, swiftContext)) {
            if (knownMembers.insert(special))
              members.push_back(special);

            // If we imported a constructor, the underlying init method is not
            // visible.
            if (isa<ConstructorDecl>(special))
              continue;
          }
        }
        
        members.push_back(member);
      }
    }

    /// \brief Import the members of all of the protocols to which the given
    /// Objective-C class, category, or extension explicitly conforms into
    /// the given list of members, so long as the the method was not already
    /// declared in the class.
    ///
    /// FIXME: This whole thing is a hack, because name lookup should really
    /// just find these members when it looks in the protocol. Unfortunately,
    /// that's not something the name lookup code can handle right now.
    void importMirroredProtocolMembers(const clang::ObjCContainerDecl *decl,
                                       DeclContext *dc,
                                       ArrayRef<ProtocolDecl *> protocols,
                                       SmallVectorImpl<Decl *> &members) {
      for (auto proto : protocols) {
        for (auto member : proto->getMembers()) {
          if (auto func = dyn_cast<FuncDecl>(member)) {
            if (auto objcMethod = dyn_cast_or_null<clang::ObjCMethodDecl>(
                                    func->getClangDecl())) {
              if (!decl->getMethod(objcMethod->getSelector(),
                                   objcMethod->isInstanceMethod())) {
                if (auto imported = Impl.importMirroredDecl(objcMethod, dc)) {
                  members.push_back(imported);

                  // Import any special methods based on this member.
                  if (auto special = importSpecialMethod(imported, dc)) {
                    members.push_back(special);
                  }
                }
              }
            }
          }
        }
      }
    }

    /// \brief Determine whether the given Objective-C class has an instance or
    /// class method with the given selector directly declared (i.e., not in
    /// a superclass or protocol).
    static bool hasMethodShallow(const clang::Selector sel, bool isInstance,
                                 const clang::ObjCInterfaceDecl *objcClass) {
      if (objcClass->getMethod(sel, isInstance))
        return true;

      for (auto cat = objcClass->visible_categories_begin(),
                catEnd = objcClass->visible_categories_end();
           cat != catEnd;
           ++cat) {
        if ((*cat)->getMethod(sel, isInstance))
          return true;
      }

      return false;
    }

    /// \brief Import constructors from our superclasses (and their
    /// categories/extensions), effectively "inheriting" constructors.
    ///
    /// FIXME: Does it make sense to have inherited constructors as a real
    /// Swift feature?
    void importInheritedConstructors(const clang::ObjCInterfaceDecl *objcClass,
                                     DeclContext *dc,
                                     SmallVectorImpl<Decl *> &members) {
      // FIXME: Would like a more robust way to ensure that we aren't creating
      // duplicates.
      llvm::SmallSet<clang::Selector, 16> knownSelectors;
      auto inheritConstructors = [&](const clang::ObjCContainerDecl *container) {
        for (auto meth = container->meth_begin(),
                  methEnd = container->meth_end();
             meth != methEnd; ++meth) {
          if ((*meth)->getMethodFamily() == clang::OMF_init &&
              isReallyInitMethod(*meth) &&
              !hasMethodShallow((*meth)->getSelector(),
                                (*meth)->isInstanceMethod(),
                                objcClass) &&
              knownSelectors.insert((*meth)->getSelector())) {
                if (auto imported = Impl.importDecl(*meth)) {
                  if (auto special = importConstructor(imported, *meth, dc)) {
                    members.push_back(special);
                  }
                }
              }
        }
      };

      for (auto curObjCClass = objcClass; curObjCClass;
           curObjCClass = curObjCClass->getSuperClass()) {
        inheritConstructors(curObjCClass);
        for (auto cat = curObjCClass->visible_categories_begin(),
                  catEnd = curObjCClass->visible_categories_end();
             cat != catEnd;
             ++cat) {
            inheritConstructors(*cat);
        }
      }
    }

    Decl *VisitObjCCategoryDecl(const clang::ObjCCategoryDecl *decl) {
      // Objective-C categories and extensions map to Swift extensions.

      // Find the Swift class being extended.
      auto objcClass
        = cast_or_null<ClassDecl>(Impl.importDecl(decl->getClassInterface()));
      if (!objcClass)
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Create the extension declaration and record it.
      auto loc = Impl.importSourceLoc(decl->getLocStart());
      auto result
        = new (Impl.SwiftContext)
            ExtensionDecl(loc,
                          TypeLoc::withoutLoc(objcClass->getDeclaredType()),
                          { },
                          dc);
      objcClass->addExtension(result);
      Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
      result->setClangNode(decl->getCanonicalDecl());
      result->setProtocols(importObjCProtocols(result,
                                               decl->getReferencedProtocols()));
      result->setCheckedInheritanceClause();

      // Import each of the members.
      SmallVector<Decl *, 4> members;
      importObjCMembers(decl, result, members);

      // Import mirrored declarations for protocols to which this category
      // or extension conforms.
      // FIXME: This is a short-term hack.
      importMirroredProtocolMembers(decl, result, result->getProtocols(),
                                    members);

      // FIXME: Source range isn't accurate.
      result->setMembers(Impl.SwiftContext.AllocateCopy(members),
                         Impl.importSourceRange(clang::SourceRange(
                                                  decl->getLocation(),
                                                  decl->getLocEnd())));

      return result;
    }

    Decl *VisitObjCProtocolDecl(const clang::ObjCProtocolDecl *decl) {
      // FIXME: Figure out how to deal with incomplete protocols, since that
      // notion doesn't exist in Swift.
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      // Append "Proto" to protocol names.
      auto name = Impl.importName(decl->getDeclName(), "Proto");
      if (name.empty())
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Create the protocol declaration and record it.
      auto result = new (Impl.SwiftContext)
                      ProtocolDecl(dc,
                                   Impl.importSourceLoc(decl->getLocStart()),
                                   Impl.importSourceLoc(decl->getLocation()),
                                   name,
                                   { });
      result->computeType();
      Impl.ImportedDecls[decl->getCanonicalDecl()] = result;

      result->setClangNode(decl->getCanonicalDecl());
      result->setCircularityCheck(CircularityCheck::Checked);

      // Import protocols this protocol conforms to.
      result->setProtocols(importObjCProtocols(result,
                                               decl->getReferencedProtocols()));
      result->setCheckedInheritanceClause();

      // Note that this is an Objective-C and class protocol.
      result->getMutableAttrs().ObjC = true;
      result->getMutableAttrs().ClassProtocol = true;
      result->setIsObjC(true);

      // Add the implicit 'Self' associated type.
      auto selfId = Impl.SwiftContext.getIdentifier("Self");
      auto selfDecl = new (Impl.SwiftContext) AssociatedTypeDecl(result,
                                                                 SourceLoc(),
                                                                 selfId,
                                                                 SourceLoc());
      selfDecl->setImplicit();
      auto selfArchetype = ArchetypeType::getNew(Impl.SwiftContext, nullptr,
                                                 selfDecl, selfId,
                                                 Type(result->getDeclaredType()),
                                                 Type());
      selfDecl->setArchetype(selfArchetype);
      result->setMembers(Impl.SwiftContext.AllocateCopy(
                           llvm::makeArrayRef<Decl*>(selfDecl)),
                         SourceRange());
                         
      // Import each of the members.
      SmallVector<Decl *, 4> members;
      members.push_back(selfDecl);
      importObjCMembers(decl, result, members);

      // FIXME: Source range isn't accurate.
      result->setMembers(Impl.SwiftContext.AllocateCopy(members),
                         Impl.importSourceRange(clang::SourceRange(
                                                  decl->getLocation(),
                                                  decl->getLocEnd())));

      // Add the protocol decl to ExternalDefinitions so that IRGen can emit
      // metadata for it.
      // FIXME: There might be better ways to do this.
      Impl.SwiftContext.addedExternalDecl(result);

      return result;
    }

    Decl *VisitObjCInterfaceDecl(const clang::ObjCInterfaceDecl *decl) {
      // FIXME: Figure out how to deal with incomplete types, since that
      // notion doesn't exist in Swift.
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      // Create the class declaration and record it.
      auto result = new (Impl.SwiftContext)
                      ClassDecl(Impl.importSourceLoc(decl->getLocStart()),
                                name,
                                Impl.importSourceLoc(decl->getLocation()),
                                { }, nullptr, dc);
      result->computeType();
      Impl.ImportedDecls[decl->getCanonicalDecl()] = result;
      result->setClangNode(decl->getCanonicalDecl());
      result->setCircularityCheck(CircularityCheck::Checked);

      // If this Objective-C class has a supertype, import it.
      if (auto objcSuper = decl->getSuperClass()) {
        auto super = cast_or_null<ClassDecl>(Impl.importDecl(objcSuper));
        if (!super)
          return nullptr;

        result->setSuperclass(super->getDeclaredType());
      }

      // Import protocols this class conforms to.
      result->setProtocols(importObjCProtocols(result,
                                               decl->getReferencedProtocols()));
      result->setCheckedInheritanceClause();

      // Note that this is an Objective-C class.
      result->getMutableAttrs().ObjC = true;
      result->setIsObjC(true);
      
      // Import each of the members.
      SmallVector<Decl *, 4> members;
      importObjCMembers(decl, result, members);

      // Import inherited constructors.
      importInheritedConstructors(decl, result, members);

      // Import mirrored declarations for protocols to which this class
      // conforms.
      // FIXME: This is a short-term hack.
      importMirroredProtocolMembers(decl, result, result->getProtocols(),
                                    members);

      // FIXME: Source range isn't accurate.
      result->setMembers(Impl.SwiftContext.AllocateCopy(members),
                         Impl.importSourceRange(clang::SourceRange(
                                                  decl->getLocation(),
                                                  decl->getLocEnd())));

      // Pass the class to the type checker to create an implicit destructor.
      Impl.SwiftContext.addedExternalDecl(result);

      return result;
    }

    Decl *VisitObjCImplDecl(const clang::ObjCImplDecl *decl) {
      // Implementations of Objective-C classes and categories are not
      // reflected into Swift.
      return nullptr;
    }

    Decl *VisitObjCPropertyDecl(const clang::ObjCPropertyDecl *decl) {
      // Properties are imported as variables.

      // FIXME: For now, don't import properties in protocols, because IRGen
      // can't handle the thunks we generate.
      if (isa<clang::ObjCProtocolDecl>(decl->getDeclContext()))
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl);
      if (!dc)
        return nullptr;

      auto name = Impl.importName(decl->getDeclName());
      if (name.empty())
        return nullptr;

      // Check whether there is a function with the same name as this
      // property. If so, suppress the property; the user will have to use
      // the methods directly, to avoid ambiguities.
      auto containerTy = dc->getDeclaredTypeInContext();
      VarDecl *overridden = nullptr;
      SmallVector<ValueDecl *, 2> lookup;
      Impl.firstClangModule->lookupQualified(containerTy, name,
                                             NL_QualifiedDefault, nullptr,
                                             lookup);
      for (auto result : lookup) {
        if (isa<FuncDecl>(result))
          return nullptr;

        if (auto var = dyn_cast<VarDecl>(result))
          overridden = var;
      }

      auto type = Impl.importType(decl->getType(), ImportTypeKind::Property);
      if (!type)
        return nullptr;

      // Import the getter.
      auto getter
        = cast_or_null<FuncDecl>(Impl.importDecl(decl->getGetterMethodDecl()));
      if (!getter && decl->getGetterMethodDecl())
        return nullptr;

      // Import the setter, if there is one.
      auto setter
        = cast_or_null<FuncDecl>(Impl.importDecl(decl->getSetterMethodDecl()));
      if (!setter && decl->getSetterMethodDecl())
        return nullptr;
      
      auto result = new (Impl.SwiftContext)
                      VarDecl(Impl.importSourceLoc(decl->getLocation()),
                              name, type, dc);

      // Build thunks.
      FuncDecl *getterThunk = buildGetterThunk(getter, dc, nullptr);
      getterThunk->makeGetter(result);

      FuncDecl *setterThunk = nullptr;
      if (setter) {
        setterThunk = buildSetterThunk(setter, dc, nullptr);
        setterThunk->makeSetter(result);
      }

      // Turn this into a computed property.
      // FIXME: Fake locations for '{' and '}'?
      result->setComputedAccessors(Impl.SwiftContext, SourceLoc(),
                                   getterThunk, setterThunk,
                                   SourceLoc());
      result->setIsObjC(true);

      // Handle attributes.
      if (decl->hasAttr<clang::IBOutletAttr>())
        result->getMutableAttrs().IBOutlet = true;
      // FIXME: Handle IBOutletCollection.

      if (overridden) {
        result->setOverriddenDecl(overridden);
      }

      return result;
    }

    Decl *
    VisitObjCCompatibleAliasDecl(const clang::ObjCCompatibleAliasDecl *decl) {
      // Like C++ using declarations, name lookup simply looks through
      // Objective-C compatibility aliases. They are not imported directly.
      return nullptr;
    }

    Decl *VisitLinkageSpecDecl(const clang::LinkageSpecDecl *decl) {
      // Linkage specifications are not imported.
      return nullptr;
    }

    Decl *VisitObjCPropertyImplDecl(const clang::ObjCPropertyImplDecl *decl) {
      // @synthesize and @dynamic are not imported, since they are not part
      // of the interface to a class.
      return nullptr;
    }

    Decl *VisitFileScopeAsmDecl(const clang::FileScopeAsmDecl *decl) {
      return nullptr;
    }

    Decl *VisitAccessSpecDecl(const clang::AccessSpecDecl *decl) {
      return nullptr;
    }

    Decl *VisitFriendDecl(const clang::FriendDecl *decl) {
      // Friends are not imported; Swift has a different access control
      // mechanism.
      return nullptr;
    }

    Decl *VisitFriendTemplateDecl(const clang::FriendTemplateDecl *decl) {
      // Friends are not imported; Swift has a different access control
      // mechanism.
      return nullptr;
    }

    Decl *VisitStaticAssertDecl(const clang::StaticAssertDecl *decl) {
      // Static assertions are an implementation detail.
      return nullptr;
    }

    Decl *VisitBlockDecl(const clang::BlockDecl *decl) {
      // Blocks are not imported (although block types can be imported).
      return nullptr;
    }

    Decl *VisitClassScopeFunctionSpecializationDecl(
                 const clang::ClassScopeFunctionSpecializationDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitImportDecl(const clang::ImportDecl *decl) {
      // Transitive module imports are not handled at the declaration level.
      // Rather, they are understood from the module itself.
      return nullptr;
    }
  };
}

/// \brief Classify the given Clang enumeration to describe how it
EnumKind ClangImporter::Implementation::classifyEnum(const clang::EnumDecl *decl) {
  Identifier name;
  if (decl->getDeclName())
    name = importName(decl->getDeclName());
  else if (decl->getTypedefNameForAnonDecl())
    name = importName(decl->getTypedefNameForAnonDecl()->getDeclName());

  // Anonymous enumerations simply get mapped to constants of the
  // underlying type of the enum, because there is no way to conjure up a
  // name for the Swift type.
  if (name.empty())
    return EnumKind::Constants;

  // FIXME: For now, Options is the only usable answer, because enums
  // are broken in IRgen.
  return EnumKind::Options;
}

Decl *ClangImporter::Implementation::importDecl(const clang::NamedDecl *decl) {
  if (!decl)
    return nullptr;
  
  auto known = ImportedDecls.find(decl->getCanonicalDecl());
  if (known != ImportedDecls.end())
    return known->second;

  SwiftDeclConverter converter(*this);
  auto result = converter.Visit(decl);
  auto canon = decl->getCanonicalDecl();
  // Note that the decl was imported from Clang.  Don't mark stdlib decls as
  // imported.
  if (result && result->getDeclContext() != getSwiftModule()) {
    assert(!result->getClangDecl() || result->getClangDecl() == canon);
    result->setClangNode(canon);
  }
  if (result || !converter.hadForwardDeclaration())
    ImportedDecls[canon] = result;
  return result;
}

Decl *
ClangImporter::Implementation::importMirroredDecl(const clang::ObjCMethodDecl *decl,
                                                  DeclContext *dc) {
  if (!decl)
    return nullptr;

  auto known = ImportedProtocolDecls.find({decl->getCanonicalDecl(), dc});
  if (known != ImportedProtocolDecls.end())
    return known->second;

  SwiftDeclConverter converter(*this);
  auto result = converter.VisitObjCMethodDecl(decl, dc);
  auto canon = decl->getCanonicalDecl();
  if (result) {
    assert(!result->getClangDecl() || result->getClangDecl() == canon);
    result->setClangNode(canon);
  }
  if (result || !converter.hadForwardDeclaration())
    ImportedProtocolDecls[{canon, dc}] = result;
  return result;
}

DeclContext *
ClangImporter::Implementation::importDeclContext(const clang::DeclContext *dc) {
  // FIXME: Should map to the module we want to import into (?).
  if (dc->isTranslationUnit())
    return firstClangModule;
  
  auto decl = dyn_cast<clang::NamedDecl>(dc);
  if (!decl)
    return nullptr;

  auto swiftDecl = importDecl(decl);
  if (!swiftDecl)
    return nullptr;

  if (auto nominal = dyn_cast<NominalTypeDecl>(swiftDecl))
    return nominal;
  if (auto extension = dyn_cast<ExtensionDecl>(swiftDecl))
    return extension;
  if (auto constructor = dyn_cast<ConstructorDecl>(swiftDecl))
    return constructor;
  if (auto destructor = dyn_cast<DestructorDecl>(swiftDecl))
    return destructor;
  return nullptr;
}

DeclContext *
ClangImporter::Implementation::importDeclContextOf(const clang::Decl *D) {
  const clang::DeclContext *DC = D->getDeclContext();
  if (DC->isTranslationUnit())
    if (auto *M = getClangModuleForDecl(D))
      return M;

  return importDeclContext(DC);
}

ValueDecl *
ClangImporter::Implementation::createConstant(Identifier name, DeclContext *dc,
                                              Type type,
                                              const clang::APValue &value,
                                              ConstantConvertKind convertKind) {
  auto &context = SwiftContext;

  auto var = new (context) VarDecl(SourceLoc(), name, type, dc);

  // Form the argument patterns.
  SmallVector<Pattern *, 3> getterArgs;

  // empty tuple
  getterArgs.push_back(TuplePattern::create(context, SourceLoc(), { },
                                            SourceLoc()));
  getterArgs.back()->setType(TupleType::getEmpty(context));

  // Form the type of the getter.
  auto getterType = type;
  for (auto it = getterArgs.rbegin(), itEnd = getterArgs.rend();
       it != itEnd; ++it) {
    getterType = FunctionType::get((*it)->getType(),
                                   getterType,
                                   context);
  }

  // Create the getter function declaration.
  auto func = FuncDecl::create(context, SourceLoc(), SourceLoc(), Identifier(),
                               SourceLoc(), nullptr, getterType, getterArgs,
                               getterArgs, TypeLoc::withoutLoc(type), dc);
  func->setBodyResultType(type);

  setVarDeclContexts(getterArgs, func);

  // Create the integer literal value.
  // FIXME: Handle other kinds of values.
  Expr *expr = nullptr;
  switch (value.getKind()) {
  case clang::APValue::AddrLabelDiff:
  case clang::APValue::Array:
  case clang::APValue::ComplexFloat:
  case clang::APValue::ComplexInt:
  case clang::APValue::LValue:
  case clang::APValue::MemberPointer:
  case clang::APValue::Struct:
  case clang::APValue::Uninitialized:
  case clang::APValue::Union:
  case clang::APValue::Vector:
    llvm_unreachable("Unhandled APValue kind");

  case clang::APValue::Float:
  case clang::APValue::Int: {
    // Print the value.
    llvm::SmallString<16> printedValue;
    if (value.getKind() == clang::APValue::Int) {
      value.getInt().toString(printedValue);
    } else {
      assert(value.getFloat().isFinite() && "can't handle infinities or NaNs");
      value.getFloat().toString(printedValue);
    }

    // If this was a negative number, record that and strip off the '-'.
    // FIXME: This is hideous!
    // FIXME: Actually make the negation work.
    bool isNegative = printedValue[0] == '-';
    if (isNegative)
      printedValue.erase(printedValue.begin());

    // Create the expression node.
    StringRef printedValueCopy(context.AllocateCopy(printedValue).data(),
                               printedValue.size());
    if (value.getKind() == clang::APValue::Int) {
      expr = new (context) IntegerLiteralExpr(printedValueCopy, SourceLoc(),
                                              /*Implicit=*/true);
    } else {
      expr = new (context) FloatLiteralExpr(printedValueCopy, SourceLoc(),
                                            /*Implicit=*/true);
    }

    if (!isNegative)
      break;

    // If it was a negative number, negate the integer literal.
    auto minus = context.getIdentifier("-");
    UnqualifiedLookup lookup(minus, getSwiftModule(), nullptr);
    if (!lookup.isSuccess())
      return nullptr;

    Expr* minusRef;
    SmallVector<ValueDecl *, 4> found;
    for (auto &result : lookup.Results) {
      if (!result.hasValueDecl())
        continue;

      if (!isa<FuncDecl>(result.getValueDecl()))
        continue;

      found.push_back(result.getValueDecl());
    }

    if (found.empty())
      return nullptr;

    if (found.size() == 1) {
      minusRef = new (context) DeclRefExpr(found[0], SourceLoc(),
                                           /*Implicit=*/true);
    } else {
      auto foundCopy = context.AllocateCopy(found);
      minusRef = new (context) OverloadedDeclRefExpr(
                                 foundCopy, SourceLoc(), /*Implicit=*/true);
    }

    expr = new (context) PrefixUnaryExpr(minusRef, expr);
    break;
  }
  }

  // If we need a conversion, add one now.
  switch (convertKind) {
  case ConstantConvertKind::None:
    break;

  case ConstantConvertKind::Construction: {
    auto typeRef = new (context) MetatypeExpr(nullptr, SourceLoc(),
                                              MetaTypeType::get(type, context));
    expr = new (context) CallExpr(typeRef, expr, /*Implicit=*/true);
    break;
   }

  case ConstantConvertKind::Coerce:
    break;

  case ConstantConvertKind::Downcast: {
    auto cast = new (context) UnconditionalCheckedCastExpr(expr,
                                                     SourceLoc(),
                                                     SourceLoc(),
                                                     TypeLoc::withoutLoc(type));
    cast->setCastKind(CheckedCastKind::Downcast);
    cast->setImplicit();
    expr = cast;
    break;
  }
  }

  // Create the return statement.
  auto ret = new (context) ReturnStmt(SourceLoc(), expr);

  // Finally, set the body.
  func->setBody(BraceStmt::create(context, SourceLoc(),
                                  BraceStmt::ExprStmtOrDecl(ret),
                                  SourceLoc()));

  // Write the function up as the getter.
  func->makeGetter(var);
  var->setComputedAccessors(context, SourceLoc(), func, nullptr, SourceLoc());

  // Register this thunk as an external definition.
  SwiftContext.addedExternalDecl(func);

  return var;

}

