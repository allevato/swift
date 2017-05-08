//===--- DerivedConformanceEquatableHashable.cpp - Derived Equatable & co -===//
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
//  This file implements implicit derivation of the Equatable and Hashable
//  protocols. (Comparable is similar enough in spirit that it would make
//  sense to live here too when we implement its derivation.)
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "DerivedConformances.h"

using namespace swift;
using namespace DerivedConformance;

/// Common preconditions for Equatable and Hashable.
static bool canDeriveConformance(NominalTypeDecl *type,
                                 ProtocolDecl *protocol) {
  // The type must be an enum.
  // TODO: Structs with Equatable/Hashable/Comparable members
  auto enumDecl = dyn_cast<EnumDecl>(type);
  if (!enumDecl)
    return false;

  // The enum must have cases.
  if (!enumDecl->hasCases())
    return false;

  // The enum must not have associated values or only have associated values
  // that conform to the protocol.
  if (!enumDecl->allAssociatedValuesConformToProtocol(protocol))
    return false;
  
  return true;
}

/// Creates a named variable based on a prefix character and a numeric index.
/// \p prefixChar The prefix character for the variable's name.
/// \p index The numeric index to append to the variable's name.
/// \p type The type of the variable.
/// \p varContext The context of the variable.
static VarDecl *indexedVarDecl(char prefixChar, int index, Type type,
                               DeclContext *varContext) {
  ASTContext &C = varContext->getASTContext();
  
  llvm::SmallString<8> indexVal;
  indexVal.append(1, prefixChar);
  APInt(32, 0).toString(indexVal, 10, /*signed*/ false);
  auto indexStr = C.AllocateCopy(indexVal);
  auto indexStrRef = StringRef(indexStr.data(), indexStr.size());
  
  return new (C) VarDecl(/*IsStatic*/false, /*IsLet*/false,
                         /*IsCaptureList*/false, SourceLoc(),
                         C.getIdentifier(indexStrRef), type, varContext);
}

/// Returns the pattern used to match and bind the associated values (if any) of
/// an enum case.
/// \p enumElementDecl The enum element to match.
/// \p varPrefix The prefix character for variable names (e.g., a0, a1, ...).
/// \p varContext The context into which payload variables should be declared.
/// \p boundVars The array to which the pattern's variables will be appended.
static Pattern*
enumElementPayloadSubpattern(EnumElementDecl *enumElementDecl,
                             char varPrefix, DeclContext *varContext,
                             SmallVectorImpl<VarDecl*> &boundVars) {
  auto parentDC = enumElementDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();
  
  auto argumentType = enumElementDecl->getArgumentTypeLoc().getType();
  if (argumentType.isNull())
    // No arguments, so no subpattern to match.
    return nullptr;
  
  if (auto tupleType = argumentType->getAs<TupleType>()) {
    // Either multiple (labeled or unlabeled) arguments, or one labeled
    // argument. Return a tuple pattern that matches the enum element in arity,
    // types, and labels. For example:
    // case a(x: Int) => (x: let a0)
    // case b(Int, String) => (let a0, let a1)
    SmallVector<TuplePatternElt, 3> elementPatterns;
    int index = 0;
    for (auto tupleElement : tupleType->getElements()) {
      auto payloadVar = indexedVarDecl(varPrefix, index++,
                                       tupleElement.getType(), varContext);
      boundVars.push_back(payloadVar);

      auto namedPattern = new (C) NamedPattern(payloadVar);
      namedPattern->setImplicit();
      elementPatterns.push_back(TuplePatternElt(tupleElement.getName(),
                                                SourceLoc(), namedPattern));
    }

    auto pat = TuplePattern::create(C, SourceLoc(), elementPatterns,
                                    SourceLoc());
    pat->setImplicit();
    return pat;
  }

  // Otherwise, a one-argument unlabeled payload. Return a paren pattern whose
  // underlying type is the same as the payload. For example:
  // case a(Int) => (let a0)
  auto underlyingType = argumentType->getWithoutParens();
  auto payloadVar = indexedVarDecl('a', 0, underlyingType, varContext);
  boundVars.push_back(payloadVar);

  auto namedPattern = new (C) NamedPattern(payloadVar);
  namedPattern->setImplicit();

  auto pat = new (C) ParenPattern(SourceLoc(), namedPattern, SourceLoc());
  pat->setImplicit();
  return pat;
}

