//===- CIRGenFunction.cpp - Emit CIR from ASTs for a Function -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This coordinates the per-function state used while generating code
//
//===----------------------------------------------------------------------===//

#include "CIRGenFunction.h"
#include "CIRGenCXXABI.h"
#include "CIRGenModule.h"
#include "CIRGenOpenMPRuntime.h"
#include "clang/AST/Attrs.inc"
#include "clang/Basic/CodeGenOptions.h"
#include "clang/CIR/MissingFeatures.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/ExprObjC.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/DiagnosticCategories.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/FPEnv.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "llvm/ADT/PointerIntPair.h"

#include "CIRGenTBAA.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Support/LogicalResult.h"

using namespace clang;
using namespace clang::CIRGen;
using namespace cir;

CIRGenFunction::CIRGenFunction(CIRGenModule &CGM, CIRGenBuilderTy &builder,
                               bool suppressNewContext)
    : CIRGenTypeCache(CGM), CGM{CGM}, builder(builder),
      SanOpts(CGM.getLangOpts().Sanitize), CurFPFeatures(CGM.getLangOpts()),
      ShouldEmitLifetimeMarkers(false) {
  if (!suppressNewContext)
    CGM.getCXXABI().getMangleContext().startNewFunction();
  EHStack.setCGF(this);

  // TODO(CIR): SetFastMathFlags(CurFPFeatures);
}

CIRGenFunction::~CIRGenFunction() {
  assert(LifetimeExtendedCleanupStack.empty() && "failed to emit a cleanup");
  assert(DeferredDeactivationCleanupStack.empty() &&
         "missed to deactivate a cleanup");

  // TODO(cir): set function is finished.
  assert(!cir::MissingFeatures::openMPRuntime());

  // If we have an OpenMPIRBuilder we want to finalize functions (incl.
  // outlining etc) at some point. Doing it once the function codegen is done
  // seems to be a reasonable spot. We do it here, as opposed to the deletion
  // time of the CodeGenModule, because we have to ensure the IR has not yet
  // been "emitted" to the outside, thus, modifications are still sensible.
  assert(!cir::MissingFeatures::openMPRuntime());
}

clang::ASTContext &CIRGenFunction::getContext() const {
  return CGM.getASTContext();
}

cir::TypeEvaluationKind CIRGenFunction::getEvaluationKind(QualType type) {
  type = type.getCanonicalType();
  while (true) {
    switch (type->getTypeClass()) {
#define TYPE(name, parent)
#define ABSTRACT_TYPE(name, parent)
#define NON_CANONICAL_TYPE(name, parent) case Type::name:
#define DEPENDENT_TYPE(name, parent) case Type::name:
#define NON_CANONICAL_UNLESS_DEPENDENT_TYPE(name, parent) case Type::name:
#include "clang/AST/TypeNodes.inc"
      llvm_unreachable("non-canonical or dependent type in IR-generation");

    case Type::ArrayParameter:
    case Type::HLSLAttributedResource:
      llvm_unreachable("NYI");

    case Type::Auto:
    case Type::DeducedTemplateSpecialization:
      llvm_unreachable("undeduced type in IR-generation");

    // Various scalar types.
    case Type::Builtin:
    case Type::Pointer:
    case Type::BlockPointer:
    case Type::LValueReference:
    case Type::RValueReference:
    case Type::MemberPointer:
    case Type::Vector:
    case Type::ExtVector:
    case Type::ConstantMatrix:
    case Type::FunctionProto:
    case Type::FunctionNoProto:
    case Type::Enum:
    case Type::ObjCObjectPointer:
    case Type::Pipe:
    case Type::BitInt:
      return cir::TEK_Scalar;

    // Complexes.
    case Type::Complex:
      return cir::TEK_Complex;

    // Arrays, records, and Objective-C objects.
    case Type::ConstantArray:
    case Type::IncompleteArray:
    case Type::VariableArray:
    case Type::Record:
    case Type::ObjCObject:
    case Type::ObjCInterface:
      return cir::TEK_Aggregate;

    // We operate on atomic values according to their underlying type.
    case Type::Atomic:
      type = cast<AtomicType>(type)->getValueType();
      continue;
    }
    llvm_unreachable("unknown type kind!");
  }
}

mlir::Type CIRGenFunction::convertTypeForMem(QualType T) {
  return CGM.getTypes().convertTypeForMem(T);
}

mlir::Type CIRGenFunction::convertType(QualType T) {
  return CGM.getTypes().convertType(T);
}

mlir::Location CIRGenFunction::getLoc(SourceLocation SLoc) {
  // Some AST nodes might contain invalid source locations (e.g.
  // CXXDefaultArgExpr), workaround that to still get something out.
  if (SLoc.isValid()) {
    const SourceManager &SM = getContext().getSourceManager();
    PresumedLoc PLoc = SM.getPresumedLoc(SLoc);
    StringRef Filename = PLoc.getFilename();
    return mlir::FileLineColLoc::get(builder.getStringAttr(Filename),
                                     PLoc.getLine(), PLoc.getColumn());
  } else {
    // Do our best...
    assert(currSrcLoc && "expected to inherit some source location");
    return *currSrcLoc;
  }
}

mlir::Location CIRGenFunction::getLoc(SourceRange SLoc) {
  // Some AST nodes might contain invalid source locations (e.g.
  // CXXDefaultArgExpr), workaround that to still get something out.
  if (SLoc.isValid()) {
    mlir::Location B = getLoc(SLoc.getBegin());
    mlir::Location E = getLoc(SLoc.getEnd());
    SmallVector<mlir::Location, 2> locs = {B, E};
    mlir::Attribute metadata;
    return mlir::FusedLoc::get(locs, metadata, &getMLIRContext());
  } else if (currSrcLoc) {
    return *currSrcLoc;
  }

  // We're brave, but time to give up.
  return builder.getUnknownLoc();
}

mlir::Location CIRGenFunction::getLoc(mlir::Location lhs, mlir::Location rhs) {
  SmallVector<mlir::Location, 2> locs = {lhs, rhs};
  mlir::Attribute metadata;
  return mlir::FusedLoc::get(locs, metadata, &getMLIRContext());
}

/// Return true if the statement contains a label in it.  If
/// this statement is not executed normally, it not containing a label means
/// that we can just remove the code.
bool CIRGenFunction::ContainsLabel(const Stmt *S, bool IgnoreCaseStmts) {
  // Null statement, not a label!
  if (!S)
    return false;

  // If this is a label, we have to emit the code, consider something like:
  // if (0) {  ...  foo:  bar(); }  goto foo;
  //
  // TODO: If anyone cared, we could track __label__'s, since we know that you
  // can't jump to one from outside their declared region.
  if (isa<LabelStmt>(S))
    return true;

  // If this is a case/default statement, and we haven't seen a switch, we
  // have to emit the code.
  if (isa<SwitchCase>(S) && !IgnoreCaseStmts)
    return true;

  // If this is a switch statement, we want to ignore cases below it.
  if (isa<SwitchStmt>(S))
    IgnoreCaseStmts = true;

  // Scan subexpressions for verboten labels.
  for (const Stmt *SubStmt : S->children())
    if (ContainsLabel(SubStmt, IgnoreCaseStmts))
      return true;

  return false;
}

bool CIRGenFunction::sanitizePerformTypeCheck() const {
  return SanOpts.has(SanitizerKind::Null) ||
         SanOpts.has(SanitizerKind::Alignment) ||
         SanOpts.has(SanitizerKind::ObjectSize) ||
         SanOpts.has(SanitizerKind::Vptr);
}

void CIRGenFunction::emitTypeCheck(TypeCheckKind TCK, clang::SourceLocation Loc,
                                   mlir::Value V, clang::QualType Type,
                                   clang::CharUnits Alignment,
                                   clang::SanitizerSet SkippedChecks,
                                   std::optional<mlir::Value> ArraySize) {
  if (!sanitizePerformTypeCheck())
    return;

  assert(false && "type check NYI");
}

/// If the specified expression does not fold
/// to a constant, or if it does but contains a label, return false.  If it
/// constant folds return true and set the folded value.
bool CIRGenFunction::ConstantFoldsToSimpleInteger(const Expr *Cond,
                                                  llvm::APSInt &ResultInt,
                                                  bool AllowLabels) {
  // FIXME: Rename and handle conversion of other evaluatable things
  // to bool.
  Expr::EvalResult Result;
  if (!Cond->EvaluateAsInt(Result, getContext()))
    return false; // Not foldable, not integer or not fully evaluatable.

  llvm::APSInt Int = Result.Val.getInt();
  if (!AllowLabels && ContainsLabel(Cond))
    return false; // Contains a label.

  ResultInt = Int;
  return true;
}

/// Determine whether the function F ends with a return stmt.
static bool endsWithReturn(const Decl *F) {
  const Stmt *Body = nullptr;
  if (auto *FD = dyn_cast_or_null<FunctionDecl>(F))
    Body = FD->getBody();
  else if (auto *OMD = dyn_cast_or_null<ObjCMethodDecl>(F))
    llvm_unreachable("NYI");

  if (auto *CS = dyn_cast_or_null<CompoundStmt>(Body)) {
    auto LastStmt = CS->body_rbegin();
    if (LastStmt != CS->body_rend())
      return isa<ReturnStmt>(*LastStmt);
  }
  return false;
}

