//===--- JSONASTDumper.cpp - Swift Language JSON AST Dumper ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements JSON-formatted dumping for the Swift ASTs.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Version.h"
#include "swift/AST/Stmt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

namespace {

/// Returns the USR of the given declaration.
std::string declUSR(const Decl *D) {
  if (!D) return "";

  std::string usr;
  llvm::raw_string_ostream os(usr);
  if (swift::ide::printDeclUSR(D, os))
    return "";
  return usr;
}

/// Returns a vector of USRs from a sequence of declarations.
template <typename T>
std::vector<std::string> declUSRs(const T &decls) {
  std::vector<std::string> result;
  for (auto d : decls)
    result.push_back(declUSR(d));
  return result;
}

/// Returns the USR of the given type.
std::string typeUSR(Type type) {
  if (!type) return "";

  std::string usr;
  llvm::raw_string_ostream os(usr);
  if (swift::ide::printTypeUSR(type, os))
    return "";
  return usr;
}

/// Returns the USR of the given value declaration's type.
std::string declTypeUSR(const ValueDecl *D) {
  if (!D) return "";

  std::string usr;
  llvm::raw_string_ostream os(usr);
  if (swift::ide::printDeclTypeUSR(D, os))
    return "";
  return usr;
}

std::string jsonStringForAccessorKind(AccessorKind kind) {
  switch (kind) {
  case AccessorKind::Get: return "get";
  case AccessorKind::DistributedGet: return "_distributed_get";
  case AccessorKind::Set: return "set";
  case AccessorKind::Read: return "_read";
  case AccessorKind::Modify: return "_modify";
  case AccessorKind::WillSet: return "willSet";
  case AccessorKind::DidSet: return "didSet";
  case AccessorKind::Address: return "unsafeAddress";
  case AccessorKind::MutableAddress: return "unsafeMutableAddress";
  case AccessorKind::Init: return "init";
  }
}

std::string jsonStringForAssociativity(Associativity assoc) {
  switch (assoc) {
  case Associativity::None: return "none";
  case Associativity::Left: return "left";
  case Associativity::Right: return "right";
  }
}

std::string jsonStringForCtorInitializerKind(CtorInitializerKind kind) {
  switch (kind) {
  case CtorInitializerKind::Convenience: return "convenience";
  case CtorInitializerKind::ConvenienceFactory: return "convenienceFactory";
  case CtorInitializerKind::Designated: return "designated";
  case CtorInitializerKind::Factory: return "factory";
  }
}

std::string jsonStringForImportKind(ImportKind kind) {
  switch (kind) {
  case ImportKind::Module: return "module";
  case ImportKind::Type: return "type";
  case ImportKind::Struct: return "struct";
  case ImportKind::Class: return "class";
  case ImportKind::Enum: return "enum";
  case ImportKind::Protocol: return "protocol";
  case ImportKind::Var: return "var";
  case ImportKind::Func: return "func";
  }
}

/// This class dumps a type-checked Swift AST in JSON format to some output
/// stream (either a file or standard out).
///
/// The only guarantees made for this output are as follows:
///
/// -   The output is valid JSON.
/// -   The top-level object contains a `compilerVersion` key whose value is
///     the full compiler version as a string (in the same format that would
///     be written in a .swiftinterface file).
///
/// The Swift compiler may introduce new AST nodes, remove AST nodes, or
/// change the layout of AST nodes at any time, so no stability is implied;
/// the structure and layout of nodes may change between compiler versions.
/// No promise is made that the output contains all possible information
/// about the AST, only that what is presented there is correct. It is also
/// a non-goal for the output to repeat with full fidelity purely syntactic
/// information about nodes (their locations, spelling, etc.) that could be
/// obtained from a syntax parse of the source, although minimal source
/// location information is provided for nodes so that consumers may relate
/// semantic nodes to corresponding syntax nodes.
///
/// Despite the above warnings, the output presented here is meant to be
/// useful to consumers who wish to perform semantic analysis on Swift
/// sources at a large scale, and this guides how certain data is represented
/// in the dumped AST. For example, references to declarations and types are
/// expressed as USRs so that this output can be combined with and related to
/// other data sources like indexstore and SourceKit.
class JSONASTVisitor : public ASTVisitor<JSONASTVisitor> {
private:
  llvm::json::OStream json;
  ASTContext &ctx;

public:
  JSONASTVisitor(llvm::raw_ostream &os, ASTContext &ctx) : json(os, /*indent=*/ 2), ctx(ctx) {}