/// Derive the body for an '==' operator for an enum
static void deriveBodyEquatable_enum_eq(AbstractFunctionDecl *eqDecl) {
  auto parentDC = eqDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = eqDecl->getParameterLists().back();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  auto boolTy = C.getBoolDecl()->getDeclaredType();

  Type enumType = aParam->getType();
  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());

  auto andOperDecl = C.getBoolShortCircuitingAndDecl();
  assert(andOperDecl && "should have && for Bool");

  SmallVector<ASTNode, 6> statements;

  auto isEqualVar = new (C) VarDecl(/*IsStatic*/ false, /*IsLet*/ false,
                                    /*IsCaptureList*/ false, SourceLoc(),
                                    C.getIdentifier("isEqual"), boolTy,
                                    eqDecl);
  isEqualVar->setInterfaceType(boolTy);
  isEqualVar->setImplicit();
  
  // generate: var resultVar
  Pattern *resultPat = new (C) NamedPattern(isEqualVar, /*implicit*/ true);
  resultPat->setType(boolTy);
  resultPat = new (C) TypedPattern(resultPat, TypeLoc::withoutLoc(boolTy));
  resultPat->setType(boolTy);
  auto isEqualBind = PatternBindingDecl::create(C, SourceLoc(),
                                                StaticSpellingKind::None,
                                                SourceLoc(),
                                                resultPat, nullptr, eqDecl);

  SmallVector<CaseStmt*, 4> cases;
  unsigned elementCount = 0;
  unsigned discriminator = 0;

  for (auto elt : enumDecl->getAllElements()) {
    elementCount++;

    // generate: case (.<Case>(payload bindings), .<Case>(payload bindings)):
    SmallVector<VarDecl*, 3> lhsPayloadVars;
    auto lhsSubpattern = enumElementPayloadSubpattern(elt, 'l', eqDecl,
                                                      lhsPayloadVars);
    auto lhsElemPat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                                 SourceLoc(), SourceLoc(),
                                                 Identifier(), elt,
                                                 lhsSubpattern);
    lhsElemPat->setImplicit();

    SmallVector<VarDecl*, 3> rhsPayloadVars;
    auto rhsSubpattern = enumElementPayloadSubpattern(elt, 'r', eqDecl,
                                                      rhsPayloadVars);
    auto rhsElemPat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                                 SourceLoc(), SourceLoc(),
                                                 Identifier(), elt,
                                                 rhsSubpattern);
    rhsElemPat->setImplicit();

    auto caseTuplePattern = TuplePattern::create(C, SourceLoc(), {
      TuplePatternElt(lhsElemPat), TuplePatternElt(rhsElemPat) },
                                                 SourceLoc());
    caseTuplePattern->setImplicit();
    
    auto labelItem = CaseLabelItem(/*IsDefault*/ false, caseTuplePattern,
                                   SourceLoc(), nullptr);

    Expr *lastExpr = nullptr;
    if (elt->getArgumentTypeLoc().getType().isNull()) {
      // If there aren't any associated values, we just make the result true
      // because the cases matched.
      lastExpr = new (C) BooleanLiteralExpr(true, SourceLoc(),
                                            /*implicit*/ true);
    } else {
      // Generate a chain of expressions that computes the AND of each
      // associated value pair's equality test.
      for (size_t varIdx = 0; varIdx < lhsPayloadVars.size(); varIdx++) {
        auto lhsVar = lhsPayloadVars[varIdx];
        auto rhsVar = rhsPayloadVars[varIdx];
        auto lhsRef = new (C) DeclRefExpr(lhsVar, DeclNameLoc(),
                                          /*implicit*/true);
        auto rhsRef = new (C) DeclRefExpr(rhsVar, DeclNameLoc(),
                                          /*implicit*/true);

        // generate: lx == rx
        auto cmpFuncExpr = new (C) UnresolvedDeclRefExpr(
          DeclName(C.getIdentifier("==")), DeclRefKind::BinaryOperator,
          DeclNameLoc());
        auto cmpArgsType = TupleType::get(
          { TupleTypeElt(enumType), TupleTypeElt(enumType) }, C);
        TupleExpr *cmpArgsTuple = TupleExpr::create(C, SourceLoc(),
                                                    { lhsRef, rhsRef },
                                                    { }, { }, SourceLoc(),
                                                    /*HasTrailingClosure*/false,
                                                    /*Implicit*/true,
                                                    cmpArgsType);
      
        auto *cmpExpr = new (C) BinaryExpr(cmpFuncExpr, cmpArgsTuple,
                                           /*implicit*/ true, boolTy);

        if (lastExpr == nullptr) {
          lastExpr = cmpExpr;
        } else {
          // generate: <lastExpr> && @autoclosure { lx == rx }
          auto andClosureExpr = new (C) AutoClosureExpr(cmpExpr, boolTy,
                                                        discriminator++,
                                                        parentDC);
          auto fnType = cast<FunctionType>(andOperDecl->getInterfaceType()
              ->getCanonicalType());
        
          auto contextTy = andOperDecl->getDeclContext()->getSelfInterfaceType();
          Expr *base = TypeExpr::createImplicitHack(SourceLoc(), contextTy, C);
          Expr *ref = new (C) DeclRefExpr(andOperDecl, DeclNameLoc(),
                                          /*Implicit*/ true,
                                          AccessSemantics::Ordinary,
                                          fnType);
          
          fnType = cast<FunctionType>(fnType.getResult());
          auto andOperExpr = new (C) DotSyntaxCallExpr(ref, SourceLoc(), base,
                                                       fnType);
          andOperExpr->setImplicit();
        
          auto tType = fnType.getInput();
          TupleExpr *abTuple = TupleExpr::create(C, SourceLoc(),
                                                 { lastExpr, andClosureExpr },
                                                 { }, { }, SourceLoc(),
                                                 /*HasTrailingClosure*/ false,
                                                 /*Implicit*/ true, tType);
        
          lastExpr = new (C) BinaryExpr(andOperExpr, abTuple,
                                        /*implicit*/ true, boolTy);

        }
      }
    }
    
    // generate: result = <lastExpr>
    auto isEqualRef = new (C) DeclRefExpr(isEqualVar, DeclNameLoc(),
                                         /*implicit*/ true);
    auto assignExpr = new (C) AssignExpr(isEqualRef, SourceLoc(),
                                         lastExpr, /*implicit*/ true);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, SourceLoc(), labelItem,
                                     /*HasBoundDecls*/ false,
                                     SourceLoc(), body));
  }

  // generate: default: result = false
  // We only generate this if the enum has more than one case. If it has exactly
  // one case, then the equality test is always exhaustive (because we generate
  // case statements for the same-case pairs).
  if (elementCount > 1) {
    auto defaultPattern = new (C) AnyPattern(SourceLoc());
    defaultPattern->setImplicit();
    auto defaultItem = CaseLabelItem(/*IsDefault*/ true, defaultPattern,
                                     SourceLoc(), nullptr);
    auto falseExpr = new (C) BooleanLiteralExpr(false, SourceLoc(),
                                                /*implicit*/ true);
    auto isEqualRef = new (C) DeclRefExpr(isEqualVar, DeclNameLoc(),
                                          /*implicit*/ true);
    auto assignExpr = new (C) AssignExpr(isEqualRef, SourceLoc(),
                                         falseExpr, /*implicit*/ true);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, SourceLoc(), defaultItem,
                                     /*HasBoundDecls*/ false,
                                     SourceLoc(), body));
  }

  // generate: switch (a, b) { }
  auto aRef = new (C) DeclRefExpr(aParam, DeclNameLoc(), /*implicit*/true);
  auto bRef = new (C) DeclRefExpr(bParam, DeclNameLoc(), /*implicit*/true);
  auto abExpr = TupleExpr::create(C, SourceLoc(), { aRef, bRef }, {}, {},
                                  SourceLoc(), /*HasTrailingClosure*/ false,
                                  /*implicit*/ true);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), abExpr,
                                       SourceLoc(), cases, SourceLoc(), C);
  
  statements.push_back(isEqualBind);
  statements.push_back(switchStmt);
  
  // generate: return result
  auto isEqualRef = new (C) DeclRefExpr(isEqualVar, DeclNameLoc(),
                                       /*implicit*/ true,
                                       AccessSemantics::Ordinary, boolTy);
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), isEqualRef);
  statements.push_back(returnStmt);
  
  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  eqDecl->setBody(body);
}