void CIRGenFunction::emitAndUpdateRetAlloca(QualType ty, mlir::Location loc,
                                            CharUnits alignment) {

  if (ty->isVoidType()) {
    // Void type; nothing to return.
    ReturnValue = Address::invalid();

    // Count the implicit return.
    if (!endsWithReturn(CurFuncDecl))
      ++NumReturnExprs;
  } else if (CurFnInfo->getReturnInfo().getKind() ==
             cir::ABIArgInfo::Indirect) {
    // TODO(CIR): Consider this implementation in CIRtoLLVM
    llvm_unreachable("NYI");
    // TODO(CIR): Consider this implementation in CIRtoLLVM
  } else if (CurFnInfo->getReturnInfo().getKind() ==
             cir::ABIArgInfo::InAlloca) {
    llvm_unreachable("NYI");
  } else {
    auto addr = emitAlloca("__retval", ty, loc, alignment);
    FnRetAlloca = addr;
    ReturnValue = Address(addr, alignment);

    // Tell the epilog emitter to autorelease the result. We do this now so
    // that various specialized functions can suppress it during their IR -
    // generation
    if (getLangOpts().ObjCAutoRefCount)
      llvm_unreachable("NYI");
  }
}

mlir::LogicalResult CIRGenFunction::declare(const Decl *var, QualType ty,
                                            mlir::Location loc,
                                            CharUnits alignment,
                                            mlir::Value &addr, bool isParam) {
  const auto *namedVar = dyn_cast_or_null<NamedDecl>(var);
  assert(namedVar && "Needs a named decl");
  assert(!symbolTable.count(var) && "not supposed to be available just yet");

  addr = emitAlloca(namedVar->getName(), ty, loc, alignment);
  auto allocaOp = cast<cir::AllocaOp>(addr.getDefiningOp());
  if (isParam)
    allocaOp.setInitAttr(mlir::UnitAttr::get(&getMLIRContext()));
  if (ty->isReferenceType() || ty.isConstQualified())
    allocaOp.setConstantAttr(mlir::UnitAttr::get(&getMLIRContext()));

  symbolTable.insert(var, addr);
  return mlir::success();
}

mlir::LogicalResult CIRGenFunction::declare(Address addr, const Decl *var,
                                            QualType ty, mlir::Location loc,
                                            CharUnits alignment,
                                            mlir::Value &addrVal,
                                            bool isParam) {
  const auto *namedVar = dyn_cast_or_null<NamedDecl>(var);
  assert(namedVar && "Needs a named decl");
  assert(!symbolTable.count(var) && "not supposed to be available just yet");

  addrVal = addr.getPointer();
  auto allocaOp = cast<cir::AllocaOp>(addrVal.getDefiningOp());
  if (isParam)
    allocaOp.setInitAttr(mlir::UnitAttr::get(&getMLIRContext()));
  if (ty->isReferenceType() || ty.isConstQualified())
    allocaOp.setConstantAttr(mlir::UnitAttr::get(&getMLIRContext()));

  symbolTable.insert(var, addrVal);
  return mlir::success();
}

/// All scope related cleanup needed:
/// - Patching up unsolved goto's.
/// - Build all cleanup code and insert yield/returns.
void CIRGenFunction::LexicalScope::cleanup() {
  auto &builder = CGF.builder;
  auto *localScope = CGF.currLexScope;

  auto applyCleanup = [&]() {
    if (PerformCleanup) {
      // ApplyDebugLocation
      assert(!cir::MissingFeatures::generateDebugInfo());
      ForceCleanup();
    }
  };

  // Cleanup are done right before codegen resume a scope. This is where
  // objects are destroyed.
  SmallVector<mlir::Block *> retBlocks;
  for (auto *retBlock : localScope->getRetBlocks()) {
    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(retBlock);
    retBlocks.push_back(retBlock);
    mlir::Location retLoc = localScope->getRetLoc(retBlock);
    (void)emitReturn(retLoc);
  }

  auto removeUnusedRetBlocks = [&]() {
    for (mlir::Block *retBlock : retBlocks) {
      if (!retBlock->getUses().empty())
        continue;
      retBlock->erase();
    }
  };

  auto insertCleanupAndLeave = [&](mlir::Block *InsPt) {
    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(InsPt);

    // If we still don't have a cleanup block, it means that `applyCleanup`
    // below might be able to get us one.
    mlir::Block *cleanupBlock = localScope->getCleanupBlock(builder);

    // Leverage and defers to RunCleanupsScope's dtor and scope handling.
    applyCleanup();

    // If we now have one after `applyCleanup`, hook it up properly.
    if (!cleanupBlock && localScope->getCleanupBlock(builder)) {
      cleanupBlock = localScope->getCleanupBlock(builder);
      builder.create<BrOp>(InsPt->back().getLoc(), cleanupBlock);
      if (!cleanupBlock->mightHaveTerminator()) {
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToEnd(cleanupBlock);
        builder.create<YieldOp>(localScope->EndLoc);
      }
    }

    if (localScope->Depth == 0) {
      // TODO(cir): get rid of all this special cases once cleanups are properly
      // implemented.
      // TODO(cir): most of this code should move into emitBranchThroughCleanup
      if (localScope->getRetBlocks().size() == 1) {
        mlir::Block *retBlock = localScope->getRetBlocks()[0];
        mlir::Location loc = localScope->getRetLoc(retBlock);
        if (retBlock->getUses().empty())
          retBlock->erase();
        else {
          // Thread return block via cleanup block.
          if (cleanupBlock) {
            for (auto &blockUse : retBlock->getUses()) {
              auto brOp = dyn_cast<cir::BrOp>(blockUse.getOwner());
              brOp.setSuccessor(cleanupBlock);
            }
          }
          builder.create<BrOp>(loc, retBlock);
          return;
        }
      }
      emitImplicitReturn();
      return;
    }

    // End of any local scope != function
    // Ternary ops have to deal with matching arms for yielding types
    // and do return a value, it must do its own cir.yield insertion.
    if (!localScope->isTernary() && !InsPt->mightHaveTerminator()) {
      !retVal ? builder.create<YieldOp>(localScope->EndLoc)
              : builder.create<YieldOp>(localScope->EndLoc, retVal);
    }
  };

  // If a cleanup block has been created at some point, branch to it
  // and set the insertion point to continue at the cleanup block.
  // Terminators are then inserted either in the cleanup block or
  // inline in this current block.
  auto *cleanupBlock = localScope->getCleanupBlock(builder);
  if (cleanupBlock)
    insertCleanupAndLeave(cleanupBlock);

  // Now deal with any pending block wrap up like implicit end of
  // scope.

  // If a terminator is already present in the current block, nothing
  // else to do here.
  auto *currBlock = builder.getBlock();
  if (isGlobalInit() && !currBlock)
    return;
  if (currBlock->mightHaveTerminator() && currBlock->getTerminator())
    return;

  // An empty non-entry block has nothing to offer, and since this is
  // synthetic, losing information does not affect anything.
  bool entryBlock = builder.getInsertionBlock()->isEntryBlock();
  if (!entryBlock && currBlock->empty()) {
    currBlock->erase();
    // Remove unused cleanup blocks.
    if (cleanupBlock && cleanupBlock->hasNoPredecessors())
      cleanupBlock->erase();
    // FIXME(cir): ideally we should call applyCleanup() before we
    // get into this condition and emit the proper cleanup. This is
    // needed to get nrvo to interop with dtor logic.
    PerformCleanup = false;
    removeUnusedRetBlocks();
    return;
  }

  // If there's a cleanup block, branch to it, nothing else to do.
  if (cleanupBlock) {
    builder.create<BrOp>(currBlock->back().getLoc(), cleanupBlock);
    return;
  }

  // No pre-existent cleanup block, emit cleanup code and yield/return.
  insertCleanupAndLeave(currBlock);
}

cir::ReturnOp CIRGenFunction::LexicalScope::emitReturn(mlir::Location loc) {
  auto &builder = CGF.getBuilder();

  // If we are on a coroutine, add the coro_end builtin call.
  auto Fn = dyn_cast<cir::FuncOp>(CGF.CurFn);
  assert(Fn && "other callables NYI");
  if (Fn.getCoroutine())
    CGF.emitCoroEndBuiltinCall(loc,
                               builder.getNullPtr(builder.getVoidPtrTy(), loc));

  if (CGF.FnRetCIRTy.has_value()) {
    // If there's anything to return, load it first.
    auto val = builder.create<LoadOp>(loc, *CGF.FnRetCIRTy, *CGF.FnRetAlloca);
    return builder.create<ReturnOp>(loc, llvm::ArrayRef(val.getResult()));
  }
  return builder.create<ReturnOp>(loc);
}

void CIRGenFunction::LexicalScope::emitImplicitReturn() {
  auto &builder = CGF.getBuilder();
  auto *localScope = CGF.currLexScope;

  const auto *FD = cast<clang::FunctionDecl>(CGF.CurGD.getDecl());

  // C++11 [stmt.return]p2:
  //   Flowing off the end of a function [...] results in undefined behavior
  //   in a value-returning function.
  // C11 6.9.1p12:
  //   If the '}' that terminates a function is reached, and the value of the
  //   function call is used by the caller, the behavior is undefined.
  if (CGF.getLangOpts().CPlusPlus && !FD->hasImplicitReturnZero() &&
      !CGF.SawAsmBlock && !FD->getReturnType()->isVoidType() &&
      builder.getInsertionBlock()) {
    bool shouldEmitUnreachable = CGF.CGM.getCodeGenOpts().StrictReturn ||
                                 !CGF.CGM.MayDropFunctionReturn(
                                     FD->getASTContext(), FD->getReturnType());

    if (CGF.SanOpts.has(SanitizerKind::Return)) {
      assert(!cir::MissingFeatures::sanitizerReturn());
      llvm_unreachable("NYI");
    } else if (shouldEmitUnreachable) {
      if (CGF.CGM.getCodeGenOpts().OptimizationLevel == 0) {
        builder.create<cir::TrapOp>(localScope->EndLoc);
        builder.clearInsertionPoint();
        return;
      }
    }

    if (CGF.SanOpts.has(SanitizerKind::Return) || shouldEmitUnreachable) {
      builder.create<cir::UnreachableOp>(localScope->EndLoc);
      builder.clearInsertionPoint();
      return;
    }
  }

  (void)emitReturn(localScope->EndLoc);
}