  void visitSourceFile(SourceFile &SF) {
    json.object([&](){
      attributeString("file", SF.getFilename());
      attributeString("compilerVersion", version::getSwiftFullVersion(version::Version::getCurrentLanguageVersion()));
      json.attributeArray("topLevelItems", [&](){
        for (auto item : SF.getTopLevelItems()) {
          value(item);
        }
      });
    });
  }

//   void emitNode(ASTNode node, std::function<void()> body) {
//     json.objectBegin();
//     writeKindAttribute(json, node);
//     if (Decl *decl = node.dyn_cast<Decl *>()) {
//       emitDeclUSRAttribute("usr", decl);
//     }
//     writeSourceRange(json, node, ctx);
//     if (node.isImplicit()) {
//       json.attribute("isImplicit", true);
//     }
//     body();
//     json.objectEnd();
//   }

  // MARK: Declarations

  void writeDeclCommon(Decl *D) {
    json.attributeArray("attributes", [&](){
      for (auto A : D->getAttrs()) {
        writeAttr(A, D->getDeclContext());
      }
    });
  }

  void writeValueDeclCommon(ValueDecl *D) {
    writeDeclCommon(D);
  }

  void writeTypeDeclCommon(TypeDecl *D) {
    writeValueDeclCommon(D);
    attributeString("name", D->getName().str());
  }

  void writeGenericTypeDeclCommon(GenericTypeDecl *D) {
    writeTypeDeclCommon(D);
  }

  void writeNominalTypeDeclCommon(NominalTypeDecl *D) {
    writeGenericTypeDeclCommon(D);
    attributeArray("inherited", D->getInherited().getEntries());
    attributeArray("members", D->getABIMembers());

  }

  void visitEnumDecl(EnumDecl *D) {
    writeNode(D, [&](){
      writeNominalTypeDeclCommon(D);
      attributeBool("isIndirect", D->isIndirect());
    });
  }

  void visitStructDecl(StructDecl *D) {
    writeNode(D, [&](){
      writeNominalTypeDeclCommon(D);
      attributeBool("hasUnreferenceableStorage", D->hasUnreferenceableStorage());
      attributeBool("isCxxNonTrivial", D->isCxxNonTrivial());
      attributeBool("isNonTrivialPtrAuth", D->isNonTrivialPtrAuth());
    });
  }

  void visitClassDecl(ClassDecl *D) {
    writeNode(D, [&](){
      writeNominalTypeDeclCommon(D);
    });
  }

  void visitProtocolDecl(ProtocolDecl *D) {
    writeNode(D, [&](){
      writeNominalTypeDeclCommon(D);
    });
  }

  void visitBuiltinTupleDecl(BuiltinTupleDecl *D) {
    writeNode(D, [&](){
      writeNominalTypeDeclCommon(D);
    });
  }

  void visitOpaqueTypeDecl(OpaqueTypeDecl *D) {
    writeNode(D, [&](){
      writeGenericTypeDeclCommon(D);
    });
  }

  void visitTypeAliasDecl(TypeAliasDecl *D) {}
  void visitGenericTypeParamDecl(GenericTypeParamDecl *D) {}
  void visitAssociatedTypeDecl(AssociatedTypeDecl *D) {}
  void visitModuleDecl(ModuleDecl *D) {}

  void visitVarDecl(VarDecl *D) {
    writeNode(D, [&](){
      writeAbstractStorageDeclCommon(D);
      attributeString("introducer", D->getIntroducerStringRef());
      attributeArray("accessors", D->getAllAccessors());
    });
  }

  void writeAbstractStorageDeclCommon(AbstractStorageDecl *D) {
    writeValueDeclCommon(D);
  }

  void visitParamDecl(ParamDecl *D) {}

  void writeAbstractFunctionDeclCommon(AbstractFunctionDecl *D) {
    writeValueDeclCommon(D);
    attribute("declName", D->getName());
    attributeBool("hasThrows", D->hasThrows());
    attributeString("thrownTypeUSR", typeUSR(D->getThrownInterfaceType()));
    attributeBool("hasAsync", D->hasAsync());
    attributeString("implicitSelfUSR", declUSR(D->getImplicitSelfDecl()));
    attributeString("overriddenDeclUSR", declUSR(D->getOverriddenDecl()));
    attribute("opaqueResultType", D->getOpaqueResultTypeDecl());
  }