/// Derive an '==' operator implementation for an enum.
static ValueDecl *
deriveEquatable_enum_eq(TypeChecker &tc, Decl *parentDecl, EnumDecl *enumDecl) {
  // enum SomeEnum<T...> {
  //   case A, B(Int), C(String, Int)
  //
  //   @derived
  //   @_implements(Equatable, ==(_:_:))
  //   func __derived_enum_equals(a: SomeEnum<T...>,
  //                              b: SomeEnum<T...>) -> Bool {
  //     var isEqual: Bool
  //     switch (a, b) {
  //     case (.A, .A):
  //       isEqual = true
  //     case (.B(let l0), .B(let r0)):
  //       isEqual = l0 == r0
  //     case (.C(let l0, let l1), .C(let r0, let r1)):
  //       isEqual = l0 == r0 && l1 == r1
  //     default: isEqual = false
  //     }
  //     return isEqual
  //   }
  
  ASTContext &C = tc.Context;
  
  auto parentDC = cast<DeclContext>(parentDecl);
  auto enumTy = parentDC->getDeclaredTypeInContext();
  auto enumIfaceTy = parentDC->getDeclaredInterfaceType();

  auto getParamDecl = [&](StringRef s) -> ParamDecl* {
    auto *param = new (C) ParamDecl(/*isLet*/true, SourceLoc(), SourceLoc(),
                                    Identifier(), SourceLoc(), C.getIdentifier(s),
                                    enumTy, parentDC);
    param->setInterfaceType(enumIfaceTy);
    return param;
  };

  auto selfDecl = ParamDecl::createSelf(SourceLoc(), parentDC,
                                        /*isStatic=*/true);
  
  ParameterList *params[] = {
    ParameterList::createWithoutLoc(selfDecl),
    ParameterList::create(C, {
        getParamDecl("a"),
        getParamDecl("b")
    })
  };

  auto boolTy = C.getBoolDecl()->getDeclaredType();

  DeclName name(C, C.Id_derived_enum_equals, params[1]);
  auto eqDecl =
    FuncDecl::create(C, /*StaticLoc=*/SourceLoc(),
                     StaticSpellingKind::KeywordStatic,
                     /*FuncLoc=*/SourceLoc(), name, /*NameLoc=*/SourceLoc(),
                     /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                     /*AccessorKeywordLoc=*/SourceLoc(),
                     /*GenericParams=*/nullptr,
                     params,
                     TypeLoc::withoutLoc(boolTy),
                     parentDC);
  eqDecl->setImplicit();
  eqDecl->setUserAccessible(false);
  eqDecl->getAttrs().add(new (C) InfixAttr(/*implicit*/false));

  // Add the @_implements(Equatable, ==(_:_:)) attribute
  auto equatableProto = C.getProtocol(KnownProtocolKind::Equatable);
  auto equatableTy = equatableProto->getDeclaredType();
  auto equatableTypeLoc = TypeLoc::withoutLoc(equatableTy);
  SmallVector<Identifier, 2> argumentLabels = { Identifier(), Identifier() };
  auto equalsDeclName = DeclName(C, DeclBaseName(C.Id_EqualsOperator),
                                 argumentLabels);
  eqDecl->getAttrs().add(new (C) ImplementsAttr(SourceLoc(),
                                                SourceRange(),
                                                equatableTypeLoc,
                                                equalsDeclName,
                                                DeclNameLoc()));

  if (!C.getEqualIntDecl()) {
    tc.diagnose(parentDecl->getLoc(), diag::no_equal_overload_for_int);
    return nullptr;
  }

  eqDecl->setBodySynthesizer(&deriveBodyEquatable_enum_eq);

  // Compute the type.
  Type paramsTy = params[1]->getType(tc.Context);

  // Compute the interface type.
  Type interfaceTy;
  Type selfIfaceTy = eqDecl->computeInterfaceSelfType();
  if (auto genericSig = parentDC->getGenericSignatureOfContext()) {
    eqDecl->setGenericEnvironment(parentDC->getGenericEnvironmentOfContext());

    Type enumIfaceTy = parentDC->getDeclaredInterfaceType();
    TupleTypeElt ifaceParamElts[] = {
      enumIfaceTy, enumIfaceTy,
    };
    auto ifaceParamsTy = TupleType::get(ifaceParamElts, C);
    interfaceTy = FunctionType::get(ifaceParamsTy, boolTy,
                                    AnyFunctionType::ExtInfo());
    interfaceTy = GenericFunctionType::get(genericSig, selfIfaceTy, interfaceTy,
                                           AnyFunctionType::ExtInfo());
  } else {
    interfaceTy = FunctionType::get(paramsTy, boolTy);
    interfaceTy = FunctionType::get(selfIfaceTy, interfaceTy);
  }
  eqDecl->setInterfaceType(interfaceTy);

  // Since we can't insert the == operator into the same FileUnit as the enum,
  // itself, we have to give it at least internal access.
  eqDecl->setAccessibility(std::max(enumDecl->getFormalAccess(),
                                    Accessibility::Internal));

  // If the enum was not imported, the derived conformance is either from the
  // enum itself or an extension, in which case we will emit the declaration
  // normally.
  if (enumDecl->hasClangNode())
    tc.Context.addExternalDecl(eqDecl);
  
  // Add the operator to the parent scope.
  cast<IterableDeclContext>(parentDecl)->addMember(eqDecl);

  return eqDecl;
}