cir::TryOp CIRGenFunction::LexicalScope::getClosestTryParent() {
  auto *scope = this;
  while (scope) {
    if (scope->isTry())
      return scope->getTry();
    scope = scope->ParentScope;
  }
  return nullptr;
}

void CIRGenFunction::finishFunction(SourceLocation EndLoc) {
  // CIRGen doesn't use a BreakContinueStack or evaluates OnlySimpleReturnStmts.

  // Usually the return expression is evaluated before the cleanup
  // code.  If the function contains only a simple return statement,
  // such as a constant, the location before the cleanup code becomes
  // the last useful breakpoint in the function, because the simple
  // return expression will be evaluated after the cleanup code. To be
  // safe, set the debug location for cleanup code to the location of
  // the return statement.  Otherwise the cleanup code should be at the
  // end of the function's lexical scope.
  //
  // If there are multiple branches to the return block, the branch
  // instructions will get the location of the return statements and
  // all will be fine.
  if (auto *DI = getDebugInfo())
    assert(!cir::MissingFeatures::generateDebugInfo() && "NYI");

  // Pop any cleanups that might have been associated with the
  // parameters.  Do this in whatever block we're currently in; it's
  // important to do this before we enter the return block or return
  // edges will be *really* confused.
  bool HasCleanups = EHStack.stable_begin() != PrologueCleanupDepth;
  if (HasCleanups) {
    // Make sure the line table doesn't jump back into the body for
    // the ret after it's been at EndLoc.
    if (auto *DI = getDebugInfo())
      assert(!cir::MissingFeatures::generateDebugInfo() && "NYI");
    // FIXME(cir): should we clearInsertionPoint? breaks many testcases
    PopCleanupBlocks(PrologueCleanupDepth);
  }

  // Emit function epilog (to return).

  // Original LLVM codegen does EmitReturnBlock() here, CIRGen handles
  // this as part of LexicalScope instead, given CIR might have multiple
  // blocks with `cir.return`.
  if (ShouldInstrumentFunction()) {
    assert(!cir::MissingFeatures::shouldInstrumentFunction() && "NYI");
  }

  // Emit debug descriptor for function end.
  if (auto *DI = getDebugInfo())
    assert(!cir::MissingFeatures::generateDebugInfo() && "NYI");

  // Reset the debug location to that of the simple 'return' expression, if any
  // rather than that of the end of the function's scope '}'.
  assert(!cir::MissingFeatures::generateDebugInfo() && "NYI");

  assert(!cir::MissingFeatures::emitFunctionEpilog() && "NYI");
  assert(!cir::MissingFeatures::emitEndEHSpec() && "NYI");

  assert(EHStack.empty() && "did not remove all scopes from cleanup stack!");

  // If someone did an indirect goto, emit the indirect goto block at the end of
  // the function.
  assert(!cir::MissingFeatures::indirectBranch() && "NYI");

  // If some of our locals escaped, insert a call to llvm.localescape in the
  // entry block.
  assert(!cir::MissingFeatures::escapedLocals() && "NYI");

  // If someone took the address of a label but never did an indirect goto, we
  // made a zero entry PHI node, which is illegal, zap it now.
  assert(!cir::MissingFeatures::indirectBranch() && "NYI");

  // CIRGen doesn't need to emit EHResumeBlock, TerminateLandingPad,
  // TerminateHandler, UnreachableBlock, TerminateFunclets, NormalCleanupDest
  // here because the basic blocks aren't shared.

  assert(!cir::MissingFeatures::emitDeclMetadata() && "NYI");
  assert(!cir::MissingFeatures::deferredReplacements() && "NYI");

  // Add the min-legal-vector-width attribute. This contains the max width from:
  // 1. min-vector-width attribute used in the source program.
  // 2. Any builtins used that have a vector width specified.
  // 3. Values passed in and out of inline assembly.
  // 4. Width of vector arguments and return types for this function.
  // 5. Width of vector arguments and return types for functions called by
  // this function.
  assert(!cir::MissingFeatures::minLegalVectorWidthAttr() && "NYI");

  // Add vscale_range attribute if appropriate.
  assert(!cir::MissingFeatures::vscaleRangeAttr() && "NYI");

  // In traditional LLVM codegen, if clang generated an unreachable return
  // block, it'd be deleted now. Same for unused ret allocas from ReturnValue
}

static void eraseEmptyAndUnusedBlocks(cir::FuncOp fnOp) {
  // Remove any left over blocks that are unrecheable and empty, since they do
  // not represent unrecheable code useful for warnings nor anything deemed
  // useful in general.
  SmallVector<mlir::Block *> blocksToDelete;
  for (auto &blk : fnOp.getBlocks()) {
    if (!blk.empty() || !blk.getUses().empty())
      continue;
    blocksToDelete.push_back(&blk);
  }
  for (auto *b : blocksToDelete)
    b->erase();
}

cir::FuncOp CIRGenFunction::generateCode(clang::GlobalDecl GD, cir::FuncOp Fn,
                                         const CIRGenFunctionInfo &FnInfo) {
  assert(Fn && "generating code for a null function");
  const auto FD = cast<FunctionDecl>(GD.getDecl());
  CurGD = GD;

  FnRetQualTy = FD->getReturnType();
  if (!FnRetQualTy->isVoidType())
    FnRetCIRTy = convertType(FnRetQualTy);

  FunctionArgList Args;
  QualType ResTy = buildFunctionArgList(GD, Args);

  if (FD->isInlineBuiltinDeclaration()) {
    llvm_unreachable("NYI");
  } else {
    // Detect the unusual situation where an inline version is shadowed by a
    // non-inline version. In that case we should pick the external one
    // everywhere. That's GCC behavior too. Unfortunately, I cannot find a way
    // to detect that situation before we reach codegen, so do some late
    // replacement.
    for (const auto *PD = FD->getPreviousDecl(); PD;
         PD = PD->getPreviousDecl()) {
      if (LLVM_UNLIKELY(PD->isInlineBuiltinDeclaration())) {
        llvm_unreachable("NYI");
      }
    }
  }

  // Check if we should generate debug info for this function.
  if (FD->hasAttr<NoDebugAttr>()) {
    assert(!cir::MissingFeatures::noDebugInfo());
  }

  // The function might not have a body if we're generating thunks for a
  // function declaration.
  SourceRange BodyRange;
  if (Stmt *Body = FD->getBody())
    BodyRange = Body->getSourceRange();
  else
    BodyRange = FD->getLocation();
  // TODO: CurEHLocation

  // Use the location of the start of the function to determine where the
  // function definition is located. By default we use the location of the
  // declaration as the location for the subprogram. A function may lack a
  // declaration in the source code if it is created by code gen. (examples:
  // _GLOBAL__I_a, __cxx_global_array_dtor, thunk).
  SourceLocation Loc = FD->getLocation();

  // If this is a function specialization then use the pattern body as the
  // location for the function.
  if (const auto *SpecDecl = FD->getTemplateInstantiationPattern())
    if (SpecDecl->hasBody(SpecDecl))
      Loc = SpecDecl->getLocation();

  Stmt *Body = FD->getBody();

  if (Body) {
    // LLVM codegen: Coroutines always emit lifetime markers
    // Hide this under request for lifetime emission so that we can write
    // tests when the time comes, but CIR should be intrinsically scope
    // accurate, so no need to tie coroutines to such markers.
    if (isa<CoroutineBodyStmt>(Body))
      assert(!cir::MissingFeatures::shouldEmitLifetimeMarkers() && "NYI");

    // Initialize helper which will detect jumps which can cause invalid
    // lifetime markers.
    if (ShouldEmitLifetimeMarkers)
      assert(!cir::MissingFeatures::shouldEmitLifetimeMarkers() && "NYI");
  }

  // Create a scope in the symbol table to hold variable declarations.
  SymTableScopeTy varScope(symbolTable);
  // Compiler synthetized functions might have invalid slocs...
  auto bSrcLoc = FD->getBody()->getBeginLoc();
  auto eSrcLoc = FD->getBody()->getEndLoc();
  auto unknownLoc = builder.getUnknownLoc();

  auto FnBeginLoc = bSrcLoc.isValid() ? getLoc(bSrcLoc) : unknownLoc;
  auto FnEndLoc = eSrcLoc.isValid() ? getLoc(eSrcLoc) : unknownLoc;
  const auto fusedLoc =
      mlir::FusedLoc::get(&getMLIRContext(), {FnBeginLoc, FnEndLoc});
  SourceLocRAIIObject fnLoc{*this, Loc.isValid() ? getLoc(Loc) : unknownLoc};

  assert(Fn.isDeclaration() && "Function already has body?");
  mlir::Block *EntryBB = Fn.addEntryBlock();
  builder.setInsertionPointToStart(EntryBB);
  {
    // Initialize lexical scope information.
    LexicalScope lexScope{*this, fusedLoc, EntryBB};

    // Emit the standard function prologue.
    StartFunction(GD, ResTy, Fn, FnInfo, Args, Loc, BodyRange.getBegin());

    // Save parameters for coroutine function.
    if (Body && isa_and_nonnull<CoroutineBodyStmt>(Body))
      llvm::append_range(FnArgs, FD->parameters());

    // Ensure that the function adheres to the forward progress guarantee, which
    // is required by certain optimizations.
    // In C++11 and up, the attribute will be removed if the body contains a
    // trivial empty loop.
    if (cir::MissingFeatures::mustProgress())
      llvm_unreachable("NYI");

    // Generate the body of the function.
    // TODO: PGO.assignRegionCounters
    assert(!cir::MissingFeatures::shouldInstrumentFunction());
    if (isa<CXXDestructorDecl>(FD))
      emitDestructorBody(Args);
    else if (isa<CXXConstructorDecl>(FD))
      emitConstructorBody(Args);
    else if (getLangOpts().CUDA && !getLangOpts().CUDAIsDevice &&
             FD->hasAttr<CUDAGlobalAttr>())
      CGM.getCUDARuntime().emitDeviceStub(*this, Fn, Args);
    else if (isa<CXXMethodDecl>(FD) &&
             cast<CXXMethodDecl>(FD)->isLambdaStaticInvoker()) {
      // The lambda static invoker function is special, because it forwards or
      // clones the body of the function call operator (but is actually
      // static).
      emitLambdaStaticInvokeBody(cast<CXXMethodDecl>(FD));
    } else if (FD->isDefaulted() && isa<CXXMethodDecl>(FD) &&
               (cast<CXXMethodDecl>(FD)->isCopyAssignmentOperator() ||
                cast<CXXMethodDecl>(FD)->isMoveAssignmentOperator())) {
      // Implicit copy-assignment gets the same special treatment as implicit
      // copy-constructors.
      emitImplicitAssignmentOperatorBody(Args);
    } else if (Body) {
      if (mlir::failed(emitFunctionBody(Body))) {
        Fn.erase();
        return nullptr;
      }
    } else
      llvm_unreachable("no definition for emitted function");

    assert(builder.getInsertionBlock() && "Should be valid");

    if (mlir::failed(Fn.verifyBody()))
      return nullptr;

    // Emit the standard function epilogue.
    finishFunction(BodyRange.getEnd());

    // If we haven't marked the function nothrow through other means, do a quick
    // pass now to see if we can.
    assert(!cir::MissingFeatures::tryMarkNoThrow());
  }

  eraseEmptyAndUnusedBlocks(Fn);
  return Fn;
}