  void visitSubscriptDecl(SubscriptDecl *D) {
    writeNode(D, [&](){
      writeAbstractStorageDeclCommon(D);
      attributeString("elementTypeUSR", typeUSR(D->getElementInterfaceType()));
    });
  }

  void visitConstructorDecl(ConstructorDecl *D) {
    writeNode(D, [&](){
      writeAbstractFunctionDeclCommon(D);
      attributeBool("isConvenienceInit", D->isConvenienceInit());
      attributeString("initKind", jsonStringForCtorInitializerKind(D->getInitKind()));
    });
  }

  void visitDestructorDecl(DestructorDecl *D) {
    writeNode(D, [&](){
      writeAbstractFunctionDeclCommon(D);
    });
  }

  void visitFuncDecl(FuncDecl *D) {
    writeNode(D, [&](){
      writeAbstractFunctionDeclCommon(D);
      attributeString("resultTypeUSR", typeUSR(D->getResultInterfaceType()));
      attribute("body", D->getBody());
    });
  }

  void visitAccessorDecl(AccessorDecl *D) {
    writeNode(D, [&](){
      attributeString("accessorKind", jsonStringForAccessorKind(D->getAccessorKind()));
      attributeString("storage", declUSR(D->getStorage()));
      if (D->isInitAccessor()) {
        std::vector<std::string> initUSRs = declUSRs(D->getInitializedProperties());
        if (!initUSRs.empty())
          attributeArray("initializes", initUSRs);

        std::vector<std::string> accessUSRs = declUSRs(D->getAccessedProperties());
        if (!accessUSRs.empty())
          attributeArray("accesses", accessUSRs);
      }
    });
  }

  void visitMacroDecl(MacroDecl *D) {
    writeNode(D, [&](){
      attribute("definition", D->definition);
      attributeString("resultTypeUSR", typeUSR(D->getResultInterfaceType()));
    });
  }

  void visitExtensionDecl(ExtensionDecl *D) {
    writeNode(D, [&](){
      writeDeclCommon(D);
      attributeString("extendedTypeUSR", typeUSR(D->getExtendedType()));
      attributeArray("inherited", D->getInherited().getEntries());
      attributeArray("members", D->getABIMembers());
    });
  }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *D) {
    writeNode(D, [&](){
      attribute("body", D->getBody());
    });
  }

  void visitImportDecl(ImportDecl *D) {
    writeNode(D, [&](){
      writeDeclCommon(D);
      attributeString("importKind", jsonStringForImportKind(D->getImportKind()));
      attributeArray("modulePath", D->getModulePath());
      attributeArray("accessPath", D->getAccessPath());
    });
  }

  void visitPoundDiagnosticDecl(PoundDiagnosticDecl *D) {
    writeNode(D, [&](){
      attributeBool("isError", D->isError());
      attribute("message", D->getMessage());
    });
  }

  void visitPrecedenceGroupDecl(PrecedenceGroupDecl *D) {
    writeNode(D, [&](){
      writeDeclCommon(D);
      attributeString("name", D->getName().str());
      attributeString("associativity", jsonStringForAssociativity(D->getAssociativity()));
      attributeBool("isAssignment", D->isAssignment());
      attributeArray("higherThan", D->getHigherThan());
      attributeArray("lowerThan", D->getLowerThan());
    });
  }

  void visitMissingDecl(MissingDecl *D) {}
  void visitMissingMemberDecl(MissingMemberDecl *D) {}

  void visitPatternBindingDecl(PatternBindingDecl *D) {}

  void visitEnumCaseDecl(EnumCaseDecl *D) {
    writeNode(D, [&](){
      // The EnumElements will also be written under the "members" key. To
      // avoid redundant information, we just write the elements' USRs here.
      // This allows users to relate which members were grouped together in
      // which `case` declarations if they wish.
      json.attributeBegin("elements");
      json.arrayBegin();
      for (const auto *element : D->getElements()) {
        json.value(declUSR(element));
      }
      json.arrayEnd();
      json.attributeEnd();
    });
  }

  void visitEnumElementDecl(EnumElementDecl *D) {
    writeNode(D, [&](){
      attributeBool("isIndirect", D->isIndirect());
    });
  }