ValueDecl *DerivedConformance::deriveEquatable(TypeChecker &tc,
                                               Decl *parentDecl,
                                               NominalTypeDecl *type,
                                               ValueDecl *requirement) {
  // Check that we can actually derive Equatable for this type.
  auto equatable = tc.getProtocol(type->getLoc(),
                                  KnownProtocolKind::Equatable);
  if (!canDeriveConformance(type, equatable))
    return nullptr;

  // Build the necessary decl.
  if (requirement->getName().str() == "==") {
    if (auto theEnum = dyn_cast<EnumDecl>(type))
      return deriveEquatable_enum_eq(tc, parentDecl, theEnum);
    else
      llvm_unreachable("todo");
  }
  tc.diagnose(requirement->getLoc(),
              diag::broken_equatable_requirement);
  return nullptr;
}

/// Returns a new expression that mixes the hash value of one expression into
/// another expression.
/// \p C The AST context.
/// \p exprSoFar The hash value expression so far.
/// \p exprToHash The expression whose hash value should be mixed in.
static Expr*
mixInHashExpr_hashValue(ASTContext &C, Expr* exprSoFar, Expr *exprToHash) {
  auto intType = C.getIntDecl()->getDeclaredType();
  auto binaryArithmeticInputType =
      TupleType::get({ TupleTypeElt(intType), TupleTypeElt(intType) }, C);

  // generate: 31 (the hashing multiplier)
  llvm::SmallString<8> multiplierVal;
  APInt(32, 31).toString(multiplierVal, 10, /*signed*/ false);
  auto multiplierStr = C.AllocateCopy(multiplierVal);
  auto multiplierExpr = new (C) IntegerLiteralExpr(
      StringRef(multiplierStr.data(), multiplierStr.size()), SourceLoc(),
      /*implicit*/ true);

  // generate: 31 &* <exprSoFar>
  auto multiplyFunc = C.getOverflowingIntegerMultiplyDecl();
  auto multiplyFuncExpr = new (C) DeclRefExpr(multiplyFunc, DeclNameLoc(),
                                              /*implicit*/ true);
  auto multiplyArgTuple = TupleExpr::create(C, SourceLoc(),
                                            { multiplierExpr, exprSoFar },
                                            { }, { }, SourceLoc(),
                                            /*HasTrailingClosure*/ false,
                                            /*Implicit*/ true,
                                            binaryArithmeticInputType);
  auto productExpr = new (C) BinaryExpr(multiplyFuncExpr, multiplyArgTuple,
                                        /*implicit*/ true);

  // generate: <exprToHash>.hashValue
  auto hashValueExpr = new (C) UnresolvedDotExpr(exprToHash, SourceLoc(),
                                                 C.Id_hashValue, DeclNameLoc(),
                                                 /*implicit*/ true);

  // generate the result: <exprToHash>.hashValue &+ 31 &* <exprSoFar>
  auto addFunc = C.getOverflowingIntegerAddDecl();
  auto addFuncExpr = new (C) DeclRefExpr(addFunc, DeclNameLoc(),
                                         /*implicit*/ true);
  TupleExpr *addArgTuple = TupleExpr::create(C, SourceLoc(),
                                             { hashValueExpr, productExpr },
                                             { }, { }, SourceLoc(),
                                             /*HasTrailingClosure*/ false,
                                             /*Implicit*/ true,
                                             binaryArithmeticInputType);
  return new (C) BinaryExpr(addFuncExpr, addArgTuple, /*implicit*/ true);
}