mlir::Value CIRGenFunction::createLoad(const VarDecl *VD, const char *Name) {
  auto addr = GetAddrOfLocalVar(VD);
  return builder.create<LoadOp>(getLoc(VD->getLocation()),
                                addr.getElementType(), addr.getPointer());
}

void CIRGenFunction::emitConstructorBody(FunctionArgList &Args) {
  assert(!cir::MissingFeatures::emitAsanPrologueOrEpilogue());
  const auto *Ctor = cast<CXXConstructorDecl>(CurGD.getDecl());
  auto CtorType = CurGD.getCtorType();

  assert((CGM.getTarget().getCXXABI().hasConstructorVariants() ||
          CtorType == Ctor_Complete) &&
         "can only generate complete ctor for this ABI");

  // Before we go any further, try the complete->base constructor delegation
  // optimization.
  if (CtorType == Ctor_Complete && IsConstructorDelegationValid(Ctor) &&
      CGM.getTarget().getCXXABI().hasConstructorVariants()) {
    emitDelegateCXXConstructorCall(Ctor, Ctor_Base, Args, Ctor->getEndLoc());
    return;
  }

  const FunctionDecl *Definition = nullptr;
  Stmt *Body = Ctor->getBody(Definition);
  assert(Definition == Ctor && "emitting wrong constructor body");

  // Enter the function-try-block before the constructor prologue if
  // applicable.
  bool IsTryBody = (Body && isa<CXXTryStmt>(Body));
  if (IsTryBody)
    llvm_unreachable("NYI");

  // TODO: incrementProfileCounter

  // TODO: RunClenaupCcope RunCleanups(*this);

  // TODO: in restricted cases, we can emit the vbase initializers of a
  // complete ctor and then delegate to the base ctor.

  // Emit the constructor prologue, i.e. the base and member initializers.
  emitCtorPrologue(Ctor, CtorType, Args);

  // Emit the body of the statement.
  if (IsTryBody)
    llvm_unreachable("NYI");
  else {
    // TODO: propagate this result via mlir::logical result. Just unreachable
    // now just to have it handled.
    if (mlir::failed(emitStmt(Body, true)))
      llvm_unreachable("NYI");
  }

  // Emit any cleanup blocks associated with the member or base initializers,
  // which inlcudes (along the exceptional path) the destructors for those
  // members and bases that were fully constructed.
  /// TODO: RunCleanups.ForceCleanup();

  if (IsTryBody)
    llvm_unreachable("NYI");
}

/// Given a value of type T* that may not be to a complete object, construct
/// an l-vlaue withi the natural pointee alignment of T.
LValue CIRGenFunction::MakeNaturalAlignPointeeAddrLValue(mlir::Value val,
                                                         QualType ty) {
  // FIXME(cir): is it safe to assume Op->getResult(0) is valid? Perhaps
  // assert on the result type first.
  LValueBaseInfo baseInfo;
  TBAAAccessInfo tbaaInfo;
  CharUnits align = CGM.getNaturalTypeAlignment(ty, &baseInfo, &tbaaInfo,
                                                /* for PointeeType= */ true);
  return makeAddrLValue(Address(val, align), ty, baseInfo, tbaaInfo);
}

LValue CIRGenFunction::MakeNaturalAlignAddrLValue(mlir::Value val,
                                                  QualType ty) {
  LValueBaseInfo baseInfo;
  TBAAAccessInfo tbaaInfo;
  CharUnits alignment = CGM.getNaturalTypeAlignment(ty, &baseInfo, &tbaaInfo);
  Address addr(val, convertTypeForMem(ty), alignment);
  return LValue::makeAddr(addr, ty, getContext(), baseInfo, tbaaInfo);
}

// Map the LangOption for exception behavior into the corresponding enum in
// the IR.
cir::fp::ExceptionBehavior
ToConstrainedExceptMD(LangOptions::FPExceptionModeKind Kind) {
  switch (Kind) {
  case LangOptions::FPE_Ignore:
    return cir::fp::ebIgnore;
  case LangOptions::FPE_MayTrap:
    return cir::fp::ebMayTrap;
  case LangOptions::FPE_Strict:
    return cir::fp::ebStrict;
  default:
    llvm_unreachable("Unsupported FP Exception Behavior");
  }
}

bool CIRGenFunction::ShouldSkipSanitizerInstrumentation() {
  if (!CurFuncDecl)
    return false;
  return CurFuncDecl->hasAttr<DisableSanitizerInstrumentationAttr>();
}

/// Return true if the current function should be instrumented with XRay nop
/// sleds.
bool CIRGenFunction::ShouldXRayInstrumentFunction() const {
  return CGM.getCodeGenOpts().XRayInstrumentFunctions;
}

static bool matchesStlAllocatorFn(const Decl *D, const ASTContext &astContext) {
  auto *MD = dyn_cast_or_null<CXXMethodDecl>(D);
  if (!MD || !MD->getDeclName().getAsIdentifierInfo() ||
      !MD->getDeclName().getAsIdentifierInfo()->isStr("allocate") ||
      (MD->getNumParams() != 1 && MD->getNumParams() != 2))
    return false;

  if (MD->parameters()[0]->getType().getCanonicalType() !=
      astContext.getSizeType())
    return false;

  if (MD->getNumParams() == 2) {
    auto *PT = MD->parameters()[1]->getType()->getAs<clang::PointerType>();
    if (!PT || !PT->isVoidPointerType() ||
        !PT->getPointeeType().isConstQualified())
      return false;
  }

  return true;
}

/// TODO: this should live in `emitFunctionProlog`
/// An argument came in as a promoted argument; demote it back to its
/// declared type.
static mlir::Value emitArgumentDemotion(CIRGenFunction &CGF, const VarDecl *var,
                                        mlir::Value value) {
  mlir::Type ty = CGF.convertType(var->getType());

  // This can happen with promotions that actually don't change the
  // underlying type, like the enum promotions.
  if (value.getType() == ty)
    return value;

  assert((isa<cir::IntType>(ty) || cir::isAnyFloatingPointType(ty)) &&
         "unexpected promotion type");

  if (isa<cir::IntType>(ty))
    return CGF.getBuilder().CIRBaseBuilderTy::createIntCast(value, ty);

  return CGF.getBuilder().CIRBaseBuilderTy::createCast(cir::CastKind::floating,
                                                       value, ty);
}