  void visitInfixOperatorDecl(InfixOperatorDecl *D) {}
  void visitPrefixOperatorDecl(PrefixOperatorDecl *D) {}
  void visitPostfixOperatorDecl(PostfixOperatorDecl *D) {}
  void visitMacroExpansionDecl(MacroExpansionDecl *D) {}

  // MARK: Statements

  void visitBraceStmt(BraceStmt *S) {
    writeNode(S, [&](){
      attributeArray("elements", S->getElements());
    });
  }

  void visitReturnStmt(ReturnStmt *S) {
    writeNode(S, [&](){
      attribute("result", S->getResult());
    });
  }

  void visitThenStmt(ThenStmt *S) {}

  void visitYieldStmt(YieldStmt *S) {
    writeNode(S, [&](){
      attributeArray("yields", S->getYields());
    });
  }

  void visitDeferStmt(DeferStmt *S) {
    writeNode(S, [&](){
      attribute("tempDecl", S->getTempDecl());
      attribute("tempCallExpr", S->getCallExpr());
    });
  }

  void visitIfStmt(IfStmt *S) {
    writeNode(S, [&](){
      writeLabeledConditionalStmtCommon(S);
      attribute("conditions", S->getCond());
    });
  }

  void visitGuardStmt(GuardStmt *S) {
    writeNode(S, [&](){
      writeLabeledConditionalStmtCommon(S);
      attribute("conditions", S->getCond());
      attribute("body", S->getBody());
    });
  }

  void visitWhileStmt(WhileStmt *S) {
    writeNode(S, [&](){
      writeLabeledConditionalStmtCommon(S);
      attribute("conditions", S->getCond());
      attribute("body", S->getBody());
    });
  }

  void visitDoStmt(DoStmt *S) {
    // emitNode(stmt, [&](){
    //   emitChild("body", stmt->getBody());
    // });
  }

  void visitDoCatchStmt(DoCatchStmt *S) {}
  void visitRepeatWhileStmt(RepeatWhileStmt *S) {}
  void visitForEachStmt(ForEachStmt *S) {}
  void visitSwitchStmt(SwitchStmt *S) {}
  void visitCaseStmt(CaseStmt *S) {}

  void visitBreakStmt(BreakStmt *S) {
    writeNode(S, [&](){
      attributeString("target", S->getTargetName().str());
    });
  }

  void visitContinueStmt(ContinueStmt *S) {
    writeNode(S, [&](){
      attributeString("target", S->getTargetName().str());
    });
  }

  void visitFallthroughStmt(FallthroughStmt *S) {
    writeNode(S, [&](){});
  }

  void visitFailStmt(FailStmt *S) {
    writeNode(S, [&](){});
  }

  void visitThrowStmt(ThrowStmt *S) {
    writeNode(S, [&](){
      attribute("subExpr", S->getSubExpr());
    });
  }

  void visitDiscardStmt(DiscardStmt *S) {
    writeNode(S, [&](){
      attribute("subExpr", S->getSubExpr());
    });
  }

  void visitPoundAssertStmt(PoundAssertStmt *S) {
    writeNode(S, [&](){
      attribute("condition", S->getCondition());
      attributeString("message", S->getMessage());
    });
  }

  void writeLabeledStmt(LabeledStmt *S) {
    if (!S->getLabelInfo().Name.empty())
      attributeString("label", S->getLabelInfo().Name.str());
  }

  void writeLabeledConditionalStmtCommon(LabeledConditionalStmt *S) {
    writeLabeledStmt(S);
  }

  // MARK: Expressions

  void visitExpr(Expr *E) {
    writeNode(E, [&](){
    });
  }

  void visitBinaryExpr(BinaryExpr *E) {
    writeNode(E, [&](){
      attribute("fn", E->getFn());
      attribute("lhs", E->getLHS());
      attribute("rhs", E->getRHS());
    });
  }

  void visitDeclRefExpr(DeclRefExpr *E) {
    writeNode(E, [&](){
      attributeConcreteDeclRef("declRef", E->getDeclRef());
    });
  }

  void visitImplicitConversionExpr(ImplicitConversionExpr *E) {
    writeNode(E, [&](){
      attribute("subExpr", E->getSubExpr());
    });
  }