static void
deriveBodyHashable_enum_hashValue(AbstractFunctionDecl *hashValueDecl) {
  auto parentDC = hashValueDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto enumDecl = parentDC->getAsEnumOrEnumExtensionContext();
  SmallVector<ASTNode, 3> statements;
  auto selfDecl = hashValueDecl->getImplicitSelfDecl();

  Type enumType = selfDecl->getType();
  Type intType = C.getIntDecl()->getDeclaredType();

  auto resultVar = new (C) VarDecl(/*IsStatic*/ false, /*IsLet*/ false,
                                   /*IsCaptureList*/ false, SourceLoc(),
                                   C.getIdentifier("result"), intType,
                                   hashValueDecl);
  resultVar->setInterfaceType(intType);
  resultVar->setImplicit();
  
  // generate: var resultVar
  Pattern *resultPat = new (C) NamedPattern(resultVar, /*implicit*/ true);
  resultPat->setType(intType);
  resultPat = new (C) TypedPattern(resultPat, TypeLoc::withoutLoc(intType));
  resultPat->setType(intType);
  auto resultBind = PatternBindingDecl::create(C, SourceLoc(),
                                               StaticSpellingKind::None,
                                               SourceLoc(),
                                               resultPat, nullptr,
                                               hashValueDecl);

  unsigned index = 0;
  SmallVector<CaseStmt*, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    // generate: case .<Case>(payload bindings):
    SmallVector<VarDecl*, 3> payloadVars;
    auto payloadPattern = enumElementPayloadSubpattern(elt, 'a', hashValueDecl,
                                                       payloadVars);
    auto pat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                          SourceLoc(), SourceLoc(),
                                          Identifier(), elt, payloadPattern);
    pat->setImplicit();

    auto labelItem = CaseLabelItem(/*IsDefault*/ false, pat, SourceLoc(),
                                   nullptr);

    // generate: <index>, the first term of the hash function.
    llvm::SmallString<8> indexVal;
    APInt(32, index++).toString(indexVal, 10, /*signed*/ false);
    auto indexStr = C.AllocateCopy(indexVal);
    Expr *lastExpr = new (C) IntegerLiteralExpr(StringRef(indexStr.data(),
                                                          indexStr.size()),
                                                SourceLoc(), /*implicit*/ true);

    // Generate a chain of expressions that mix the payload's hash values.
    for (auto payloadVar : payloadVars) {
      auto payloadVarRef = new (C) DeclRefExpr(payloadVar, DeclNameLoc(),
                                               /*implicit*/ true);
      lastExpr = mixInHashExpr_hashValue(C, lastExpr, payloadVarRef);
    }

    // generate: result = <lastExpr>
    auto resultRef = new (C) DeclRefExpr(resultVar, DeclNameLoc(),
                                         /*implicit*/ true);
    auto assignExpr = new (C) AssignExpr(resultRef, SourceLoc(),
                                         lastExpr, /*implicit*/ true);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, SourceLoc(), labelItem,
                                     /*HasBoundDecls*/ false,
                                     SourceLoc(), body));
  }

  // generate: switch enumVar { }
  auto enumRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                     /*implicit*/true);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), enumRef,
                                       SourceLoc(), cases, SourceLoc(), C);

  statements.push_back(resultBind);
  statements.push_back(switchStmt);

  // generate: return result
  auto resultRef = new (C) DeclRefExpr(resultVar, DeclNameLoc(),
                                       /*implicit*/ true,
                                       AccessSemantics::Ordinary, intType);
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), resultRef);
  statements.push_back(returnStmt);

  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  hashValueDecl->setBody(body);
}