void CIRGenFunction::StartFunction(GlobalDecl GD, QualType RetTy,
                                   cir::FuncOp Fn,
                                   const CIRGenFunctionInfo &FnInfo,
                                   const FunctionArgList &Args,
                                   SourceLocation Loc,
                                   SourceLocation StartLoc) {
  assert(!CurFn &&
         "Do not use a CIRGenFunction object for more than one function");

  const auto *D = GD.getDecl();

  DidCallStackSave = false;
  CurCodeDecl = D;
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  if (FD && FD->usesSEHTry())
    CurSEHParent = GD;
  CurFuncDecl = (D ? D->getNonClosureContext() : nullptr);
  FnRetTy = RetTy;
  CurFn = Fn;
  CurFnInfo = &FnInfo;

  // If this function is ignored for any of the enabled sanitizers, disable
  // the sanitizer for the function.
  do {
#define SANITIZER(NAME, ID)                                                    \
  if (SanOpts.empty())                                                         \
    break;                                                                     \
  if (SanOpts.has(SanitizerKind::ID))                                          \
    if (CGM.isInNoSanitizeList(SanitizerKind::ID, Fn, Loc))                    \
      SanOpts.set(SanitizerKind::ID, false);

#include "clang/Basic/Sanitizers.def"
#undef SANITIZER
  } while (false);

  if (D) {
    const bool SanitizeBounds = SanOpts.hasOneOf(SanitizerKind::Bounds);
    SanitizerMask no_sanitize_mask;
    bool NoSanitizeCoverage = false;

    for (auto *Attr : D->specific_attrs<NoSanitizeAttr>()) {
      no_sanitize_mask |= Attr->getMask();
      // SanitizeCoverage is not handled by SanOpts.
      if (Attr->hasCoverage())
        NoSanitizeCoverage = true;
    }

    // Apply the no_sanitize* attributes to SanOpts.
    SanOpts.Mask &= ~no_sanitize_mask;
    if (no_sanitize_mask & SanitizerKind::Address)
      SanOpts.set(SanitizerKind::KernelAddress, false);
    if (no_sanitize_mask & SanitizerKind::KernelAddress)
      SanOpts.set(SanitizerKind::Address, false);
    if (no_sanitize_mask & SanitizerKind::HWAddress)
      SanOpts.set(SanitizerKind::KernelHWAddress, false);
    if (no_sanitize_mask & SanitizerKind::KernelHWAddress)
      SanOpts.set(SanitizerKind::HWAddress, false);

    // TODO(cir): set llvm::Attribute::NoSanitizeBounds
    if (SanitizeBounds && !SanOpts.hasOneOf(SanitizerKind::Bounds))
      assert(!cir::MissingFeatures::sanitizeOther());

    // TODO(cir): set llvm::Attribute::NoSanitizeCoverage
    if (NoSanitizeCoverage && CGM.getCodeGenOpts().hasSanitizeCoverage())
      assert(!cir::MissingFeatures::sanitizeOther());

    // Some passes need the non-negated no_sanitize attribute. Pass them on.
    if (CGM.getCodeGenOpts().hasSanitizeBinaryMetadata()) {
      // TODO(cir): set no_sanitize_thread
      if (no_sanitize_mask & SanitizerKind::Thread)
        assert(!cir::MissingFeatures::sanitizeOther());
    }
  }

  if (ShouldSkipSanitizerInstrumentation()) {
    assert(!cir::MissingFeatures::sanitizeOther());
  } else {
    // Apply sanitizer attributes to the function.
    if (SanOpts.hasOneOf(SanitizerKind::Address | SanitizerKind::KernelAddress))
      assert(!cir::MissingFeatures::sanitizeOther());
    if (SanOpts.hasOneOf(SanitizerKind::HWAddress |
                         SanitizerKind::KernelHWAddress))
      assert(!cir::MissingFeatures::sanitizeOther());
    if (SanOpts.has(SanitizerKind::MemtagStack))
      assert(!cir::MissingFeatures::sanitizeOther());
    if (SanOpts.has(SanitizerKind::Thread))
      assert(!cir::MissingFeatures::sanitizeOther());
    if (SanOpts.has(SanitizerKind::NumericalStability))
      assert(!cir::MissingFeatures::sanitizeOther());
    if (SanOpts.hasOneOf(SanitizerKind::Memory | SanitizerKind::KernelMemory))
      assert(!cir::MissingFeatures::sanitizeOther());
  }
  if (SanOpts.has(SanitizerKind::SafeStack))
    assert(!cir::MissingFeatures::sanitizeOther());
  if (SanOpts.has(SanitizerKind::ShadowCallStack))
    assert(!cir::MissingFeatures::sanitizeOther());

  if (SanOpts.has(SanitizerKind::Realtime))
    llvm_unreachable("NYI");

  // Apply fuzzing attribute to the function.
  if (SanOpts.hasOneOf(SanitizerKind::Fuzzer | SanitizerKind::FuzzerNoLink))
    assert(!cir::MissingFeatures::sanitizeOther());

  // Ignore TSan memory acesses from within ObjC/ObjC++ dealloc, initialize,
  // .cxx_destruct, __destroy_helper_block_ and all of their calees at run time.
  if (SanOpts.has(SanitizerKind::Thread)) {
    if (const auto *OMD = dyn_cast_or_null<ObjCMethodDecl>(D)) {
      llvm_unreachable("NYI");
    }
  }

  // Ignore unrelated casts in STL allocate() since the allocator must cast
  // from void* to T* before object initialization completes. Don't match on the
  // namespace because not all allocators are in std::
  if (D && SanOpts.has(SanitizerKind::CFIUnrelatedCast)) {
    if (matchesStlAllocatorFn(D, getContext()))
      SanOpts.Mask &= ~SanitizerKind::CFIUnrelatedCast;
  }

  // Ignore null checks in coroutine functions since the coroutines passes
  // are not aware of how to move the extra UBSan instructions across the split
  // coroutine boundaries.
  if (D && SanOpts.has(SanitizerKind::Null))
    if (FD && FD->getBody() &&
        FD->getBody()->getStmtClass() == Stmt::CoroutineBodyStmtClass)
      SanOpts.Mask &= ~SanitizerKind::Null;

  // Add pointer authentication attriburtes.
  const CodeGenOptions &codeGenOptions = CGM.getCodeGenOpts();
  if (codeGenOptions.PointerAuth.ReturnAddresses)
    llvm_unreachable("NYI");
  if (codeGenOptions.PointerAuth.FunctionPointers)
    llvm_unreachable("NYI");
  if (codeGenOptions.PointerAuth.AuthTraps)
    llvm_unreachable("NYI");
  if (codeGenOptions.PointerAuth.IndirectGotos)
    llvm_unreachable("NYI");

  // Apply xray attributes to the function (as a string, for now)
  if (const auto *XRayAttr = D ? D->getAttr<XRayInstrumentAttr>() : nullptr) {
    assert(!cir::MissingFeatures::xray());
  } else {
    assert(!cir::MissingFeatures::xray());
  }

  if (ShouldXRayInstrumentFunction()) {
    assert(!cir::MissingFeatures::xray());
  }

  if (CGM.getCodeGenOpts().getProfileInstr() != CodeGenOptions::ProfileNone) {
    assert(!cir::MissingFeatures::getProfileCount());
  }

  unsigned Count, Offset;
  if (const auto *Attr =
          D ? D->getAttr<PatchableFunctionEntryAttr>() : nullptr) {
    llvm_unreachable("NYI");
  } else {
    Count = CGM.getCodeGenOpts().PatchableFunctionEntryCount;
    Offset = CGM.getCodeGenOpts().PatchableFunctionEntryOffset;
  }
  if (Count && Offset <= Count) {
    llvm_unreachable("NYI");
  }
  // Instruct that functions for COFF/CodeView targets should start with a
  // pathable instruction, but only on x86/x64. Don't forward this to ARM/ARM64
  // backends as they don't need it -- instructions on these architectures are
  // always automatically patachable at runtime.
  if (CGM.getCodeGenOpts().HotPatch &&
      getContext().getTargetInfo().getTriple().isX86() &&
      getContext().getTargetInfo().getTriple().getEnvironment() !=
          llvm::Triple::CODE16)
    llvm_unreachable("NYI");

  // Add no-jump-tables value.
  if (CGM.getCodeGenOpts().NoUseJumpTables)
    llvm_unreachable("NYI");

  // Add no-inline-line-tables value.
  if (CGM.getCodeGenOpts().NoInlineLineTables)
    llvm_unreachable("NYI");

  // Add profile-sample-accurate value.
  if (CGM.getCodeGenOpts().ProfileSampleAccurate)
    llvm_unreachable("NYI");

  if (!CGM.getCodeGenOpts().SampleProfileFile.empty())
    llvm_unreachable("NYI");

  if (D && D->hasAttr<CFICanonicalJumpTableAttr>())
    llvm_unreachable("NYI");

  if (D && D->hasAttr<NoProfileFunctionAttr>())
    llvm_unreachable("NYI");

  if (D && D->hasAttr<HybridPatchableAttr>())
    llvm_unreachable("NYI");

  if (D) {
    // Funciton attribiutes take precedence over command line flags.
    if ([[maybe_unused]] auto *a = D->getAttr<FunctionReturnThunksAttr>()) {
      llvm_unreachable("NYI");
    } else if (CGM.getCodeGenOpts().FunctionReturnThunks)
      llvm_unreachable("NYI");
  }

  if (FD && (getLangOpts().OpenCL ||
             ((getLangOpts().HIP || getLangOpts().OffloadViaLLVM) &&
              getLangOpts().CUDAIsDevice))) {
    // Add metadata for a kernel function.
    emitKernelMetadata(FD, Fn);
  }

  if (FD && FD->hasAttr<ClspvLibclcBuiltinAttr>()) {
    llvm_unreachable("NYI");
  }

  // If we are checking function types, emit a function type signature as
  // prologue data.
  if (FD && getLangOpts().CPlusPlus && SanOpts.has(SanitizerKind::Function)) {
    llvm_unreachable("NYI");
  }

  // If we're checking nullability, we need to know whether we can check the
  // return value. Initialize the falg to 'true' and refine it in
  // emitParmDecl.
  if (SanOpts.has(SanitizerKind::NullabilityReturn)) {
    llvm_unreachable("NYI");
  }

  // If we're in C++ mode and the function name is "main", it is guaranteed to
  // be norecurse by the standard (3.6.1.3 "The function main shall not be
  // used within a program").
  //
  // OpenCL C 2.0 v2.2-11 s6.9.i:
  //     Recursion is not supported.
  //
  // SYCL v1.2.1 s3.10:
  //     kernels cannot include RTTI information, exception cases, recursive
  //     code, virtual functions or make use of C++ libraries that are not
  //     compiled for the device.
  if (FD &&
      ((getLangOpts().CPlusPlus && FD->isMain()) || getLangOpts().OpenCL ||
       getLangOpts().SYCLIsDevice |
           (getLangOpts().CUDA && FD->hasAttr<CUDAGlobalAttr>())))
    ; // TODO: support norecurse attr

  llvm::RoundingMode RM = getLangOpts().getDefaultRoundingMode();
  cir::fp::ExceptionBehavior FPExceptionBehavior =
      ToConstrainedExceptMD(getLangOpts().getDefaultExceptionMode());
  builder.setDefaultConstrainedRounding(RM);
  builder.setDefaultConstrainedExcept(FPExceptionBehavior);
  if ((FD && (FD->UsesFPIntrin() || FD->hasAttr<StrictFPAttr>())) ||
      (!FD && (FPExceptionBehavior != cir::fp::ebIgnore ||
               RM != llvm::RoundingMode::NearestTiesToEven))) {
    llvm_unreachable("NYI");
  }

  if (cir::MissingFeatures::stackrealign())
    llvm_unreachable("NYI");

  if (FD && FD->isMain() && cir::MissingFeatures::zerocallusedregs())
    llvm_unreachable("NYI");

  // CIRGen has its own logic for entry blocks, usually per operation region.
  mlir::Block *retBlock = currLexScope->getOrCreateRetBlock(*this, getLoc(Loc));
  // returnBlock handles per region getJumpDestInCurrentScope LLVM traditional
  // codegen logic.
  (void)returnBlock(retBlock);

  mlir::Block *EntryBB = &Fn.getBlocks().front();

  if (cir::MissingFeatures::requiresReturnValueCheck())
    llvm_unreachable("NYI");

  if (getDebugInfo()) {
    llvm_unreachable("NYI");
  }

  if (ShouldInstrumentFunction()) {
    llvm_unreachable("NYI");
  }

  // Since emitting the mcount call here impacts optimizations such as
  // function inlining, we just add an attribute to insert a mcount call in
  // backend. The attribute "counting-function" is set to mcount function name
  // which is architecture dependent.
  if (CGM.getCodeGenOpts().InstrumentForProfiling) {
    llvm_unreachable("NYI");
  }

  if (CGM.getCodeGenOpts().PackedStack) {
    llvm_unreachable("NYI");
  }

  if (CGM.getCodeGenOpts().WarnStackSize != UINT_MAX) {
    llvm_unreachable("NYI");
  }

  assert(!cir::MissingFeatures::emitStartEHSpec() && "NYI");
  PrologueCleanupDepth = EHStack.stable_begin();

  // Emit OpenMP specific initialization of the device functions.
  if (getLangOpts().OpenMP && CurCodeDecl)
    CGM.getOpenMPRuntime().emitFunctionProlog(*this, CurCodeDecl);

  if (FD && getLangOpts().HLSL) {
    // Handle emitting HLSL entry functions.
    if (FD->hasAttr<HLSLShaderAttr>()) {
      llvm_unreachable("NYI");
    }
    llvm_unreachable("NYI");
  }

  // TODO: emitFunctionProlog

  {
    // Set the insertion point in the builder to the beginning of the
    // function body, it will be used throughout the codegen to create
    // operations in this function.
    builder.setInsertionPointToStart(EntryBB);

    // TODO: this should live in `emitFunctionProlog
    // Declare all the function arguments in the symbol table.
    for (const auto nameValue : llvm::zip(Args, EntryBB->getArguments())) {
      auto *paramVar = std::get<0>(nameValue);
      mlir::Value paramVal = std::get<1>(nameValue);
      auto alignment = getContext().getDeclAlign(paramVar);
      auto paramLoc = getLoc(paramVar->getSourceRange());
      paramVal.setLoc(paramLoc);

      mlir::Value addr;
      if (failed(declare(paramVar, paramVar->getType(), paramLoc, alignment,
                         addr, true /*param*/)))
        return;

      auto address = Address(addr, alignment);
      setAddrOfLocalVar(paramVar, address);

      // TODO: this should live in `emitFunctionProlog`
      bool isPromoted = isa<ParmVarDecl>(paramVar) &&
                        cast<ParmVarDecl>(paramVar)->isKNRPromoted();
      assert(!cir::MissingFeatures::constructABIArgDirectExtend());
      if (isPromoted)
        paramVal = emitArgumentDemotion(*this, paramVar, paramVal);

      // Location of the store to the param storage tracked as beginning of
      // the function body.
      auto fnBodyBegin = getLoc(FD->getBody()->getBeginLoc());
      builder.CIRBaseBuilderTy::createStore(fnBodyBegin, paramVal, addr);
    }
    assert(builder.getInsertionBlock() && "Should be valid");

    auto FnEndLoc = getLoc(FD->getBody()->getEndLoc());

    // When the current function is not void, create an address to store the
    // result value.
    if (FnRetCIRTy.has_value())
      emitAndUpdateRetAlloca(FnRetQualTy, FnEndLoc,
                             CGM.getNaturalTypeAlignment(FnRetQualTy));
  }

  if (D && isa<CXXMethodDecl>(D) && cast<CXXMethodDecl>(D)->isInstance()) {
    CGM.getCXXABI().emitInstanceFunctionProlog(Loc, *this);

    const auto *MD = cast<CXXMethodDecl>(D);
    if (MD->getParent()->isLambda() && MD->getOverloadedOperator() == OO_Call) {
      // We're in a lambda.
      auto Fn = dyn_cast<cir::FuncOp>(CurFn);
      assert(Fn && "other callables NYI");
      Fn.setLambdaAttr(mlir::UnitAttr::get(&getMLIRContext()));

      // Figure out the captures.
      MD->getParent()->getCaptureFields(LambdaCaptureFields,
                                        LambdaThisCaptureField);
      if (LambdaThisCaptureField) {
        // If the lambda captures the object referred to by '*this' - either by
        // value or by reference, make sure CXXThisValue points to the correct
        // object.

        // Get the lvalue for the field (which is a copy of the enclosing object
        // or contains the address of the enclosing object).
        LValue thisFieldLValue =
            emitLValueForLambdaField(LambdaThisCaptureField);
        if (!LambdaThisCaptureField->getType()->isPointerType()) {
          // If the enclosing object was captured by value, just use its
          // address. Sign this pointer.
          CXXThisValue = thisFieldLValue.getPointer();
        } else {
          // Load the lvalue pointed to by the field, since '*this' was captured
          // by reference.
          CXXThisValue = emitLoadOfLValue(thisFieldLValue, SourceLocation())
                             .getScalarVal();
        }
      }
      for (auto *FD : MD->getParent()->fields()) {
        if (FD->hasCapturedVLAType()) {
          llvm_unreachable("NYI");
        }
      }

    } else {
      // Not in a lambda; just use 'this' from the method.
      // FIXME: Should we generate a new load for each use of 'this'? The fast
      // register allocator would be happier...
      CXXThisValue = CXXABIThisValue;
    }

    // Check the 'this' pointer once per function, if it's available
    if (CXXABIThisValue) {
      SanitizerSet SkippedChecks;
      SkippedChecks.set(SanitizerKind::ObjectSize, true);
      QualType ThisTy = MD->getThisType();
      (void)ThisTy;

      // If this is the call operator of a lambda with no capture-default, it
      // may have a staic invoker function, which may call this operator with
      // a null 'this' pointer.
      if (isLambdaCallOperator(MD) &&
          MD->getParent()->getLambdaCaptureDefault() == LCD_None)
        SkippedChecks.set(SanitizerKind::Null, true);

      assert(!cir::MissingFeatures::emitTypeCheck() && "NYI");
    }
  }

  // If any of the arguments have a variably modified type, make sure to emit
  // the type size, but only if the function is not naked. Naked functions have
  // no prolog to run this evaluation.
  if (!FD || !FD->hasAttr<NakedAttr>()) {
    for (const VarDecl *vd : Args) {
      // Dig out the type as written from ParmVarDecls; it's unclear whether the
      // standard (C99 6.9.1p10) requires this, but we're following the
      // precedent set by gcc.
      QualType ty;
      if (const auto *pvd = dyn_cast<ParmVarDecl>(vd))
        ty = pvd->getOriginalType();
      else
        ty = vd->getType();

      if (ty->isVariablyModifiedType())
        emitVariablyModifiedType(ty);
    }
  }
  // Emit a location at the end of the prologue.
  if (getDebugInfo())
    llvm_unreachable("NYI");
  // TODO: Do we need to handle this in two places like we do with
  // target-features/target-cpu?
  if (CurFuncDecl)
    if ([[maybe_unused]] const auto *vecWidth =
            CurFuncDecl->getAttr<MinVectorWidthAttr>())
      llvm_unreachable("NYI");

  if (CGM.shouldEmitConvergenceTokens())
    llvm_unreachable("NYI");
}