  void writeApplyExprCommon(ApplyExpr *E) {
    attribute("fn", E->getFn());
    attributeArray("args", *E->getArgs());
  }

  void visitApplyExpr(ApplyExpr *E) {
    writeNode(E, [&](){
      writeApplyExprCommon(E);
    });
  }

  void visitSelfApplyExpr(SelfApplyExpr *E) {
    writeNode(E, [&](){
      writeApplyExprCommon(E);
      attribute("base", E->getBase());
    });
  }

//   void visitCallExpr(CallExpr *expr) {
//     emitNode(expr, [&](){
//       emitTypeUSRAttribute("typeUSR", expr->getType());
//       emitChild("fn", expr->getFn());

//     });
//   }

//   void visitDeclRefExpr(DeclRefExpr *expr) {
//     emitNode(expr, [&](){
//       emitTypeUSRAttribute("typeUSR", expr->getType());
//       emitChild("decl", expr->getDeclRef());
      
//     });
//   }

//   void visitIntegerLiteralExpr(IntegerLiteralExpr *expr) {
//     emitNode(expr, [&](){
//       emitTypeUSRAttribute("typeUSR", expr->getType());
//       json.attribute("value", expr->getDigitsText());
//     });
//   }

//   void visitMemberRefExpr(MemberRefExpr *expr) {
//     emitNode(expr, [&](){
//       emitTypeUSRAttribute("typeUSR", expr->getType());
//       emitChild("base", expr->getBase());
//       emitChild("member", expr->getMember());
//     });
//   }

//   void visitParenExpr(ParenExpr *expr) {
//     emitNode(expr, [&](){
//       emitTypeUSRAttribute("typeUSR", expr->getType());
//       emitChild("subexpr", expr->getSubExpr());
//     });
//   }

  // MARK: Patterns

  void visitParenPattern(ParenPattern *P) {
    writeNode(P, [&](){
      attribute("subPattern", P->getSubPattern());
    });
  }

  void visitTuplePattern(TuplePattern *P) {
    writeNode(P, [&](){

    });
  }

  void visitNamedPattern(NamedPattern *P) {
    writeNode(P, [&](){
      attribute("decl", P->getDecl());
    });
  }

  void visitAnyPattern(AnyPattern *P) {
    writeNode(P, [&](){
      attributeBool("isAsyncLet", P->isAsyncLet());
    });
  }

  void visitTypedPattern(TypedPattern *P) {
    writeNode(P, [&](){
      attribute("subPattern", P->getSubPattern());
      attributeString("typeUSR", typeUSR(P->getType()));
    });
  }

  void visitBindingPattern(BindingPattern *P) {
    writeNode(P, [&](){
      attributeString("introducer", P->getIntroducerStringRef());
      attribute("subPattern", P->getSubPattern());
    });
  }

  void visitIsPattern(IsPattern *P) {
    writeNode(P, [&](){

    });
  }

  void visitEnumElementPattern(EnumElementPattern *P) {
    writeNode(P, [&](){
      attribute("name", P->getName().getFullName());
      attributeString("parentTypeUSR", typeUSR(P->getParentType()));
      if (P->hasUnresolvedOriginalExpr()) {
        attribute("unresolvedExpr", P->getUnresolvedOriginalExpr());
      } else {
        attributeString("elementDeclUSR", declUSR(P->getElementDecl()));
      }
      attribute("subPattern", P->getSubPattern());
    });
  }

  void visitOptionalSomePattern(OptionalSomePattern *P) {
    writeNode(P, [&](){
      attribute("subPattern", P->getSubPattern());
    });
  }

  void visitBoolPattern(BoolPattern *P) {
    writeNode(P, [&](){
      attributeBool("value", P->getValue());
    });
  }

  void visitExprPattern(ExprPattern *P) {
    writeNode(P, [&](){

    });
  }


  // MARK: Types

  void visitTypeRepr(TypeRepr *type) {}

private:
  // MARK: General writing helpers

  void value(StringRef s) {
    json.value(s);
  }

  void value(const char *s) {
    json.value(s);
  }

  void value(ASTNode node) {
    if (auto D = node.dyn_cast<Decl *>()) {
      visit(D);
    } else if (auto S = node.dyn_cast<Stmt *>()) {
      visit(S);
    } else if (auto P = node.dyn_cast<Pattern *>()) {
      visit(P);
    } else {
      auto E = node.get<Expr *>();
      visit(E);
    }
  }