/// Derive a 'hashValue' implementation for an enum.
static ValueDecl *
deriveHashable_enum_hashValue(TypeChecker &tc, Decl *parentDecl,
                              EnumDecl *enumDecl) {
  // enum SomeEnum {
  //   case A, B(Int), C(String, Int)
  //   @derived var hashValue: Int {
  //     var result: Int
  //     switch self {
  //     case A:
  //       result = 0.hashValue
  //     case B(let a0):
  //       result = 1.hashValue &+ 31 &* a0.hashValue
  //     case C(let a0, let a1):
  //       result = 2.hashValue &+ 31 * (a0.hashValue &+ 31 &* a1.hashValue)
  //     }
  //     return result
  //   }
  // }
  ASTContext &C = tc.Context;
  
  auto parentDC = cast<DeclContext>(parentDecl);
  Type intType = C.getIntDecl()->getDeclaredType();
  
  // We can't form a Hashable conformance if Int isn't Hashable or
  // ExpressibleByIntegerLiteral.
  if (!tc.conformsToProtocol(intType,C.getProtocol(KnownProtocolKind::Hashable),
                             enumDecl, None)) {
    tc.diagnose(enumDecl->getLoc(), diag::broken_int_hashable_conformance);
    return nullptr;
  }

  ProtocolDecl *intLiteralProto =
      C.getProtocol(KnownProtocolKind::ExpressibleByIntegerLiteral);
  if (!tc.conformsToProtocol(intType, intLiteralProto, enumDecl, None)) {
    tc.diagnose(enumDecl->getLoc(),
                diag::broken_int_integer_literal_convertible_conformance);
    return nullptr;
  }
  
  auto selfDecl = ParamDecl::createSelf(SourceLoc(), parentDC);
  
  ParameterList *params[] = {
    ParameterList::createWithoutLoc(selfDecl),
    ParameterList::createEmpty(C)
  };
  
  FuncDecl *getterDecl =
      FuncDecl::create(C, /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
                       /*FuncLoc=*/SourceLoc(),
                       Identifier(), /*NameLoc=*/SourceLoc(),
                       /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                       /*AccessorKeywordLoc=*/SourceLoc(),
                       /*GenericParams=*/nullptr, params,
                       TypeLoc::withoutLoc(intType), parentDC);
  getterDecl->setImplicit();
  getterDecl->setBodySynthesizer(deriveBodyHashable_enum_hashValue);

  // Compute the type of hashValue().
  Type methodType = FunctionType::get(TupleType::getEmpty(tc.Context), intType);

  // Compute the interface type of hashValue().
  Type interfaceType;
  Type selfIfaceType = getterDecl->computeInterfaceSelfType();
  if (auto sig = parentDC->getGenericSignatureOfContext()) {
    getterDecl->setGenericEnvironment(parentDC->getGenericEnvironmentOfContext());
    interfaceType = GenericFunctionType::get(sig, selfIfaceType, methodType,
                                             AnyFunctionType::ExtInfo());
  } else
    interfaceType = FunctionType::get(selfIfaceType, methodType);
  
  getterDecl->setInterfaceType(interfaceType);
  getterDecl->setAccessibility(std::max(Accessibility::Internal,
                                        enumDecl->getFormalAccess()));

  // If the enum was not imported, the derived conformance is either from the
  // enum itself or an extension, in which case we will emit the declaration
  // normally.
  if (enumDecl->hasClangNode())
    tc.Context.addExternalDecl(getterDecl);

  // Create the property.
  VarDecl *hashValueDecl = new (C) VarDecl(/*IsStatic*/false, /*IsLet*/false,
                                           /*IsCaptureList*/false, SourceLoc(),
                                           C.Id_hashValue, intType, parentDC);
  hashValueDecl->setImplicit();
  hashValueDecl->setInterfaceType(intType);
  hashValueDecl->makeComputed(SourceLoc(), getterDecl,
                              nullptr, nullptr, SourceLoc());
  hashValueDecl->setAccessibility(getterDecl->getFormalAccess());

  Pattern *hashValuePat = new (C) NamedPattern(hashValueDecl, /*implicit*/true);
  hashValuePat->setType(intType);
  hashValuePat
    = new (C) TypedPattern(hashValuePat, TypeLoc::withoutLoc(intType),
                           /*implicit*/ true);
  hashValuePat->setType(intType);

  auto patDecl = PatternBindingDecl::create(C, SourceLoc(),
                                            StaticSpellingKind::None,
                                            SourceLoc(), hashValuePat, nullptr,
                                            parentDC);
  patDecl->setImplicit();

  auto dc = cast<IterableDeclContext>(parentDecl);
  dc->addMember(getterDecl);
  dc->addMember(hashValueDecl);
  dc->addMember(patDecl);
  return hashValueDecl;
}

ValueDecl *DerivedConformance::deriveHashable(TypeChecker &tc,
                                              Decl *parentDecl,
                                              NominalTypeDecl *type,
                                              ValueDecl *requirement) {
  // Check that we can actually derive Hashable for this type.
  auto hashable = tc.getProtocol(type->getLoc(),
                                 KnownProtocolKind::Hashable);
  if (!canDeriveConformance(type, hashable))
    return nullptr;
  
  // Build the necessary decl.
  if (requirement->getName().str() == "hashValue") {
    if (auto theEnum = dyn_cast<EnumDecl>(type))
      return deriveHashable_enum_hashValue(tc, parentDecl, theEnum);
    else
      llvm_unreachable("todo");
  }
  tc.diagnose(requirement->getLoc(),
              diag::broken_hashable_requirement);
  return nullptr;
}
