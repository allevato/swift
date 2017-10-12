//===--- DerivedConformanceRawRepresentable.cpp - Derived RawRepresentable ===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements implicit derivation of the RawRepresentable protocol
//  for an enum.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "DerivedConformances.h"

using namespace swift;
using namespace DerivedConformance;

bool DerivedConformance::canDeriveValueEnumerable(
    TypeChecker &tc, NominalTypeDecl *type, ValueDecl *requirement) {
  if (auto enumDecl = dyn_cast<EnumDecl>(type)) {
    // Validate the enum.
    tc.validateDecl(enumDecl);

    // ValueEnumerable can be synthesized for enums that have at least one case
    // and where no case has associated values.
    // TODO: Support enums with ValueEnumerable payloads.
    return enumDecl->hasCases() &&
        enumDecl->hasOnlyCasesWithoutAssociatedValues();
  }

  // Types other than enums are not supported.
  return false;
}

static Type computeValueSequenceType(TypeChecker &tc, EnumDecl *enumDecl) {
  auto parentDC = enumDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();
  
  auto selfType = enumDecl->getDeclaredInterfaceType();

  auto valueSequenceType = BoundGenericType::get(
      C.getAnyRandomAccessCollectionDecl(), nullptr, { selfType });
  return valueSequenceType;
}

static Type deriveValueEnumerable_ValueSequence(TypeChecker &tc,
                                                Decl *parentDecl,
                                                EnumDecl *enumDecl) {
  // enum SomeEnum: ValueEnumerable {
  //   case A, B
  //
  //   @derived
  //   typealias ValueSequence = AnyRandomAccessCollection<SomeEnum>
  // }
  auto valueSequenceType = computeValueSequenceType(tc, enumDecl);
  return cast<DeclContext>(parentDecl)->mapTypeIntoContext(valueSequenceType);
}

/// Returns a new integer literal expression with the given value.
/// \p C The AST context.
/// \p value The integer value.
/// \return The integer literal expression.
static Expr* integerLiteralExpr(ASTContext &C, int64_t value) {
  llvm::SmallString<8> integerVal;
  APInt(32, value).toString(integerVal, 10, /*signed*/ false);
  auto integerStr = C.AllocateCopy(integerVal);
  auto integerExpr = new (C) IntegerLiteralExpr(
    StringRef(integerStr.data(), integerStr.size()), SourceLoc(),
    /*implicit*/ true);
  return integerExpr;
}

static void deriveBodyValueEnumerable_allValues(
    AbstractFunctionDecl *allValuesDecl) {
  // enum SomeEnum: ValueEnumerable {
  //   case A, B
  //
  //   @derived
  //   static var allValues: AnyRandomAccessCollection<SomeEnum> {
  //     return AnyRandomAccessCollection((0..<2).lazy.map {
  //       switch $0 {
  //       case 0: return A
  //       case 1: return B
  //       default: unreachable()
  //       }
  //       return result
  //     })
  //   }
  // }

  auto parentDC = allValuesDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto enumDecl = parentDC->getAsEnumOrEnumExtensionContext();

  auto valueSequenceType = computeValueSequenceType(tc, enumDecl);
  valueSequenceType = allValuesDecl->mapTypeIntoContext(valueSequenceType);

  Type enumType = parentDC->getDeclaredTypeInContext();

  SmallVector<ASTNode, 4> cases;
  auto ordinal = 0;
  for (auto elt : enumDecl->getAllElements()) {
    auto ordinalExpr = integerLiteralExpr(C, ordinal++);
    auto pat = new (C) ExprPattern(ordinalExpr, /*isResolved*/ true, nullptr,
        nullptr);
    pat->setImplicit();

    auto labelItem =
      CaseLabelItem(/*IsDefault=*/false, pat, SourceLoc(), nullptr);

    auto eltRef = new (C) DeclRefExpr(elt, DeclNameLoc(), /*implicit*/true);
    auto metaTyRef = TypeExpr::createImplicit(enumType, C);
    auto valueExpr = new (C) DotSyntaxCallExpr(eltRef, SourceLoc(), metaTyRef);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), valueExpr);

    auto body = BraceStmt::create(C, SourceLoc(),
                                  ASTNode(returnStmt), SourceLoc());

    cases.push_back(CaseStmt::create(C, SourceLoc(), labelItem,
                                     /*HasBoundDecls=*/false, SourceLoc(),
                                     body));
  }

  auto selfRef = createSelfDeclRef(toRawDecl);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), selfRef,
                                       SourceLoc(), cases, SourceLoc(), C);
  auto body = BraceStmt::create(C, SourceLoc(),
                                ASTNode(switchStmt),
                                SourceLoc());
  allValuesDecl->setBody(body);
}

static VarDecl *deriveValueEnumerable_allValues(TypeChecker &tc,
                                                Decl *parentDecl,
                                                EnumDecl *enumDecl) {
  ASTContext &C = tc.Context;

  auto valueSequenceInterfaceType = computeValueSequenceType(tc, enumDecl);

  auto parentDC = cast<DeclContext>(parentDecl);
  auto valueSequenceType =
      parentDC->mapTypeIntoContext(valueSequenceInterfaceType);
  // Define the getter.
  auto getterDecl = declareDerivedPropertyGetter(tc, parentDecl, enumDecl,
                                                 valueSequenceInterfaceType,
                                                 valueSequenceType,
                                                 /*isStatic=*/true,
                                                 /*isFinal=*/false);
  getterDecl->setBodySynthesizer(&deriveBodyValueEnumerable_allValues);

  // Define the property.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl)
    = declareDerivedReadOnlyProperty(tc, parentDecl, enumDecl,
                                     C.Id_allValues,
                                     valueSequenceInterfaceType,
                                     valueSequenceType,
                                     getterDecl,
                                     /*isStatic=*/true,
                                     /*isFinal=*/false);

  auto dc = cast<IterableDeclContext>(parentDecl);
  dc->addMember(getterDecl);
  dc->addMember(propDecl);
  dc->addMember(pbDecl);

  return propDecl;
}

ValueDecl *DerivedConformance::deriveValueEnumerable(TypeChecker &tc,
                                                     Decl *parentDecl,
                                                     NominalTypeDecl *type,
                                                     ValueDecl *requirement) {
  // We can only synthesize ValueEnumerable for enums without associated
  // values.
  // TODO: Support values with ValueEnumerable associated values.
  if (auto enumDecl = dyn_cast<EnumDecl>(type))
    if (requirement->getBaseName() == tc.Context.Id_allValues)
      return deriveValueEnumerable_allValues(tc, parentDecl, enumDecl);

  tc.diagnose(requirement->getLoc(),
              diag::broken_raw_representable_requirement);
  return nullptr;
}

Type DerivedConformance::deriveValueEnumerable(TypeChecker &tc,
                                               Decl *parentDecl,
                                               NominalTypeDecl *type,
                                               AssociatedTypeDecl *assocType) {
  // We can only synthesize ValueEnumerable for enums without associated
  // values.
  // TODO: Support values with ValueEnumerable associated values.
  if (auto enumDecl = dyn_cast<EnumDecl>(type))
    if (assocType->getName() == tc.Context.Id_ValueSequence)
      return deriveValueEnumerable_ValueSequence(tc, parentDecl, enumDecl);

  tc.diagnose(assocType->getLoc(),
              diag::broken_raw_representable_requirement);
  return nullptr;
}