  void value(Argument arg) {
    json.object([&](){
      attributeString("label", arg.getLabel().str());
      attribute("expr", arg.getExpr());
    });
  }

  void writeAttr(DeclAttribute *attr, DeclContext *DC) {
    json.object([&](){
      if (const CustomAttr *customAttr = dyn_cast<CustomAttr>(attr)) {
        attributeString("typeUSR", typeUSR(customAttr->getType()));
        attributeArray("args", *customAttr->getArgs());
      } else {
        attributeString("attrName", attr->getAttrName());
        if (const ImplementsAttr *A = dyn_cast<ImplementsAttr>(attr)) {
          attributeString("protocolDeclUSR", declUSR(A->getProtocol(DC)));
          attribute("memberName", A->getMemberName());
        }
      }
    });
  }

  void value(DeclBaseName baseName) {
    json.object([&](){
      switch (baseName.getKind()) {
      case DeclBaseName::Kind::Constructor:
        attributeString("special", "init");
        break;
      case DeclBaseName::Kind::Destructor:
        attributeString("special", "deinit");
        break;
      case DeclBaseName::Kind::Subscript:
        attributeString("special", "subscript");
        break;
      case DeclBaseName::Kind::Normal:
        attributeString("name", baseName.getIdentifier().str());
        attributeBool("isOperator", baseName.isOperator());
      }
    });
  }

  void value(DeclName declName) {
    json.object([&](){
      json.attributeBegin("baseName");
      value(declName.getBaseName());
      json.attributeEnd();
      attributeArray("arguments", declName.getArgumentNames());
    });
  }

  void value(Identifier ident) {
    json.value(ident.str());
  }

  void value(const InheritedEntry &entry) {
    json.object([&](){
      attributeString("type", typeUSR(entry.getType()));
      attributeBool("isPreconcurrency", entry.isPreconcurrency());
      attributeBool("isRetroactive", entry.isRetroactive());
      attributeBool("isSuppressed", entry.isSuppressed());
      attributeBool("isUnchecked", entry.isUnchecked());
    });
  }

  template <typename T>
  void value(Located<T> v) {
    value(v.Item);
  }

  void value(const PrecedenceGroupDecl::Relation &relation) {
    json.object([&](){
      attributeString("name", relation.Name.str());
    });
  }

  void value(StmtCondition cond) {
    json.arrayBegin();
    for (const auto &element : cond) {
      json.objectBegin();
      switch (element.getKind()) {
      case StmtConditionElement::CK_Boolean:
        attributeString("kind", "boolean");
        attributeBool("expr", element.getBoolean());
        break;
      case StmtConditionElement::CK_PatternBinding:
        attributeString("kind", "pattern");
        attribute("pattern", element.getPattern());
        attribute("initializer", element.getInitializer());
        break;
      case StmtConditionElement::CK_Availability:
        attributeString("kind", "availability");
        // TODO
        break;
      case StmtConditionElement::CK_HasSymbol:
        attributeString("kind", "hasSymbol");
        // TODO
        break;
      }
      json.objectEnd();
    }
    json.arrayEnd();
  }

  void value(SubstitutionMap subst) {
    GenericSignature sig = subst.getGenericSignature();
    size_t numParams = sig.getGenericParams().size();
    size_t numReqs = sig.getRequirements().size();

    json.object([&](){
      if (numParams) {
        json.attributeArray("replacements", [&](){
          for (size_t i = 0; i < numParams; ++i) {
            json.object([&](){
              attributeString("typeUSR", typeUSR(subst.getReplacementTypes()[i]));
            });
          }
        });
      }
      if (numReqs) {
        json.attributeArray("requirements", [&](){
          for (size_t i = 0; i < numReqs; i++) {
            json.object([&](){
              const ProtocolConformanceRef &ref = subst.getConformances()[i];
              if (ref.isAbstract()) {
                attributeString("kind", "abstract");
                attributeString("protocolDeclUSR", declUSR(ref.getAbstract()));
              } else if (ref.isConcrete()) {
                ProtocolConformance *conf = ref.getConcrete();
                attributeString("kind", "concrete");
                attributeString("conformingTypeUSR", typeUSR(conf->getType()));
                attributeString("protocolDeclUSR", declUSR(conf->getProtocol()));
              } else if (ref.isPack()) {
                PackConformance *conf = ref.getPack();
                attributeString("kind", "pack");
                attributeString("conformingTypeUSR", typeUSR(conf->getType()));
                attributeString("protocolDeclUSR", declUSR(conf->getProtocol()));
              }
            });
          }
        });
      }
    });
  }