/// Return true if the current function should be instrumented with
/// __cyg_profile_func_* calls
bool CIRGenFunction::ShouldInstrumentFunction() {
  if (!CGM.getCodeGenOpts().InstrumentFunctions &&
      !CGM.getCodeGenOpts().InstrumentFunctionsAfterInlining &&
      !CGM.getCodeGenOpts().InstrumentFunctionEntryBare)
    return false;

  llvm_unreachable("NYI");
}

mlir::LogicalResult CIRGenFunction::emitFunctionBody(const clang::Stmt *Body) {
  // TODO: incrementProfileCounter(Body);

  // We start with function level scope for variables.
  SymTableScopeTy varScope(symbolTable);

  auto result = mlir::LogicalResult::success();
  if (const CompoundStmt *S = dyn_cast<CompoundStmt>(Body))
    emitCompoundStmtWithoutScope(*S);
  else
    result = emitStmt(Body, /*useCurrentScope*/ true);

  // This is checked after emitting the function body so we know if there are
  // any permitted infinite loops.
  // TODO: if (checkIfFunctionMustProgress())
  // CurFn->addFnAttr(llvm::Attribute::MustProgress);
  return result;
}

clang::QualType CIRGenFunction::buildFunctionArgList(clang::GlobalDecl GD,
                                                     FunctionArgList &Args) {
  const auto *FD = cast<FunctionDecl>(GD.getDecl());
  QualType ResTy = FD->getReturnType();

  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (MD && MD->isInstance()) {
    if (CGM.getCXXABI().HasThisReturn(GD))
      llvm_unreachable("NYI");
    else if (CGM.getCXXABI().hasMostDerivedReturn(GD))
      llvm_unreachable("NYI");
    CGM.getCXXABI().buildThisParam(*this, Args);
  }

  // The base version of an inheriting constructor whose constructed base is a
  // virtual base is not passed any arguments (because it doesn't actually
  // call the inherited constructor).
  bool PassedParams = true;
  if (const auto *CD = dyn_cast<CXXConstructorDecl>(FD))
    if (auto Inherited = CD->getInheritedConstructor())
      PassedParams =
          getTypes().inheritingCtorHasParams(Inherited, GD.getCtorType());

  if (PassedParams) {
    for (auto *Param : FD->parameters()) {
      Args.push_back(Param);
      if (!Param->hasAttr<PassObjectSizeAttr>())
        continue;

      auto *Implicit = ImplicitParamDecl::Create(
          getContext(), Param->getDeclContext(), Param->getLocation(),
          /*Id=*/nullptr, getContext().getSizeType(), ImplicitParamKind::Other);
      SizeArguments[Param] = Implicit;
      Args.push_back(Implicit);
    }
  }

  if (MD && (isa<CXXConstructorDecl>(MD) || isa<CXXDestructorDecl>(MD)))
    CGM.getCXXABI().addImplicitStructorParams(*this, ResTy, Args);

  return ResTy;
}

