//===--- GenInit.cpp - IR Generation for Initialization -------------------===//
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
//  This file implements IR generation for the initialization of
//  local and global variables.
//
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Pattern.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/IRGen/Linking.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

#include "DebugTypeInfo.h"
#include "GenTuple.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "FixedTypeInfo.h"
#include "Temporary.h"

using namespace swift;
using namespace irgen;

/// Emit a global variable.
void IRGenModule::emitSILGlobalVariable(SILGlobalVariable *var) {
  auto &ti = getTypeInfo(var->getLoweredType());
  
  // If the variable is empty in all resilience domains, don't actually emit it;
  // just return undef.
  if (ti.isKnownEmpty(ResilienceExpansion::Minimal)) {
    if (DebugInfo && var->getDecl()) {
      auto DbgTy = DebugTypeInfo::getGlobal(var, Int8Ty, Size(0), Alignment(1));
      DebugInfo->emitGlobalVariableDeclaration(
          nullptr, var->getDecl()->getName().str(), "", DbgTy,
          var->getLinkage() != SILLinkage::Public,
          IRGenDebugInfo::NotHeapAllocated, SILLocation(var->getDecl()));
    }
    return;
  }

  /// Create the global variable.
  getAddrOfSILGlobalVariable(var, ti,
                     var->isDefinition() ? ForDefinition : NotForDefinition);
}

StackAddress FixedTypeInfo::allocateStack(IRGenFunction &IGF, SILType T,
                                          const Twine &name) const {
  // If the type is known to be empty, don't actually allocate anything.
  if (isKnownEmpty(ResilienceExpansion::Maximal)) {
    auto addr = getUndefAddress();
    return { addr };
  }

  Address alloca =
    IGF.createAlloca(getStorageType(), getFixedAlignment(), name);
  IGF.Builder.CreateLifetimeStart(alloca, getFixedSize());
  
  return { alloca };
}

void FixedTypeInfo::destroyStack(IRGenFunction &IGF, StackAddress addr,
                                 SILType T, bool isOutlined) const {
  destroy(IGF, addr.getAddress(), T, isOutlined);
  FixedTypeInfo::deallocateStack(IGF, addr, T);
}

void FixedTypeInfo::deallocateStack(IRGenFunction &IGF, StackAddress addr,
                                    SILType T) const {
  if (isKnownEmpty(ResilienceExpansion::Maximal))
    return;
  IGF.Builder.CreateLifetimeEnd(addr.getAddress(), getFixedSize());
}

void TemporarySet::destroyAll(IRGenFunction &IGF) const {
  assert(!hasBeenCleared() && "destroying a set that's been cleared?");

  // Deallocate all the temporaries.
  for (auto &temporary : reversed(Stack)) {
    temporary.destroy(IGF);
  }
}

void Temporary::destroy(IRGenFunction &IGF) const {
  auto &ti = IGF.getTypeInfo(Type);
  ti.deallocateStack(IGF, Addr, Type);
}