  /// This should be the top-most call in any AST node's `visit*` method. This
  /// method will create a new JSON object with the attributes that are common
  /// to all nodes: kind, location, and so forth. The lambda passed to the
  /// function is responsible for writing additional attributes and visiting
  /// child nodes.
  void writeNode(ASTNode node, std::function<void()> body = [](){}) {
    json.object([&](){
      writeKindAttribute(node);
      attributeBool("isImplicit", node.isImplicit());
      if (Decl *D = node.dyn_cast<Decl *>()) {
        attributeString("usr", declUSR(D));
      } else if (Expr *E = node.dyn_cast<Expr *>()) {
        attributeString("typeUSR", typeUSR(E->getType()));
      } else if (Pattern *P = node.dyn_cast<Pattern *>()) {
        attributeString("typeUSR", typeUSR(P->getType()));
      }
      writeSourceRange(node);
      body();
    });
  }

  /// Writes a Boolean-valued attribute into the current object, only if the
  /// value is true.
  void attributeBool(StringRef k, bool v) {
    if (!v) return;
    json.attribute(k, v);
  }

  void attributeConcreteDeclRef(StringRef k, ConcreteDeclRef v) {
    json.attributeObject(k, [&](){
      attributeString("name", v.getDecl()->getBaseName().getIdentifier().str());
      attributeString("usr", declUSR(v.getDecl()));
      attributeString("typeUSR", declTypeUSR(v.getDecl()));
      attribute("substitutions", v.getSubstitutions());
    });
  }

  /// Writes a string-valued attribute into the current object, only if the
  /// value is non-empty.
  void attributeString(StringRef k, StringRef v) {
    if (v.empty()) return;
    json.attribute(k, v);
  }

  template <typename T>
  void attribute(StringRef k, const T& v) {
    json.attributeBegin(k);
    value(v);
    json.attributeEnd();
  }

  template <typename T>
  void attribute(StringRef k, T *v) {
    if (v == nullptr) return;

    json.attributeBegin(k);
    visit(v);
    json.attributeEnd();
  }

  template <typename T>
  void attributeArray(StringRef k, const T &elements) {
    if (elements.begin() == elements.end()) return;

    json.attributeArray(k, [&](){
      for (auto element : elements) {
        value(element);
      }
    });
  }

  void writeKindAttribute(ASTNode node) {
    if (Decl *D = node.dyn_cast<Decl *>()) {
      attributeString("kind", std::string(Decl::getKindName(D->getKind())) + "Decl");
    } else if (Expr *E = node.dyn_cast<Expr *>()) {
      attributeString("kind", std::string(Expr::getKindName(E->getKind())) + "Expr");
    } else if (Stmt *S = node.dyn_cast<Stmt *>()) {
      attributeString("kind", std::string(Stmt::getKindName(S->getKind())) + "Stmt");
    } else if (Pattern *P = node.dyn_cast<Pattern *>()) {
      attributeString("kind", std::string(Pattern::getKindName(P->getKind())) + "Pattern");
    }
  }

  void writeSourceRange(ASTNode node) {
    SourceLoc sloc = node.getStartLoc();
    SourceLoc eloc = node.getEndLoc();
    if (sloc.isInvalid() || eloc.isInvalid()) return;

    SourceManager &srcMgr = ctx.SourceMgr;
    unsigned startBufferID = srcMgr.findBufferContainingLoc(sloc);
    unsigned startOffset = srcMgr.getLocOffsetInBuffer(sloc, startBufferID);
    unsigned endBufferID = srcMgr.findBufferContainingLoc(eloc);
    unsigned endOffset = srcMgr.getLocOffsetInBuffer(eloc, endBufferID);
    json.attributeArray("sourceRange", [&](){
      json.value(startOffset);
      json.value(endOffset);
    });
  }
};

} // end anonymous namespace

void SourceFile::dumpJSON(ASTContext &ctx, llvm::raw_ostream &os) {
  JSONASTVisitor(os, ctx).visitSourceFile(*this);
}