static std::string getVersionedTmpName(llvm::StringRef name, unsigned cnt) {
  SmallString<256> Buffer;
  llvm::raw_svector_ostream Out(Buffer);
  Out << name << cnt;
  return std::string(Out.str());
}

std::string CIRGenFunction::getCounterAggTmpAsString() {
  return getVersionedTmpName("agg.tmp", CounterAggTmp++);
}

std::string CIRGenFunction::getCounterRefTmpAsString() {
  return getVersionedTmpName("ref.tmp", CounterRefTmp++);
}

void CIRGenFunction::emitNullInitialization(mlir::Location loc, Address DestPtr,
                                            QualType Ty) {
  // Ignore empty classes in C++.
  if (getLangOpts().CPlusPlus) {
    if (const RecordType *RT = Ty->getAs<RecordType>()) {
      if (cast<CXXRecordDecl>(RT->getDecl())->isEmpty())
        return;
    }
  }

  // Cast the dest ptr to the appropriate i8 pointer type.
  if (builder.isInt8Ty(DestPtr.getElementType())) {
    llvm_unreachable("NYI");
  }

  // Get size and alignment info for this aggregate.
  CharUnits size = getContext().getTypeSizeInChars(Ty);
  [[maybe_unused]] mlir::Attribute SizeVal{};
  [[maybe_unused]] const VariableArrayType *vla = nullptr;

  // Don't bother emitting a zero-byte memset.
  if (size.isZero()) {
    // But note that getTypeInfo returns 0 for a VLA.
    if (const VariableArrayType *vlaType = dyn_cast_or_null<VariableArrayType>(
            getContext().getAsArrayType(Ty))) {
      llvm_unreachable("NYI");
    } else {
      return;
    }
  } else {
    SizeVal = CGM.getSize(size);
  }

  // If the type contains a pointer to data member we can't memset it to zero.
  // Instead, create a null constant and copy it to the destination.
  // TODO: there are other patterns besides zero that we can usefully memset,
  // like -1, which happens to be the pattern used by member-pointers.
  if (!CGM.getTypes().isZeroInitializable(Ty)) {
    llvm_unreachable("NYI");
  }

  // In LLVM Codegen: otherwise, just memset the whole thing to zero using
  // Builder.CreateMemSet. In CIR just emit a store of #cir.zero to the
  // respective address.
  // Builder.CreateMemSet(DestPtr, Builder.getInt8(0), SizeVal, false);
  builder.createStore(loc, builder.getZero(loc, convertType(Ty)), DestPtr);
}

CIRGenFunction::CIRGenFPOptionsRAII::CIRGenFPOptionsRAII(CIRGenFunction &CGF,
                                                         const clang::Expr *E)
    : CGF(CGF) {
  ConstructorHelper(E->getFPFeaturesInEffect(CGF.getLangOpts()));
}

CIRGenFunction::CIRGenFPOptionsRAII::CIRGenFPOptionsRAII(CIRGenFunction &CGF,
                                                         FPOptions FPFeatures)
    : CGF(CGF) {
  ConstructorHelper(FPFeatures);
}

void CIRGenFunction::CIRGenFPOptionsRAII::ConstructorHelper(
    FPOptions FPFeatures) {
  OldFPFeatures = CGF.CurFPFeatures;
  CGF.CurFPFeatures = FPFeatures;

  OldExcept = CGF.builder.getDefaultConstrainedExcept();
  OldRounding = CGF.builder.getDefaultConstrainedRounding();

  if (OldFPFeatures == FPFeatures)
    return;

  // TODO(cir): create guard to restore fast math configurations.
  assert(!cir::MissingFeatures::fastMathGuard());

  llvm::RoundingMode NewRoundingBehavior = FPFeatures.getRoundingMode();
  // TODO(cir): override rounding behaviour once FM configs are guarded.
  auto NewExceptionBehavior =
      ToConstrainedExceptMD(static_cast<LangOptions::FPExceptionModeKind>(
          FPFeatures.getExceptionMode()));
  // TODO(cir): override exception behaviour once FM configs are guarded.

  // TODO(cir): override FP flags once FM configs are guarded.
  assert(!cir::MissingFeatures::fastMathFlags());

  assert((CGF.CurFuncDecl == nullptr || CGF.builder.getIsFPConstrained() ||
          isa<CXXConstructorDecl>(CGF.CurFuncDecl) ||
          isa<CXXDestructorDecl>(CGF.CurFuncDecl) ||
          (NewExceptionBehavior == cir::fp::ebIgnore &&
           NewRoundingBehavior == llvm::RoundingMode::NearestTiesToEven)) &&
         "FPConstrained should be enabled on entire function");

  // TODO(cir): mark CIR function with fast math attributes.
  assert(!cir::MissingFeatures::fastMathFuncAttributes());
}

CIRGenFunction::CIRGenFPOptionsRAII::~CIRGenFPOptionsRAII() {
  CGF.CurFPFeatures = OldFPFeatures;
  CGF.builder.setDefaultConstrainedExcept(OldExcept);
  CGF.builder.setDefaultConstrainedRounding(OldRounding);
}

// TODO(cir): should be shared with LLVM codegen.
bool CIRGenFunction::shouldNullCheckClassCastValue(const CastExpr *CE) {
  const Expr *E = CE->getSubExpr();

  if (CE->getCastKind() == CK_UncheckedDerivedToBase)
    return false;

  if (isa<CXXThisExpr>(E->IgnoreParens())) {
    // We always assume that 'this' is never null.
    return false;
  }

  if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(CE)) {
    // And that glvalue casts are never null.
    if (ICE->isGLValue())
      return false;
  }

  return true;
}

void CIRGenFunction::emitDeclRefExprDbgValue(const DeclRefExpr *E,
                                             const APValue &Init) {
  assert(!cir::MissingFeatures::generateDebugInfo());
}

Address CIRGenFunction::emitVAListRef(const Expr *E) {
  if (getContext().getBuiltinVaListType()->isArrayType())
    return emitPointerWithAlignment(E);
  return emitLValue(E).getAddress();
}

// Emits an error if we don't have a valid set of target features for the
// called function.
void CIRGenFunction::checkTargetFeatures(const CallExpr *E,
                                         const FunctionDecl *TargetDecl) {
  return checkTargetFeatures(E->getBeginLoc(), TargetDecl);
}

// Emits an error if we don't have a valid set of target features for the
// called function.
void CIRGenFunction::checkTargetFeatures(SourceLocation Loc,
                                         const FunctionDecl *TargetDecl) {
  // Early exit if this is an indirect call.
  if (!TargetDecl)
    return;

  // Get the current enclosing function if it exists. If it doesn't
  // we can't check the target features anyhow.
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(CurCodeDecl);
  if (!FD)
    return;

  // Grab the required features for the call. For a builtin this is listed in
  // the td file with the default cpu, for an always_inline function this is any
  // listed cpu and any listed features.
  unsigned BuiltinID = TargetDecl->getBuiltinID();
  std::string MissingFeature;
  llvm::StringMap<bool> CallerFeatureMap;
  CGM.getASTContext().getFunctionFeatureMap(CallerFeatureMap, FD);
  if (BuiltinID) {
    StringRef FeatureList(
        getContext().BuiltinInfo.getRequiredFeatures(BuiltinID));
    if (!Builtin::evaluateRequiredTargetFeatures(FeatureList,
                                                 CallerFeatureMap)) {
      CGM.getDiags().Report(Loc, diag::err_builtin_needs_feature)
          << TargetDecl->getDeclName() << FeatureList;
    }
  } else if (!TargetDecl->isMultiVersion() &&
             TargetDecl->hasAttr<TargetAttr>()) {
    // Get the required features for the callee.

    const TargetAttr *TD = TargetDecl->getAttr<TargetAttr>();
    ParsedTargetAttr ParsedAttr = getContext().filterFunctionTargetAttrs(TD);

    SmallVector<StringRef, 1> ReqFeatures;
    llvm::StringMap<bool> CalleeFeatureMap;
    getContext().getFunctionFeatureMap(CalleeFeatureMap, TargetDecl);

    for (const auto &F : ParsedAttr.Features) {
      if (F[0] == '+' && CalleeFeatureMap.lookup(F.substr(1)))
        ReqFeatures.push_back(StringRef(F).substr(1));
    }

    for (const auto &F : CalleeFeatureMap) {
      // Only positive features are "required".
      if (F.getValue())
        ReqFeatures.push_back(F.getKey());
    }
    if (!llvm::all_of(ReqFeatures, [&](StringRef Feature) {
          if (!CallerFeatureMap.lookup(Feature)) {
            MissingFeature = Feature.str();
            return false;
          }
          return true;
        }))
      CGM.getDiags().Report(Loc, diag::err_function_needs_feature)
          << FD->getDeclName() << TargetDecl->getDeclName() << MissingFeature;
  } else if (!FD->isMultiVersion() && FD->hasAttr<TargetAttr>()) {
    llvm::StringMap<bool> CalleeFeatureMap;
    getContext().getFunctionFeatureMap(CalleeFeatureMap, TargetDecl);

    for (const auto &F : CalleeFeatureMap) {
      if (F.getValue() && (!CallerFeatureMap.lookup(F.getKey()) ||
                           !CallerFeatureMap.find(F.getKey())->getValue()))
        CGM.getDiags().Report(Loc, diag::err_function_needs_feature)
            << FD->getDeclName() << TargetDecl->getDeclName() << F.getKey();
    }
  }
}

CIRGenFunction::VlaSizePair CIRGenFunction::getVLASize(QualType type) {
  const VariableArrayType *vla =
      CGM.getASTContext().getAsVariableArrayType(type);
  assert(vla && "type was not a variable array type!");
  return getVLASize(vla);
}

CIRGenFunction::VlaSizePair
CIRGenFunction::getVLASize(const VariableArrayType *type) {
  // The number of elements so far; always size_t.
  mlir::Value numElements;

  QualType elementType;
  do {
    elementType = type->getElementType();
    mlir::Value vlaSize = VLASizeMap[type->getSizeExpr()];
    assert(vlaSize && "no size for VLA!");
    assert(vlaSize.getType() == SizeTy);

    if (!numElements) {
      numElements = vlaSize;
    } else {
      // It's undefined behavior if this wraps around, so mark it that way.
      // FIXME: Teach -fsanitize=undefined to trap this.

      numElements = builder.createMul(numElements, vlaSize);
    }
  } while ((type = getContext().getAsVariableArrayType(elementType)));

  assert(numElements && "Undefined elements number");
  return {numElements, elementType};
}

// TODO(cir): most part of this function can be shared between CIRGen
// and traditional LLVM codegen
void CIRGenFunction::emitVariablyModifiedType(QualType type) {
  assert(type->isVariablyModifiedType() &&
         "Must pass variably modified type to EmitVLASizes!");

  // We're going to walk down into the type and look for VLA
  // expressions.
  do {
    assert(type->isVariablyModifiedType());

    const Type *ty = type.getTypePtr();
    switch (ty->getTypeClass()) {
    case clang::Type::CountAttributed:
    case clang::Type::PackIndexing:
    case clang::Type::ArrayParameter:
    case clang::Type::HLSLAttributedResource:
      llvm_unreachable("NYI");

#define TYPE(Class, Base)
#define ABSTRACT_TYPE(Class, Base)
#define NON_CANONICAL_TYPE(Class, Base)
#define DEPENDENT_TYPE(Class, Base) case Type::Class:
#define NON_CANONICAL_UNLESS_DEPENDENT_TYPE(Class, Base)
#include "clang/AST/TypeNodes.inc"
      llvm_unreachable("unexpected dependent type!");

    // These types are never variably-modified.
    case Type::Builtin:
    case Type::Complex:
    case Type::Vector:
    case Type::ExtVector:
    case Type::ConstantMatrix:
    case Type::Record:
    case Type::Enum:
    case Type::Using:
    case Type::TemplateSpecialization:
    case Type::ObjCTypeParam:
    case Type::ObjCObject:
    case Type::ObjCInterface:
    case Type::ObjCObjectPointer:
    case Type::BitInt:
      llvm_unreachable("type class is never variably-modified!");

    case Type::Elaborated:
      type = cast<clang::ElaboratedType>(ty)->getNamedType();
      break;

    case Type::Adjusted:
      type = cast<clang::AdjustedType>(ty)->getAdjustedType();
      break;

    case Type::Decayed:
      type = cast<clang::DecayedType>(ty)->getPointeeType();
      break;

    case Type::Pointer:
      type = cast<clang::PointerType>(ty)->getPointeeType();
      break;

    case Type::BlockPointer:
      type = cast<clang::BlockPointerType>(ty)->getPointeeType();
      break;

    case Type::LValueReference:
    case Type::RValueReference:
      type = cast<clang::ReferenceType>(ty)->getPointeeType();
      break;

    case Type::MemberPointer:
      type = cast<clang::MemberPointerType>(ty)->getPointeeType();
      break;

    case Type::ConstantArray:
    case Type::IncompleteArray:
      // Losing element qualification here is fine.
      type = cast<clang::ArrayType>(ty)->getElementType();
      break;

    case Type::VariableArray: {
      // Losing element qualification here is fine.
      const VariableArrayType *vat = cast<clang::VariableArrayType>(ty);

      // Unknown size indication requires no size computation.
      // Otherwise, evaluate and record it.
      if (const Expr *sizeExpr = vat->getSizeExpr()) {
        // It's possible that we might have emitted this already,
        // e.g. with a typedef and a pointer to it.
        mlir::Value &entry = VLASizeMap[sizeExpr];
        if (!entry) {
          mlir::Value size = emitScalarExpr(sizeExpr);
          assert(!cir::MissingFeatures::sanitizeVLABound());

          // Always zexting here would be wrong if it weren't
          // undefined behavior to have a negative bound.
          // FIXME: What about when size's type is larger than size_t?
          entry = builder.createIntCast(size, SizeTy);
        }
      }
      type = vat->getElementType();
      break;
    }

    case Type::FunctionProto:
    case Type::FunctionNoProto:
      type = cast<clang::FunctionType>(ty)->getReturnType();
      break;

    case Type::Paren:
    case Type::TypeOf:
    case Type::UnaryTransform:
    case Type::Attributed:
    case Type::BTFTagAttributed:
    case Type::SubstTemplateTypeParm:
    case Type::MacroQualified:
      // Keep walking after single level desugaring.
      type = type.getSingleStepDesugaredType(getContext());
      break;

    case Type::Typedef:
    case Type::Decltype:
    case Type::Auto:
    case Type::DeducedTemplateSpecialization:
      // Stop walking: nothing to do.
      return;

    case Type::TypeOfExpr:
      // Stop walking: emit typeof expression.
      emitIgnoredExpr(cast<clang::TypeOfExprType>(ty)->getUnderlyingExpr());
      return;

    case Type::Atomic:
      type = cast<clang::AtomicType>(ty)->getValueType();
      break;

    case Type::Pipe:
      type = cast<clang::PipeType>(ty)->getElementType();
      break;
    }
  } while (type->isVariablyModifiedType());
}

/// Computes the length of an array in elements, as well as the base
/// element type and a properly-typed first element pointer.
mlir::Value
CIRGenFunction::emitArrayLength(const clang::ArrayType *origArrayType,
                                QualType &baseType, Address &addr) {
  const auto *arrayType = origArrayType;

  // If it's a VLA, we have to load the stored size.  Note that
  // this is the size of the VLA in bytes, not its size in elements.
  mlir::Value numVLAElements{};
  if (isa<VariableArrayType>(arrayType)) {
    llvm_unreachable("NYI");
  }

  uint64_t countFromCLAs = 1;
  QualType eltType;

  // llvm::ArrayType *llvmArrayType =
  //     dyn_cast<llvm::ArrayType>(addr.getElementType());
  auto cirArrayType = mlir::dyn_cast<cir::ArrayType>(addr.getElementType());

  while (cirArrayType) {
    assert(isa<ConstantArrayType>(arrayType));
    countFromCLAs *= cirArrayType.getSize();
    eltType = arrayType->getElementType();

    cirArrayType = mlir::dyn_cast<cir::ArrayType>(cirArrayType.getEltType());

    arrayType = getContext().getAsArrayType(arrayType->getElementType());
    assert((!cirArrayType || arrayType) &&
           "CIR and Clang types are out-of-synch");
  }

  if (arrayType) {
    // From this point onwards, the Clang array type has been emitted
    // as some other type (probably a packed struct). Compute the array
    // size, and just emit the 'begin' expression as a bitcast.
    llvm_unreachable("NYI");
  }

  baseType = eltType;
  auto numElements = builder.getConstInt(*currSrcLoc, SizeTy, countFromCLAs);

  // If we had any VLA dimensions, factor them in.
  if (numVLAElements)
    llvm_unreachable("NYI");

  return numElements;
}

mlir::Value CIRGenFunction::emitAlignmentAssumption(
    mlir::Value ptrValue, QualType ty, SourceLocation loc,
    SourceLocation assumptionLoc, mlir::IntegerAttr alignment,
    mlir::Value offsetValue) {
  if (SanOpts.has(SanitizerKind::Alignment))
    llvm_unreachable("NYI");
  return builder.create<cir::AssumeAlignedOp>(getLoc(assumptionLoc), ptrValue,
                                              alignment, offsetValue);
}

mlir::Value CIRGenFunction::emitAlignmentAssumption(
    mlir::Value ptrValue, const Expr *expr, SourceLocation assumptionLoc,
    mlir::IntegerAttr alignment, mlir::Value offsetValue) {
  QualType ty = expr->getType();
  SourceLocation loc = expr->getExprLoc();
  return emitAlignmentAssumption(ptrValue, ty, loc, assumptionLoc, alignment,
                                 offsetValue);
}

void CIRGenFunction::emitVarAnnotations(const VarDecl *decl, mlir::Value val) {
  assert(decl->hasAttr<AnnotateAttr>() && "no annotate attribute");
  llvm::SmallVector<mlir::Attribute, 4> annotations;
  for (const auto *annot : decl->specific_attrs<AnnotateAttr>()) {
    annotations.push_back(CGM.emitAnnotateAttr(annot));
  }
  auto allocaOp = dyn_cast_or_null<cir::AllocaOp>(val.getDefiningOp());
  assert(allocaOp && "expects available alloca");
  allocaOp.setAnnotationsAttr(builder.getArrayAttr(annotations));
}
