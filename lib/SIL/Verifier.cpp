//===--- Verifier.cpp - Verification of Swift SIL Code --------------------===//
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

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/SILVTable.h"
#include "swift/SIL/Dominance.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Basic/Range.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#define DEBUG_TYPE "silverifier"
using namespace swift;

using Lowering::AbstractionPattern;

// The verifier is basically all assertions, so don't compile it with NDEBUG to
// prevent release builds from triggering spurious unused variable warnings.
#ifndef NDEBUG

/// Returns true if A is an opened existential type, Self, or is equal to an
/// archetype in F's nested archetype list.
///
/// FIXME: Once Self has been removed in favor of opened existential types
/// everywhere, remove support for self.
static bool isArchetypeValidInFunction(ArchetypeType *A, SILFunction *F) {
  // The only two cases where an archetype is always legal in a function is if
  // it is self or if it is from an opened existential type. Currently, Self is
  // being migrated away from in favor of opened existential types, so we should
  // remove the special case here for Self when that process is completed.
  //
  // *NOTE* Associated types of self are not valid here.
  if (!A->getOpenedExistentialType().isNull() || A->getSelfProtocol())
    return true;

  // Ok, we have an archetype, make sure it is in the nested archetypes of our
  // caller.
  for (auto Iter : F->getContextGenericParams()->getAllNestedArchetypes())
    if (A->isEqual(&*Iter))
      return true;
  return false;
}

namespace {
/// Metaprogramming-friendly base class.
template <class Impl>
class SILVerifierBase : public SILVisitor<Impl> {
public:
  // visitCLASS calls visitPARENT and checkCLASS.
  // checkCLASS does nothing by default.
#define VALUE(CLASS, PARENT)                                    \
  void visit##CLASS(CLASS *I) {                                 \
    static_cast<Impl*>(this)->visit##PARENT(I);                 \
    static_cast<Impl*>(this)->check##CLASS(I);                  \
  }                                                             \
  void check##CLASS(CLASS *I) {}
#include "swift/SIL/SILNodes.def"

  void visitValueBase(ValueBase *V) {
    static_cast<Impl*>(this)->checkValueBase(V);
  }
  void checkValueBase(ValueBase *V) {}
};
} // end anonymous namespace

namespace {

/// The SIL verifier walks over a SIL function / basic block / instruction,
/// checking and enforcing its invariants.
class SILVerifier : public SILVerifierBase<SILVerifier> {
  Module *M;
  const SILFunction &F;
  Lowering::TypeConverter &TC;
  const SILInstruction *CurInstruction = nullptr;
  DominanceInfo *Dominance;

  SILVerifier(const SILVerifier&) = delete;
  void operator=(const SILVerifier&) = delete;
public:
  void _require(bool condition, const Twine &complaint,
                const std::function<void()> &extraContext = nullptr) {
    if (condition) return;

    llvm::dbgs() << "SIL verification failed: " << complaint << "\n";

    if (extraContext) extraContext();

    if (CurInstruction) {
      llvm::dbgs() << "Verifying instruction:\n";
      CurInstruction->printInContext(llvm::dbgs());
      llvm::dbgs() << "In function @" << F.getName() <<" basic block:\n";
      CurInstruction->getParent()->print(llvm::dbgs());
    } else {
      llvm::dbgs() << "In function @" << F.getName() <<" basic block:\n";
      F.print(llvm::dbgs());
    }

    abort();
  }
#define require(condition, complaint) \
  _require(bool(condition), complaint ": " #condition)

  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireObjectType(SILType type, const Twine &valueDescription,
                     const char *typeName) {
    _require(type.isObject(), valueDescription + " must be an object");
    auto result = type.getAs<T>();
    _require(bool(result), valueDescription + " must have type " + typeName);
    return result;
  }
  template <class T> typename CanTypeWrapperTraits<T>::type
  _requireObjectType(SILValue value, const Twine &valueDescription,
                     const char *typeName) {
    return _requireObjectType<T>(value.getType(), valueDescription, typeName);
  }
#define requireObjectType(type, value, valueDescription) \
  _requireObjectType<type>(value, valueDescription, #type)

  void requireReferenceValue(SILValue value, const Twine &valueDescription) {
    require(value.getType().isObject(), valueDescription +" must be an object");
    require(value.getType().hasReferenceSemantics(),
            valueDescription + " must have reference semantics");
  }

  /// Assert that two types are equal.
  void requireSameType(SILType type1, SILType type2, const Twine &complaint) {
    _require(type1 == type2, complaint, [&] {
      llvm::dbgs() << "  " << type1 << "\n  " << type2 << '\n';
    });
  }

  void requireSameFunctionComponents(CanSILFunctionType type1,
                                     CanSILFunctionType type2,
                                     const Twine &what) {
    require(type1->getInterfaceResult() == type2->getInterfaceResult(),
            "result types of " + what + " do not match");
    require(type1->getInterfaceParameters().size() ==
            type2->getInterfaceParameters().size(),
            "inputs of " + what + " do not match in count");
    for (auto i : indices(type1->getInterfaceParameters())) {
      require(type1->getInterfaceParameters()[i] ==
              type2->getInterfaceParameters()[i],
              "input " + Twine(i) + " of " + what + " do not match");
    }
  }

  SILVerifier(const SILFunction &F)
      : M(F.getModule().getSwiftModule()), F(F), TC(F.getModule().Types) {
    // Check to make sure that all blocks are well formed.  If not, the
    // SILVerifier object will explode trying to compute dominance info.
    for (auto &BB : F) {
      require(!BB.empty(), "Basic blocks cannot be empty");
      require(isa<TermInst>(BB.getInstList().back()),
              "Basic blocks must end with a terminator instruction");
    }

    Dominance = new DominanceInfo(const_cast<SILFunction*>(&F));
  }

  ~SILVerifier() {
    delete Dominance;
  }

  void visitSILArgument(SILArgument *arg) {
    checkLegalTypes(arg->getFunction(), arg);
  }

  void visitSILInstruction(SILInstruction *I) {
    CurInstruction = I;
    checkSILInstruction(I);

    // Check the SILLLocation attached to the instruction.
    checkInstructionsSILLocation(I);

    checkLegalTypes(I->getFunction(), I);
  }

  void checkSILInstruction(SILInstruction *I) {
    const SILBasicBlock *BB = I->getParent();
    // Check that non-terminators look ok.
    if (!isa<TermInst>(I)) {
      require(!BB->empty(), "Can't be in a parent block if it is empty");
      require(&*BB->getInstList().rbegin() != I,
              "Non-terminators cannot be the last in a block");
    } else {
      require(&*BB->getInstList().rbegin() == I,
              "Terminator must be the last in block");
    }

    // Verify that all of our uses are in this function.
    for (Operand *use : I->getUses()) {
      auto user = use->getUser();
      require(user, "instruction user is null?");
      require(isa<SILInstruction>(user),
              "instruction used by non-instruction");
      auto userI = cast<SILInstruction>(user);
      require(userI->getParent(),
              "instruction used by unparented instruction");
      require(userI->getParent()->getParent() == &F,
              "instruction used by instruction in different function");

      auto operands = userI->getAllOperands();
      require(operands.begin() <= use && use <= operands.end(),
              "use doesn't actually belong to instruction it claims to");
    }

    // Verify some basis structural stuff about an instruction's operands.
    for (auto &operand : I->getAllOperands()) {
      require(operand.get().isValid(), "instruction has null operand");

      if (auto *valueI = dyn_cast<SILInstruction>(operand.get())) {
        require(valueI->getParent(),
                "instruction uses value of unparented instruction");
        require(valueI->getParent()->getParent() == &F,
                "instruction uses value of instruction from another function");
        require(Dominance->properlyDominates(valueI, I),
                "instruction isn't dominated by its operand");
      }
      
      if (auto *valueBBA = dyn_cast<SILArgument>(operand.get())) {
        require(valueBBA->getParent(),
                "instruction uses value of unparented instruction");
        require(valueBBA->getParent()->getParent() == &F,
                "bb argument value from another function");
        require(Dominance->dominates(valueBBA->getParent(), I->getParent()),
                "instruction isn't dominated by its bb argument operand");
      }

      require(operand.getUser() == I,
              "instruction's operand's owner isn't the instruction");
      require(isInValueUses(&operand), "operand value isn't used by operand");

      // Make sure that if operand is generic that its primary archetypes match
      // the function context.
      checkLegalTypes(I->getFunction(), operand.get().getDef());
    }
  }

  void checkInstructionsSILLocation(SILInstruction *I) {
    SILLocation L = I->getLoc();
    SILLocation::LocationKind LocKind = L.getKind();
    ValueKind InstKind = I->getKind();

    // Regular locations and SIL file locations are allowed on all instructions.
    if (LocKind == SILLocation::RegularKind ||
        LocKind == SILLocation::SILFileKind)
      return;

    if (LocKind == SILLocation::CleanupKind ||
        LocKind == SILLocation::InlinedKind)
      require(InstKind != ValueKind::ReturnInst ||
              InstKind != ValueKind::AutoreleaseReturnInst,
        "cleanup and inlined locations are not allowed on return instructions");

    if (LocKind == SILLocation::ReturnKind ||
        LocKind == SILLocation::ImplicitReturnKind)
      require(InstKind == ValueKind::BranchInst ||
              InstKind == ValueKind::ReturnInst ||
              InstKind == ValueKind::AutoreleaseReturnInst ||
              InstKind == ValueKind::UnreachableInst,
        "return locations are only allowed on branch and return instructions");

    if (LocKind == SILLocation::ArtificialUnreachableKind)
      require(InstKind == ValueKind::UnreachableInst,
        "artificial locations are only allowed on Unreachable instructions");
  }

  /// Check that the types of this value producer are all legal in the function
  /// context in which it exists.
  void checkLegalTypes(SILFunction *F, ValueBase *value) {
    for (auto type : value->getTypes()) {
      checkLegalType(F, type);
    }
  }

  /// Check that the given type is a legal SIL value.
  void checkLegalType(SILFunction *F, SILType type) {
    auto rvalueType = type.getSwiftRValueType();
    require(!isa<LValueType>(rvalueType),
            "l-value types are not legal in SIL");
    require(!isa<AnyFunctionType>(rvalueType),
            "AST function types are not legal in SIL");

    rvalueType.visit([&](Type t) {
      auto *A = dyn_cast<ArchetypeType>(t.getPointer());
      if (!A)
        return;
      require(isArchetypeValidInFunction(A, F),
              "Operand is of an ArchetypeType that does not exist in the "
              "Caller's generic param list.");
    });
  }

  /// Check that this operand appears in the use-chain of the value it uses.
  static bool isInValueUses(const Operand *operand) {
    for (auto use : operand->get()->getUses())
      if (use == operand)
        return true;
    return false;
  }

  void checkAllocStackInst(AllocStackInst *AI) {
    require(AI->getContainerResult().getType().isLocalStorage(),
            "first result of alloc_stack must be local storage");
    require(AI->getAddressResult().getType().isAddress(),
            "second result of alloc_stack must be an address type");
    require(AI->getContainerResult().getType().getSwiftRValueType()
              == AI->getElementType().getSwiftRValueType(),
            "container storage must be for allocated type");
  }

  void checkAllocRefInst(AllocRefInst *AI) {
    requireReferenceValue(AI, "Result of alloc_ref");
  }

  void checkAllocRefDynamicInst(AllocRefDynamicInst *ARDI) {
    requireReferenceValue(ARDI, "Result of alloc_ref_dynamic");
    require(ARDI->getOperand().getType().is<AnyMetatypeType>(),
            "operand of alloc_ref_dynamic must be of metatype type");
    auto metaTy = ARDI->getOperand().getType().castTo<AnyMetatypeType>();
    require(metaTy->hasRepresentation(),
            "operand of alloc_ref_dynamic must have a metatype representation");
    if (ARDI->isObjC()) {
      require(metaTy->getRepresentation() == MetatypeRepresentation::ObjC,
              "alloc_ref_dynamic [objc] requires operand of ObjC metatype");
    } else {
      require(metaTy->getRepresentation() == MetatypeRepresentation::Thick,
              "alloc_ref_dynamic requires operand of thick metatype");
    }
  }

  /// Check the substitutions passed to an apply or partial_apply.
  CanSILFunctionType checkApplySubstitutions(ArrayRef<Substitution> subs,
                                             SILType calleeTy) {
    auto fnTy = requireObjectType(SILFunctionType, calleeTy, "callee operand");

    // If there are substitutions, verify them and apply them to the callee.
    if (subs.empty()) {
      require(!fnTy->isPolymorphic(),
              "callee of apply without substitutions must not be polymorphic");
      return fnTy;
    }
    require(fnTy->isPolymorphic(),
            "callee of apply with substitutions must be polymorphic");

    // Apply the substitutions.
    return fnTy->substInterfaceGenericArgs(F.getModule(), M, subs);
  }

  void checkApplyInst(ApplyInst *AI) {
    // If we have a substitution whose replacement type is an archetype, make
    // sure that the replacement archetype is in the context generic params of
    // the caller function.
    // For each substitution Sub in AI...
    for (auto &Sub : AI->getSubstitutions()) {
      // If Sub's replacement is not an archetype type or is from an opened
      // existential type, skip it...
      auto A = Sub.Replacement->getAs<ArchetypeType>();
      if (!A)
        continue;
      require(isArchetypeValidInFunction(A, AI->getFunction()),
              "Archetype to be substituted must be valid in function.");
    }

    // Then make sure that we have a type that can be substituted for the
    // callee.
    auto substTy = checkApplySubstitutions(AI->getSubstitutions(),
                                      AI->getCallee().getType());
    require(AI->getOrigCalleeType()->getAbstractCC() ==
            AI->getSubstCalleeType()->getAbstractCC(),
            "calling convention difference between types");

    require(!AI->getSubstCalleeType()->isPolymorphic(),
            "substituted callee type should not be generic");

    require(substTy == AI->getSubstCalleeType(),
            "substituted callee type does not match substitutions");

    // Check that the arguments and result match.
    require(AI->getArguments().size() ==
            substTy->getInterfaceParameters().size(),
            "apply doesn't have right number of arguments for function");
    for (size_t i = 0, size = AI->getArguments().size(); i < size; ++i) {
      requireSameType(AI->getArguments()[i].getType(),
                      substTy->getInterfaceParameters()[i].getSILType(),
                      "operand of 'apply' doesn't match function input type");
    }
    require(AI->getType() == substTy->getInterfaceResult().getSILType(),
            "type of apply instruction doesn't match function result type");
  }

  void checkPartialApplyInst(PartialApplyInst *PAI) {
    auto resultInfo = requireObjectType(SILFunctionType, PAI,
                                        "result of partial_apply");
    require(resultInfo->getExtInfo().hasContext(),
            "result of closure cannot have a thin function type");

    // If we have a substitution whose replacement type is an archetype, make
    // sure that the replacement archetype is in the context generic params of
    // the caller function.
    // For each substitution Sub in AI...
    for (auto &Sub : PAI->getSubstitutions()) {
      // If Sub's replacement is not an archetype type or is from an opened
      // existential type, skip it...
      Sub.Replacement.visit([&](Type t) {
        auto *A = t->getAs<ArchetypeType>();
        if (!A)
          return;
        require(isArchetypeValidInFunction(A, PAI->getFunction()),
                "Archetype to be substituted must be valid in function.");
      });
    }

    auto substTy = checkApplySubstitutions(PAI->getSubstitutions(),
                                        PAI->getCallee().getType());

    require(!PAI->getSubstCalleeType()->isPolymorphic(),
            "substituted callee type should not be generic");

    require(substTy == PAI->getSubstCalleeType(),
            "substituted callee type does not match substitutions");

    // The arguments must match the suffix of the original function's input
    // types.
    require(PAI->getArguments().size() +
              resultInfo->getInterfaceParameters().size()
              == substTy->getInterfaceParameters().size(),
            "result of partial_apply should take as many inputs as were not "
            "applied by the instruction");

    unsigned offset =
      substTy->getInterfaceParameters().size() - PAI->getArguments().size();

    for (unsigned i = 0, size = PAI->getArguments().size(); i < size; ++i) {
      require(PAI->getArguments()[i].getType()
                == substTy->getInterfaceParameters()[i + offset].getSILType(),
              "applied argument types do not match suffix of function type's "
              "inputs");
    }

    // The arguments to the result function type must match the prefix of the
    // original function's input types.
    for (unsigned i = 0, size = resultInfo->getInterfaceParameters().size();
         i < size; ++i) {
      require(resultInfo->getInterfaceParameters()[i] ==
              substTy->getInterfaceParameters()[i],
              "inputs to result function type do not match unapplied inputs "
              "of original function");
    }
    
    // The "returns inner pointer" convention doesn't survive through a partial
    // application, since the thunk takes responsibility for lifetime-extending
    // 'self'.
    auto expectedResult = substTy->getInterfaceResult();
    if (expectedResult.getConvention() == ResultConvention::UnownedInnerPointer)
    {
      expectedResult = SILResultInfo(expectedResult.getType(),
                                     ResultConvention::Unowned);
      require(resultInfo->getInterfaceResult() == expectedResult,
              "result type of result function type for partially applied "
              "@unowned_inner_pointer function should have @unowned convention");
    } else {
      require(resultInfo->getInterfaceResult() == expectedResult,
              "result type of result function type does not match original "
              "function");
    }
  }

  void checkBuiltinFunctionRefInst(BuiltinFunctionRefInst *BFI) {
    auto fnType = requireObjectType(SILFunctionType, BFI,
                                    "result of builtin_function_ref");
    require(fnType->getRepresentation()
              == FunctionType::Representation::Thin,
            "builtin_function_ref should have a thin function result");
  }

  bool isValidLinkageForTransparentRef(SILLinkage linkage) {
    switch (linkage) {
    case SILLinkage::Private:
    case SILLinkage::Hidden:
    case SILLinkage::HiddenExternal:
      return false;

    case SILLinkage::Public:
    case SILLinkage::PublicExternal:
    case SILLinkage::Shared:
      return true;

    }
  }

  void checkFunctionRefInst(FunctionRefInst *FRI) {
    auto fnType = requireObjectType(SILFunctionType, FRI,
                                    "result of function_ref");
    require(fnType->getRepresentation()
              == FunctionType::Representation::Thin,
            "function_ref should have a thin function result");
    if (F.isTransparent()) {
      require(isValidLinkageForTransparentRef(
                                    FRI->getReferencedFunction()->getLinkage())
                || FRI->getReferencedFunction()->isExternalDeclaration(),
              "function_ref inside transparent function cannot "
              "reference a private or hidden symbol");
    }
  }

  void checkGlobalAddrInst(GlobalAddrInst *GAI) {
    require(GAI->getType().isAddress(),
            "GlobalAddr must have an address result type");
    require(GAI->getGlobal()->hasStorage(),
            "GlobalAddr cannot take the address of a computed variable");
    require(!GAI->getGlobal()->getDeclContext()->isLocalContext(),
            "GlobalAddr cannot take the address of a local var");
  }

  void checkSILGlobalAddrInst(SILGlobalAddrInst *GAI) {
    require(GAI->getType().isAddress(),
            "SILGlobalAddr must have an address result type");
    require(GAI->getType().getObjectType() ==
              GAI->getReferencedGlobal()->getLoweredType(),
            "SILGlobalAddr must be the address type of the variable it "
            "references");
    if (F.isTransparent()) {
      require(isValidLinkageForTransparentRef(
                                      GAI->getReferencedGlobal()->getLinkage()),
              "function_ref inside transparent function cannot "
              "reference a private or hidden symbol");
    }
  }

  void checkIntegerLiteralInst(IntegerLiteralInst *ILI) {
    require(ILI->getType().is<BuiltinIntegerType>(),
            "invalid integer literal type");
  }
  void checkLoadInst(LoadInst *LI) {
    require(LI->getType().isObject(), "Result of load must be an object");
    require(LI->getOperand().getType().isAddress(),
            "Load operand must be an address");
    require(LI->getOperand().getType().getObjectType() == LI->getType(),
            "Load operand type and result type mismatch");
  }

  void checkStoreInst(StoreInst *SI) {
    require(SI->getSrc().getType().isObject(),
            "Can't store from an address source");
    require(SI->getDest().getType().isAddress(),
            "Must store to an address dest");
    require(SI->getDest().getType().getObjectType() == SI->getSrc().getType(),
            "Store operand type and dest type mismatch");
  }

  void checkAssignInst(AssignInst *AI) {
    SILValue Src = AI->getSrc(), Dest = AI->getDest();
    require(AI->getModule().getStage() == SILStage::Raw,
            "assign instruction can only exist in raw SIL");
    require(Src.getType().isObject(), "Can't assign from an address source");
    require(Dest.getType().isAddress(), "Must store to an address dest");
    require(Dest.getType().getObjectType() == Src.getType(),
            "Store operand type and dest type mismatch");
  }

  void checkMarkUninitializedInst(MarkUninitializedInst *MU) {
    SILValue Src = MU->getOperand();
    require(MU->getModule().getStage() == SILStage::Raw,
            "mark_uninitialized instruction can only exist in raw SIL");
    require(Src.getType().isAddress() ||
            Src.getType().getSwiftRValueType()->getClassOrBoundGenericClass(),
            "mark_uninitialized must be an address or class");
    require(Src.getType() == MU->getType(0),"operand and result type mismatch");
  }
  void checkMarkFunctionEscapeInst(MarkFunctionEscapeInst *MFE) {
    require(MFE->getModule().getStage() == SILStage::Raw,
            "mark_function_escape instruction can only exist in raw SIL");
    for (auto Elt : MFE->getElements())
      require(Elt.getType().isAddress(), "MFE must refer to variable addrs");
  }

  void checkCopyAddrInst(CopyAddrInst *SI) {
    require(SI->getSrc().getType().isAddress(),
            "Src value should be lvalue");
    require(SI->getDest().getType().isAddress(),
            "Dest address should be lvalue");
    require(SI->getDest().getType() == SI->getSrc().getType(),
            "Store operand type and dest type mismatch");
  }

  void checkRetainValueInst(RetainValueInst *I) {
    require(I->getOperand().getType().isObject(),
            "Source value should be an object value");
  }

  void checkReleaseValueInst(ReleaseValueInst *I) {
    require(I->getOperand().getType().isObject(),
            "Source value should be an object value");
  }
  
  void checkAutoreleaseValueInst(AutoreleaseValueInst *I) {
    require(I->getOperand().getType().isObject(),
            "Source value should be an object value");
    // TODO: This instruction could in principle be generalized.
    require(I->getOperand().getType().hasRetainablePointerRepresentation(),
            "Source value must be a reference type or optional thereof");
  }
  
  void checkCopyBlockInst(CopyBlockInst *I) {
    require(I->getOperand().getType().isBlockPointerCompatible(),
            "operand of copy_block should be a block");
    require(I->getOperand().getType() == I->getType(),
            "result of copy_block should be same type as operand");
  }
  
  void checkStructInst(StructInst *SI) {
    auto *structDecl = SI->getType().getStructOrBoundGenericStruct();
    require(structDecl, "StructInst must return a struct");
    require(SI->getType().isObject(),
            "StructInst must produce an object");

    SILType structTy = SI->getType();
    auto opi = SI->getElements().begin(), opEnd = SI->getElements().end();
    for (VarDecl *field : structDecl->getStoredProperties()) {
      require(opi != opEnd,
              "number of struct operands does not match number of stored "
              "member variables of struct");

      SILType loweredType = structTy.getFieldType(field, F.getModule());
      require((*opi).getType() == loweredType,
              "struct operand type does not match field type");
      ++opi;
    }
  }

  void checkEnumInst(EnumInst *UI) {
    EnumDecl *ud = UI->getType().getEnumOrBoundGenericEnum();
    require(ud, "EnumInst must return an enum");
    require(UI->getElement()->getParentEnum() == ud,
            "EnumInst case must be a case of the result enum type");
    require(UI->getType().isObject(),
            "EnumInst must produce an object");
    require(UI->hasOperand() == UI->getElement()->hasArgumentType(),
            "EnumInst must take an argument iff the element does");

    if (UI->getElement()->hasArgumentType()) {
      require(UI->getOperand().getType().isObject(),
              "EnumInst operand must be an object");
      SILType caseTy = UI->getType().getEnumElementType(UI->getElement(),
                                                        F.getModule());
      require(caseTy == UI->getOperand().getType(),
              "EnumInst operand type does not match type of case");
    }
  }

  void checkInitEnumDataAddrInst(InitEnumDataAddrInst *UI) {
    EnumDecl *ud = UI->getOperand().getType().getEnumOrBoundGenericEnum();
    require(ud, "InitEnumDataAddrInst must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "InitEnumDataAddrInst case must be a case of the enum operand type");
    require(UI->getElement()->hasArgumentType(),
            "InitEnumDataAddrInst case must have a data type");
    require(UI->getOperand().getType().isAddress(),
            "InitEnumDataAddrInst must take an address operand");
    require(UI->getType().isAddress(),
            "InitEnumDataAddrInst must produce an address");

    SILType caseTy =
      UI->getOperand().getType().getEnumElementType(UI->getElement(),
                                                    F.getModule());
    require(caseTy == UI->getType(),
            "InitEnumDataAddrInst result does not match type of enum case");
  }

  void checkUncheckedEnumDataInst(UncheckedEnumDataInst *UI) {
    EnumDecl *ud = UI->getOperand().getType().getEnumOrBoundGenericEnum();
    require(ud, "UncheckedEnumData must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "UncheckedEnumData case must be a case of the enum operand type");
    require(UI->getElement()->hasArgumentType(),
            "UncheckedEnumData case must have a data type");
    require(UI->getOperand().getType().isObject(),
            "UncheckedEnumData must take an address operand");
    require(UI->getType().isObject(),
            "UncheckedEnumData must produce an address");

    SILType caseTy =
      UI->getOperand().getType().getEnumElementType(UI->getElement(),
                                                    F.getModule());
    require(caseTy == UI->getType(),
            "UncheckedEnumData result does not match type of enum case");
  }

  void checkUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *UI) {
    EnumDecl *ud = UI->getOperand().getType().getEnumOrBoundGenericEnum();
    require(ud, "UncheckedTakeEnumDataAddrInst must take an enum operand");
    require(UI->getElement()->getParentEnum() == ud,
            "UncheckedTakeEnumDataAddrInst case must be a case of the enum operand type");
    require(UI->getElement()->hasArgumentType(),
            "UncheckedTakeEnumDataAddrInst case must have a data type");
    require(UI->getOperand().getType().isAddress(),
            "UncheckedTakeEnumDataAddrInst must take an address operand");
    require(UI->getType().isAddress(),
            "UncheckedTakeEnumDataAddrInst must produce an address");

    SILType caseTy =
      UI->getOperand().getType().getEnumElementType(UI->getElement(),
                                                    F.getModule());
    require(caseTy == UI->getType(),
            "UncheckedTakeEnumDataAddrInst result does not match type of enum case");
  }

  void checkInjectEnumAddrInst(InjectEnumAddrInst *IUAI) {
    require(IUAI->getOperand().getType().is<EnumType>()
              || IUAI->getOperand().getType().is<BoundGenericEnumType>(),
            "InjectEnumAddrInst must take an enum operand");
    require(IUAI->getElement()->getParentEnum()
              == IUAI->getOperand().getType().getEnumOrBoundGenericEnum(),
            "InjectEnumAddrInst case must be a case of the enum operand type");
    require(IUAI->getOperand().getType().isAddress(),
            "InjectEnumAddrInst must take an address operand");
  }

  void checkTupleInst(TupleInst *TI) {
    CanTupleType ResTy = requireObjectType(TupleType, TI, "Result of tuple");

    require(TI->getElements().size() == ResTy->getFields().size(),
            "Tuple field count mismatch!");

    for (size_t i = 0, size = TI->getElements().size(); i < size; ++i) {
      require(TI->getElements()[i].getType().getSwiftType()
               ->isEqual(ResTy.getElementType(i)),
              "Tuple element arguments do not match tuple type!");
    }
  }

  void checkMetatypeInst(MetatypeInst *MI) {
    require(MI->getType(0).is<MetatypeType>(),
            "metatype instruction must be of metatype type");
    require(MI->getType(0).castTo<MetatypeType>()->hasRepresentation(),
            "metatype instruction must have a metatype representation");
  }
  void checkValueMetatypeInst(ValueMetatypeInst *MI) {
    require(MI->getType().is<MetatypeType>(),
            "value_metatype instruction must be of metatype type");
    require(MI->getType().castTo<MetatypeType>()->hasRepresentation(),
            "value_metatype instruction must have a metatype representation");
    require(MI->getOperand().getType().getSwiftRValueType() ==
            CanType(MI->getType().castTo<MetatypeType>()->getInstanceType()),
            "value_metatype result must be metatype of operand type");
  }
  void checkExistentialMetatypeInst(ExistentialMetatypeInst *MI) {
    require(MI->getType().is<ExistentialMetatypeType>(),
            "existential_metatype instruction must be of metatype type");
    require(MI->getType().castTo<ExistentialMetatypeType>()->hasRepresentation(),
            "value_metatype instruction must have a metatype representation");
    require(MI->getOperand().getType().isAnyExistentialType(),
            "existential_metatype operand must be of protocol type");
    require(MI->getOperand().getType().getSwiftRValueType() ==
            MI->getType().castTo<ExistentialMetatypeType>().getInstanceType(),
            "existential_metatype result must be metatype of operand type");
  }

  void checkStrongRetainInst(StrongRetainInst *RI) {
    requireReferenceValue(RI->getOperand(), "Operand of strong_retain");
  }
  void checkStrongRetainAutoreleasedInst(StrongRetainAutoreleasedInst *RI) {
    require(RI->getOperand().getType().isObject(),
            "Operand of strong_retain_autoreleased must be an object");
    require(RI->getOperand().getType().hasRetainablePointerRepresentation(),
            "Operand of strong_retain_autoreleased must be a retainable pointer");
    require(isa<ApplyInst>(RI->getOperand()),
            "Operand of strong_retain_autoreleased must be the return value of "
            "an apply instruction");
  }
  void checkStrongReleaseInst(StrongReleaseInst *RI) {
    requireReferenceValue(RI->getOperand(), "Operand of release");
  }
  void checkStrongRetainUnownedInst(StrongRetainUnownedInst *RI) {
    requireObjectType(UnownedStorageType, RI->getOperand(),
                      "Operand of retain_unowned");
  }
  void checkUnownedRetainInst(UnownedRetainInst *RI) {
    requireObjectType(UnownedStorageType, RI->getOperand(),
                      "Operand of unowned_retain");
  }
  void checkUnownedReleaseInst(UnownedReleaseInst *RI) {
    requireObjectType(UnownedStorageType, RI->getOperand(),
                      "Operand of unowned_release");
  }
  void checkDeallocStackInst(DeallocStackInst *DI) {
    require(DI->getOperand().getType().isLocalStorage(),
            "Operand of dealloc_stack must be local storage");
  }
  void checkDeallocRefInst(DeallocRefInst *DI) {
    require(DI->getOperand().getType().isObject(),
            "Operand of dealloc_ref must be object");
    require(DI->getOperand().getType().getClassOrBoundGenericClass(),
            "Operand of dealloc_ref must be of class type");
  }
  void checkDeallocBoxInst(DeallocBoxInst *DI) {
    require(DI->getElementType().isObject(),
            "Element type of dealloc_box must be an object type");
    requireObjectType(BuiltinNativeObjectType, DI->getOperand(),
                      "Operand of dealloc_box");
  }
  void checkDestroyAddrInst(DestroyAddrInst *DI) {
    require(DI->getOperand().getType().isAddress(),
            "Operand of destroy_addr must be address");
  }

  void checkIndexAddrInst(IndexAddrInst *IAI) {
    require(IAI->getType().isAddress(), "index_addr must produce an address");
    require(IAI->getType() == IAI->getBase().getType(),
            "index_addr must produce an address of the same type as its base");
    require(IAI->getIndex().getType().is<BuiltinIntegerType>(),
            "index_addr index must be of a builtin integer type");
  }

  void checkIndexRawPointerInst(IndexRawPointerInst *IAI) {
    require(IAI->getType().is<BuiltinRawPointerType>(),
            "index_raw_pointer must produce a RawPointer");
    require(IAI->getBase().getType().is<BuiltinRawPointerType>(),
            "index_raw_pointer base must be a RawPointer");
    require(IAI->getIndex().getType().is<BuiltinIntegerType>(),
            "index_raw_pointer index must be of a builtin integer type");
  }

  void checkTupleExtractInst(TupleExtractInst *EI) {
    CanTupleType operandTy = requireObjectType(TupleType, EI->getOperand(),
                                               "Operand of tuple_extract");
    require(EI->getType().isObject(),
            "result of tuple_extract must be object");

    require(EI->getFieldNo() < operandTy->getNumElements(),
            "invalid field index for element_addr instruction");
    require(EI->getType().getSwiftRValueType()
            == operandTy.getElementType(EI->getFieldNo()),
            "type of tuple_element_addr does not match type of element");
  }

  void checkStructExtractInst(StructExtractInst *EI) {
    SILType operandTy = EI->getOperand().getType();
    require(operandTy.isObject(),
            "cannot struct_extract from address");
    require(EI->getType().isObject(),
            "result of struct_extract cannot be address");
    StructDecl *sd = operandTy.getStructOrBoundGenericStruct();
    require(sd, "must struct_extract from struct");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot load computed property with struct_extract");

    require(EI->getField()->getDeclContext() == sd,
            "struct_extract field is not a member of the struct");

    SILType loweredFieldTy = operandTy.getFieldType(EI->getField(),
                                                    F.getModule());
    require(loweredFieldTy == EI->getType(),
            "result of struct_extract does not match type of field");
  }

  void checkTupleElementAddrInst(TupleElementAddrInst *EI) {
    SILType operandTy = EI->getOperand().getType();
    require(operandTy.isAddress(),
            "must derive element_addr from address");
    require(!operandTy.hasReferenceSemantics(),
            "cannot derive tuple_element_addr from reference type");
    require(EI->getType(0).isAddress(),
            "result of tuple_element_addr must be address");
    require(operandTy.is<TupleType>(),
            "must derive tuple_element_addr from tuple");

    ArrayRef<TupleTypeElt> fields = operandTy.castTo<TupleType>()->getFields();
    require(EI->getFieldNo() < fields.size(),
            "invalid field index for element_addr instruction");
    require(EI->getType().getSwiftRValueType()
              == CanType(fields[EI->getFieldNo()].getType()),
            "type of tuple_element_addr does not match type of element");
  }

  void checkStructElementAddrInst(StructElementAddrInst *EI) {
    SILType operandTy = EI->getOperand().getType();
    require(operandTy.isAddress(),
            "must derive struct_element_addr from address");
    StructDecl *sd = operandTy.getStructOrBoundGenericStruct();
    require(sd, "struct_element_addr operand must be struct address");
    require(EI->getType(0).isAddress(),
            "result of struct_element_addr must be address");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot get address of computed property with struct_element_addr");

    require(EI->getField()->getDeclContext() == sd,
            "struct_element_addr field is not a member of the struct");

    SILType loweredFieldTy = operandTy.getFieldType(EI->getField(),
                                                    F.getModule());
    require(loweredFieldTy == EI->getType(),
            "result of struct_element_addr does not match type of field");
  }

  void checkRefElementAddrInst(RefElementAddrInst *EI) {
    requireReferenceValue(EI->getOperand(), "Operand of ref_element_addr");
    require(EI->getType(0).isAddress(),
            "result of ref_element_addr must be lvalue");
    require(!EI->getField()->isStatic(),
            "cannot get address of static property with struct_element_addr");
    require(EI->getField()->hasStorage(),
            "cannot get address of computed property with ref_element_addr");
    SILType operandTy = EI->getOperand().getType();
    ClassDecl *cd = operandTy.getClassOrBoundGenericClass();
    require(cd, "ref_element_addr operand must be a class instance");

    require(EI->getField()->getDeclContext() == cd,
            "ref_element_addr field must be a member of the class");

    SILType loweredFieldTy = operandTy.getFieldType(EI->getField(),
                                                    F.getModule());
    require(loweredFieldTy == EI->getType(),
            "result of ref_element_addr does not match type of field");
  }

  SILType getMethodSelfType(CanSILFunctionType ft) {
    return ft->getInterfaceParameters().back().getSILType();
  }
  CanType getMethodSelfInstanceType(CanSILFunctionType ft) {
    auto selfTy = getMethodSelfType(ft);
    if (auto metaTy = selfTy.getAs<AnyMetatypeType>())
      return metaTy.getInstanceType();
    return selfTy.getSwiftRValueType();
  }

  void checkWitnessMethodInst(WitnessMethodInst *AMI) {
    auto methodType = requireObjectType(SILFunctionType, AMI,
                                        "result of witness_method");

    auto *protocol
      = dyn_cast<ProtocolDecl>(AMI->getMember().getDecl()->getDeclContext());
    require(protocol,
            "witness_method method must be a protocol method");

    require(methodType->getRepresentation() == FunctionType::Representation::Thin,
            "result of witness_method must be thin function");

    require(methodType->getAbstractCC()
              == F.getModule().Types.getProtocolWitnessCC(protocol),
            "result of witness_method must have correct @cc for protocol");

    require(methodType->isPolymorphic(),
            "result of witness_method must be polymorphic");

    auto selfGenericParam
      = methodType->getGenericSignature()->getGenericParams()[0];
    require(selfGenericParam->getDepth() == 0
            && selfGenericParam->getIndex() == 0,
            "method should be polymorphic on Self parameter at depth 0 index 0");
    auto selfMarker
      = methodType->getGenericSignature()->getRequirements()[0];
    require(selfMarker.getKind() == RequirementKind::WitnessMarker
            && selfMarker.getFirstType()->isEqual(selfGenericParam),
            "method's Self parameter should appear first in requirements");
    auto selfRequirement
      = methodType->getGenericSignature()->getRequirements()[1];
    require(selfRequirement.getKind() == RequirementKind::Conformance
            && selfRequirement.getFirstType()->isEqual(selfGenericParam)
            && selfRequirement.getSecondType()->getAs<ProtocolType>()
              ->getDecl() == protocol,
            "method's Self parameter should be constrained by protocol");

    if (AMI->getLookupType().is<ArchetypeType>()) {
      require(AMI->getConformance() == nullptr,
              "archetype lookup should have null conformance");
    } else {
      require(AMI->getConformance(),
              "concrete type lookup requires conformance");
      require(AMI->getConformance()->getType()
                ->isEqual(AMI->getLookupType().getSwiftRValueType()),
              "concrete type lookup requires conformance that matches type");
      // We allow for null conformances.
      require(!AMI->getConformance() ||
              AMI->getModule().lookUpWitnessTable(AMI->getConformance(),
                                                  false).first,
              "Could not find witness table for conformance.");
    }
  }

  bool isSelfArchetype(CanType t, ArrayRef<ProtocolDecl*> protocols) {
    ArchetypeType *archetype = dyn_cast<ArchetypeType>(t);
    if (!archetype)
      return false;

    auto selfProto = archetype->getSelfProtocol();
    if (!selfProto)
      return false;

    for (auto checkProto : protocols) {
      if (checkProto == selfProto || checkProto->inheritsFrom(selfProto))
        return true;
    }

    return false;
  }

  bool isOpenedArchetype(CanType t) {
    ArchetypeType *archetype = dyn_cast<ArchetypeType>(t);
    if (!archetype)
      return false;

    return !archetype->getOpenedExistentialType().isNull();
  }

  void checkProtocolMethodInst(ProtocolMethodInst *EMI) {
    auto methodType = requireObjectType(SILFunctionType, EMI,
                                        "result of protocol_method");

    auto proto = dyn_cast<ProtocolDecl>(EMI->getMember().getDecl()
                                        ->getDeclContext());
    require(proto, "protocol_method must take a method of a protocol");
    SILType operandType = EMI->getOperand().getType();

    require(methodType->getAbstractCC()
              == F.getModule().Types.getProtocolWitnessCC(proto),
            "result of protocol_method must have correct @cc for protocol");
    
    if (EMI->getMember().isForeign) {
      require(methodType->getRepresentation() == FunctionType::Representation::Thin,
              "result of foreign protocol_method must be thin");
    } else {
      require(methodType->getRepresentation() == FunctionType::Representation::Thick,
              "result of native protocol_method must be thick");
    }
    
    if (EMI->getMember().getDecl()->isInstanceMember()) {
      require(operandType.isExistentialType(),
              "instance protocol_method must apply to an existential");
      SILType selfType = getMethodSelfType(methodType);
      if (!operandType.isClassExistentialType()) {
        require(selfType.isAddress(),
                "protocol_method result must take its self parameter "
                "by address");
      }
      CanType selfObjType = selfType.getSwiftRValueType();
      require(isSelfArchetype(selfObjType, proto),
              "result must be a method of protocol's Self archetype");
    } else {
      require(operandType.isObject(),
              "static protocol_method cannot apply to an address");
      require(operandType.is<ExistentialMetatypeType>(),
              "static protocol_method must apply to an existential metatype");
      require(operandType.castTo<ExistentialMetatypeType>()
                .getInstanceType().isExistentialType(),
              "static protocol_method must apply to an existential metatype");
      require(getMethodSelfType(methodType) == EMI->getOperand().getType(),
              "result must be a method of the existential metatype");
    }
  }
  
  // Get the expected type of a dynamic method reference.
  SILType getDynamicMethodType(SILType selfType, SILDeclRef method) {
    auto &C = F.getASTContext();
    
    // The type of the dynamic method must match the usual type of the method,
    // but with the more opaque Self type.
    auto methodTy = F.getModule().Types.getConstantType(method)
      .castTo<SILFunctionType>();
    
    auto params = methodTy->getInterfaceParameters();
    SmallVector<SILParameterInfo, 4>
      dynParams(params.begin(), params.end() - 1);
    dynParams.push_back(SILParameterInfo(selfType.getSwiftType(),
                                             params.back().getConvention()));
    
    auto dynResult = methodTy->getInterfaceResult();
    // If the method returns Self, substitute AnyObject for the result type.
    if (auto fnDecl = dyn_cast<FuncDecl>(method.getDecl())) {
      if (fnDecl->hasDynamicSelf()) {
        auto anyObjectTy = C.getProtocol(KnownProtocolKind::AnyObject)
                             ->getDeclaredType();
        auto newResultTy
          = dynResult.getType()->replaceCovariantResultType(anyObjectTy, 0);
        dynResult = SILResultInfo(newResultTy->getCanonicalType(),
                                  dynResult.getConvention());
      }
    }
    
    auto fnTy = SILFunctionType::get(nullptr,
                                     methodTy->getExtInfo(),
                                     methodTy->getCalleeConvention(),
                                     dynParams,
                                     dynResult,
                                     F.getASTContext());
    return SILType::getPrimitiveObjectType(fnTy);
  }
  
  void checkDynamicMethodInst(DynamicMethodInst *EMI) {
    requireObjectType(SILFunctionType, EMI, "result of dynamic_method");
    SILType operandType = EMI->getOperand().getType();

    require(EMI->getMember().getDecl()->isObjC(), "method must be [objc]");
    if (EMI->getMember().getDecl()->isInstanceMember()) {
      require(operandType.getSwiftType()->is<BuiltinUnknownObjectType>(),
              "operand must have Builtin.UnknownObject type");
    } else {
      require(operandType.getSwiftType()->is<ExistentialMetatypeType>(),
              "operand must have metatype type");
      require(operandType.getSwiftType()->castTo<ExistentialMetatypeType>()
                ->getInstanceType()->is<ProtocolType>(),
              "operand must have metatype of protocol type");
      require(operandType.getSwiftType()->castTo<ExistentialMetatypeType>()
                ->getInstanceType()->castTo<ProtocolType>()->getDecl()
                ->isSpecificProtocol(KnownProtocolKind::AnyObject),
              "operand must have metatype of AnyObject type");
    }
    
    requireSameType(EMI->getType(),
                    getDynamicMethodType(operandType, EMI->getMember()),
                    "result must be of the method's type");
  }

  static bool isClassOrClassMetatype(Type t) {
    if (auto *meta = t->getAs<AnyMetatypeType>()) {
      return bool(meta->getInstanceType()->getClassOrBoundGenericClass());
    } else {
      return bool(t->getClassOrBoundGenericClass());
    }
  }

  static bool isClassOrClassMetatype(SILType t) {
    return t.isObject() && isClassOrClassMetatype(t.getSwiftRValueType());
  }

  void checkClassMethodInst(ClassMethodInst *CMI) {
    require(CMI->getType() == TC.getConstantType(CMI->getMember()),
            "result type of class_method must match type of method");
    auto methodType = requireObjectType(SILFunctionType, CMI,
                                        "result of class_method");
    require(methodType->getRepresentation() == FunctionType::Representation::Thin,
            "result method must be of a thin function type");
    SILType operandType = CMI->getOperand().getType();
    require(isClassOrClassMetatype(operandType.getSwiftType()),
            "operand must be of a class type");
    require(isClassOrClassMetatype(getMethodSelfType(methodType)),
            "result must be a method of a class");
  }

  void checkSuperMethodInst(SuperMethodInst *CMI) {
    require(CMI->getType() == TC.getConstantType(CMI->getMember()),
            "result type of super_method must match type of method");
    auto methodType = requireObjectType(SILFunctionType, CMI,
                                        "result of super_method");
    require(methodType->getRepresentation() == FunctionType::Representation::Thin,
            "result method must be of a thin function type");
    SILType operandType = CMI->getOperand().getType();
    require(isClassOrClassMetatype(operandType.getSwiftType()),
            "operand must be of a class type");
    require(isClassOrClassMetatype(getMethodSelfType(methodType)),
            "result must be a method of a class");

    Type methodClass;
    auto decl = CMI->getMember().getDecl();
    if (auto classDecl = dyn_cast<ClassDecl>(decl))
      methodClass = classDecl->getDeclaredTypeInContext();
    else
      methodClass = decl->getDeclContext()->getDeclaredTypeInContext();

    require(methodClass->getClassOrBoundGenericClass(),
            "super_method must look up a class method");
    require(!methodClass->isEqual(operandType.getSwiftType()),
            "super_method operand should be a subtype of the "
            "lookup class type");
  }

  void checkProjectExistentialInst(ProjectExistentialInst *PEI) {
    SILType operandType = PEI->getOperand().getType();
    require(operandType.isAddress(),
            "project_existential must be applied to address");

    SmallVector<ProtocolDecl*, 4> protocols;
    require(operandType.getSwiftRValueType().isExistentialType(protocols),
            "project_existential must be applied to address of existential");
    require(PEI->getType().isAddress(),
            "project_existential result must be an address");

    require(isSelfArchetype(PEI->getType().getSwiftRValueType(), protocols),
            "project_existential result must be Self archetype of one of "
            "its protocols");
  }

  void checkProjectExistentialRefInst(ProjectExistentialRefInst *PEI) {
    SILType operandType = PEI->getOperand().getType();
    require(operandType.isObject(),
            "project_existential_ref operand must not be address");
    SmallVector<ProtocolDecl*, 4> protocols;
    require(operandType.getSwiftRValueType().isExistentialType(protocols),
            "project_existential must be applied to existential");
    require(operandType.isClassExistentialType(),
            "project_existential_ref operand must be class existential");

    require(isSelfArchetype(PEI->getType().getSwiftRValueType(), protocols),
            "project_existential_ref result must be Self archetype of one of "
            "its protocols");
  }

  void checkOpenExistentialInst(OpenExistentialInst *OEI) {
    SILType operandType = OEI->getOperand().getType();
    require(operandType.isAddress(),
            "open_existential must be applied to address");

    SmallVector<ProtocolDecl*, 4> protocols;
    require(operandType.getSwiftRValueType().isExistentialType(protocols),
            "open_existential must be applied to address of existential");
    require(OEI->getType().isAddress(),
            "open_existential result must be an address");

    require(isOpenedArchetype(OEI->getType().getSwiftRValueType()),
            "open_existential result must be an opened existential archetype");
  }

  void checkOpenExistentialRefInst(OpenExistentialRefInst *OEI) {
    SILType operandType = OEI->getOperand().getType();
    require(operandType.isObject(),
            "open_existential_ref operand must not be address");

    CanType instanceTy = operandType.getSwiftType();
    bool isOperandMetatype = false;
    if (auto metaTy = dyn_cast<AnyMetatypeType>(instanceTy)) {
      instanceTy = metaTy.getInstanceType();
      isOperandMetatype = true;
    }

    require(instanceTy.isExistentialType(),
            "open_existential_ref must be applied to existential or metatype "
            "thereof");
    require(isOperandMetatype || instanceTy->isClassExistentialType(),
            "open_existential_ref operand must be class existential or "
            "metatype");

    CanType resultInstanceTy = OEI->getType().getSwiftRValueType();
    if (auto resultMetaTy = dyn_cast<MetatypeType>(resultInstanceTy)) {
      require(isOperandMetatype, 
              "open_existential_ref result is a metatype but operand is not");
      require(resultMetaTy->hasRepresentation(),
              "open_existential_ref result metatype must have a "
              "representation");
      require(operandType.is<ExistentialMetatypeType>(),
              "open_existential_ref yielding metatype should operate on "
              "an existential metatype");
      require(resultMetaTy->getRepresentation() == 
              operandType.castTo<ExistentialMetatypeType>()->getRepresentation(),
              "open_existential_ref result and operand metatypes must have the "
              "same representation");
              
      resultInstanceTy = resultMetaTy.getInstanceType();
    } else {
      require(!isOperandMetatype, 
              "open_existential_ref operand is a metatype but result is not");
    }

    require(isOpenedArchetype(resultInstanceTy),
            "open_existential_ref result must be an opened existential "
            "archetype or metatype thereof");
  }

  void checkInitExistentialInst(InitExistentialInst *AEI) {
    SILType exType = AEI->getOperand().getType();
    require(exType.isAddress(),
            "init_existential must be applied to an address");
    require(exType.isExistentialType(),
            "init_existential must be applied to address of existential");
    require(!exType.isClassExistentialType(),
            "init_existential must be applied to non-class existential");
    require(!AEI->getConcreteType().isExistentialType(),
            "init_existential cannot put an existential container inside "
            "an existential container");

    for (ProtocolConformance *C : AEI->getConformances())
      // We allow for null conformances.
      require(!C || AEI->getModule().lookUpWitnessTable(C, false).first,
              "Could not find witness table for conformance.");
  }

  void checkInitExistentialRefInst(InitExistentialRefInst *IEI) {
    SILType concreteType = IEI->getOperand().getType();
    require(concreteType.getSwiftType()->mayHaveSuperclass(),
            "init_existential_ref operand must be a class instance");
    require(IEI->getType().isClassExistentialType(),
            "init_existential_ref result must be a class existential type");
    require(IEI->getType().isObject(),
            "init_existential_ref result must not be an address");
    for (ProtocolConformance *C : IEI->getConformances())
      // We allow for null conformances.
      require(!C || IEI->getModule().lookUpWitnessTable(C, false).first,
              "Could not find witness table for conformance.");
  }

  void checkUpcastExistentialInst(UpcastExistentialInst *UEI) {
    SILType srcType = UEI->getSrcExistential().getType();
    SILType destType = UEI->getDestExistential().getType();
    require(srcType != destType,
            "can't upcast_existential to same type");
    require(srcType.isExistentialType(),
            "upcast_existential source must be existential");
    require(destType.isAddress(),
            "upcast_existential dest must be an address");
    require(destType.isExistentialType(),
            "upcast_existential dest must be address of existential");
    require(!destType.isClassExistentialType(),
            "upcast_existential dest must be non-class existential");
  }

  void checkUpcastExistentialRefInst(UpcastExistentialRefInst *UEI) {
    require(UEI->getOperand().getType() != UEI->getType(),
            "can't upcast_existential_ref to same type");
    require(UEI->getOperand().getType().isObject(),
            "upcast_existential_ref operand must not be an address");
    require(UEI->getOperand().getType().isClassExistentialType(),
            "upcast_existential_ref operand must be class existential");
    require(UEI->getType().isObject(),
            "upcast_existential_ref result must not be an address");
    require(UEI->getType().isClassExistentialType(),
            "upcast_existential_ref result must be class existential");
  }

  void checkDeinitExistentialInst(DeinitExistentialInst *DEI) {
    SILType exType = DEI->getOperand().getType();
    require(exType.isAddress(),
            "deinit_existential must be applied to an address");
    require(exType.isExistentialType(),
            "deinit_existential must be applied to address of existential");
    require(!exType.isClassExistentialType(),
            "deinit_existential must be applied to non-class existential");
  }

  void verifyCheckedCast(CheckedCastKind kind, SILType fromTy, SILType toTy) {
    // Verify common invariants.
    require(fromTy != toTy, "can't checked cast to same type");
    require(fromTy.isAddress() == toTy.isAddress(),
            "address-ness of checked cast src and dest must match");

    switch (kind) {
    case CheckedCastKind::Unresolved:
    case CheckedCastKind::Coercion:
      llvm_unreachable("invalid for SIL");
    case CheckedCastKind::Downcast:
      require(fromTy.getClassOrBoundGenericClass(),
              "downcast operand must be a class type");
      require(toTy.getClassOrBoundGenericClass(),
              "downcast must convert to a class type");
      require(fromTy.isSuperclassOf(toTy),
              "downcast must convert to a subclass");
      return;
    case CheckedCastKind::SuperToArchetype: {
      require(fromTy.isObject(),
              "super_to_archetype operand must be an object");
      require(fromTy.getClassOrBoundGenericClass(),
              "super_to_archetype operand must be a class instance");
      auto archetype = toTy.getAs<ArchetypeType>();
      require(archetype, "super_to_archetype must convert to archetype type");
      require(archetype->requiresClass(),
              "super_to_archetype must convert to class archetype type");
      return;
    }
    case CheckedCastKind::ArchetypeToConcrete: {
      require(fromTy.getAs<ArchetypeType>(),
              "archetype_to_concrete must convert from archetype type");
      return;
    }
    case CheckedCastKind::ArchetypeToArchetype: {
      require(fromTy.getAs<ArchetypeType>(),
              "archetype_to_archetype must convert from archetype type");
      require(toTy.getAs<ArchetypeType>(),
              "archetype_to_archetype must convert to archetype type");
      return;
    }
    case CheckedCastKind::ExistentialToArchetype: {
      require(fromTy.isExistentialType(),
              "existential_to_archetype must convert from protocol type");
      require(toTy.getAs<ArchetypeType>(),
              "existential_to_archetype must convert to archetype type");
      return;
    }
    case CheckedCastKind::ExistentialToConcrete: {
      require(fromTy.isExistentialType(),
              "existential_to_concrete must convert from protocol type");
      return;
    }
    case CheckedCastKind::ConcreteToArchetype: {
      require(toTy.getAs<ArchetypeType>(),
              "concrete_to_archetype must convert to archetype type");
      return;
    }
    case CheckedCastKind::ConcreteToUnrelatedExistential: {
      require(toTy.isExistentialType(),
              "concrete_to_existential must convert to protocol type");
      return;
    }
    }
  }

  void checkUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *CI) {
    verifyCheckedCast(CI->getCastKind(),
                      CI->getOperand().getType(),
                      CI->getType());
  }

  void checkCheckedCastBranchInst(CheckedCastBranchInst *CBI) {
    verifyCheckedCast(CBI->getCastKind(),
                      CBI->getOperand().getType(),
                      CBI->getCastType());

    require(CBI->getSuccessBB()->bbarg_size() == 1,
            "success dest of checked_cast_br must take one argument");
    require(CBI->getSuccessBB()->bbarg_begin()[0]->getType()
              == CBI->getCastType(),
            "success dest block argument of checked_cast_br must match type of cast");
    require(CBI->getFailureBB()->bbarg_empty(),
            "failure dest of checked_cast_br must take no arguments");
  }

  void checkThinToThickFunctionInst(ThinToThickFunctionInst *TTFI) {
    auto opFTy = requireObjectType(SILFunctionType, TTFI->getOperand(),
                                   "thin_to_thick_function operand");
    auto resFTy = requireObjectType(SILFunctionType, TTFI,
                                    "thin_to_thick_function result");
    require(opFTy->isPolymorphic() == resFTy->isPolymorphic(),
            "thin_to_thick_function operand and result type must differ only "
            " in thinness");
    requireSameFunctionComponents(opFTy, resFTy,
                                  "thin_to_thick_function operand and result");

    require(opFTy->getRepresentation() == FunctionType::Representation::Thin,
            "operand of thin_to_thick_function must be thin");
    require(resFTy->getRepresentation() == FunctionType::Representation::Thick,
            "result of thin_to_thick_function must be thick");

    auto adjustedOperandExtInfo = opFTy->getExtInfo().withRepresentation(
                                           FunctionType::Representation::Thick);
    require(adjustedOperandExtInfo == resFTy->getExtInfo(),
            "operand and result of thin_to_think_function must agree in particulars");
  }

  void checkThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCI) {
    auto opTy = requireObjectType(AnyMetatypeType, TTOCI->getOperand(),
                                  "thick_to_objc_metatype operand");
    auto resTy = requireObjectType(AnyMetatypeType, TTOCI,
                                   "thick_to_objc_metatype result");

    require(TTOCI->getOperand().getType().is<MetatypeType>() ==
            TTOCI->getType().is<MetatypeType>(),
            "thick_to_objc_metatype cannot change metatype kinds");
    require(opTy->getRepresentation() == MetatypeRepresentation::Thick,
            "operand of thick_to_objc_metatype must be thick");
    require(resTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "operand of thick_to_objc_metatype must be ObjC");

    require(opTy->getInstanceType()->isEqual(resTy->getInstanceType()),
            "thick_to_objc_metatype instance types do not match");
  }

  void checkObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTI) {
    auto opTy = requireObjectType(AnyMetatypeType, OCTTI->getOperand(),
                                  "objc_to_thick_metatype operand");
    auto resTy = requireObjectType(AnyMetatypeType, OCTTI,
                                   "objc_to_thick_metatype result");

    require(OCTTI->getOperand().getType().is<MetatypeType>() ==
            OCTTI->getType().is<MetatypeType>(),
            "objc_to_thick_metatype cannot change metatype kinds");
    require(opTy->getRepresentation() == MetatypeRepresentation::ObjC,
            "operand of objc_to_thick_metatype must be ObjC");
    require(resTy->getRepresentation() == MetatypeRepresentation::Thick,
            "operand of objc_to_thick_metatype must be thick");

    require(opTy->getInstanceType()->isEqual(resTy->getInstanceType()),
            "objc_to_thick_metatype instance types do not match");
  }

  void checkRefToUnownedInst(RefToUnownedInst *I) {
    requireReferenceValue(I->getOperand(), "Operand of ref_to_unowned");
    auto operandType = I->getOperand().getType().getSwiftRValueType();
    auto resultType = requireObjectType(UnownedStorageType, I,
                                        "Result of ref_to_unowned");
    require(resultType.getReferentType() == operandType,
            "Result of ref_to_unowned does not have the "
            "operand's type as its referent type");
  }

  void checkUnownedToRefInst(UnownedToRefInst *I) {
    auto operandType = requireObjectType(UnownedStorageType,
                                         I->getOperand(),
                                         "Operand of unowned_to_ref");
    requireReferenceValue(I, "Result of unowned_to_ref");
    auto resultType = I->getType().getSwiftRValueType();
    require(operandType.getReferentType() == resultType,
            "Operand of unowned_to_ref does not have the "
            "operand's type as its referent type");
  }

  void checkRefToUnmanagedInst(RefToUnmanagedInst *I) {
    requireReferenceValue(I->getOperand(), "Operand of ref_to_unmanaged");
    auto operandType = I->getOperand().getType().getSwiftRValueType();
    auto resultType = requireObjectType(UnmanagedStorageType, I,
                                        "Result of ref_to_unmanaged");
    require(resultType.getReferentType() == operandType,
            "Result of ref_to_unmanaged does not have the "
            "operand's type as its referent type");
  }

  void checkUnmanagedToRefInst(UnmanagedToRefInst *I) {
    auto operandType = requireObjectType(UnmanagedStorageType,
                                         I->getOperand(),
                                         "Operand of unmanaged_to_ref");
    requireReferenceValue(I, "Result of unmanaged_to_ref");
    auto resultType = I->getType().getSwiftRValueType();
    require(operandType.getReferentType() == resultType,
            "Operand of unmanaged_to_ref does not have the "
            "operand's type as its referent type");
  }

  void checkUpcastInst(UpcastInst *UI) {
    require(UI->getType() != UI->getOperand().getType(),
            "can't upcast to same type");
    // FIXME: Existential metatype upcasts should have their own instruction.
    // For now accept them blindly.
    if (UI->getType().is<ExistentialMetatypeType>()) {
      require(UI->getOperand().getType().is<AnyMetatypeType>(),
              "must upcast existential metatype from metatype");
      require(UI->getOperand().getType().castTo<AnyMetatypeType>()
                ->getRepresentation() == MetatypeRepresentation::Thick,
              "must upcast existential metatype from thick metatype");
      return;
    }
    
    if (UI->getType().is<MetatypeType>()) {
      CanType instTy(UI->getType().castTo<MetatypeType>()->getInstanceType());
      
      if (instTy->isExistentialType())
        return;
      
      require(UI->getOperand().getType().is<MetatypeType>(),
              "upcast operand must be a class or class metatype instance");
      CanType opInstTy(UI->getOperand().getType().castTo<MetatypeType>()
                         ->getInstanceType());
      require(instTy->getClassOrBoundGenericClass(),
              "upcast must convert a class metatype to a class metatype");
      require(instTy->isSuperclassOf(opInstTy, nullptr),
              "upcast must cast to a superclass or an existential metatype");
    } else {
      require(UI->getType().getClassOrBoundGenericClass(),
              "upcast must convert a class instance to a class type");
      require(UI->getType().isSuperclassOf(UI->getOperand().getType()),
              "upcast must cast to a superclass");
    }
  }

  void checkIsNonnullInst(IsNonnullInst *II) {
    require(II->getOperand().getType().getSwiftType()
              ->mayHaveSuperclass(),
            "isa operand must be a class type");
  }

  void checkAddressToPointerInst(AddressToPointerInst *AI) {
    require(AI->getOperand().getType().isAddress(),
            "address-to-pointer operand must be an address");
    require(AI->getType().getSwiftType()->isEqual(
                              AI->getType().getASTContext().TheRawPointerType),
            "address-to-pointer result type must be RawPointer");
  }
  
  bool isHeapObjectReferenceType(SILType silTy) {
    auto &C = silTy.getASTContext();
    if (silTy.getSwiftRValueType()->mayHaveSuperclass())
      return true;
    if (silTy.getSwiftRValueType()->isEqual(C.TheNativeObjectType))
      return true;
    if (silTy.getSwiftRValueType()->isEqual(C.TheUnknownObjectType))
      return true;
    // TODO: AnyObject type, @objc-only existentials in general
    return false;
  }

  void checkUncheckedRefCastInst(UncheckedRefCastInst *AI) {
    require(AI->getOperand().getType().isObject(),
            "unchecked_ref_cast operand must be a value");
    require(isHeapObjectReferenceType(AI->getOperand().getType()),
            "unchecked_ref_cast operand must be a heap object reference");
    require(AI->getType().isObject(),
            "unchecked_ref_cast result must be an object");
    require(isHeapObjectReferenceType(AI->getType()),
            "unchecked_ref_cast result must be a heap object reference");
  }
  
  void checkUncheckedAddrCastInst(UncheckedAddrCastInst *AI) {
    require(AI->getOperand().getType().isAddress(),
            "unchecked_addr_cast operand must be an address");
    require(AI->getType().isAddress(),
            "unchecked_addr_cast result must be an address");
  }

  void checkRefToRawPointerInst(RefToRawPointerInst *AI) {
    require(AI->getOperand().getType()
              .getSwiftType()->mayHaveSuperclass() ||
            AI->getOperand().getType().getSwiftType()->isEqual(
                            AI->getType().getASTContext().TheNativeObjectType),
            "ref-to-raw-pointer operand must be a class reference or"
            " NativeObject");
    require(AI->getType().getSwiftType()->isEqual(
                            AI->getType().getASTContext().TheRawPointerType),
            "ref-to-raw-pointer result must be RawPointer");
  }

  void checkRawPointerToRefInst(RawPointerToRefInst *AI) {
    require(AI->getType()
              .getSwiftType()->mayHaveSuperclass() ||
            AI->getType().getSwiftType()->isEqual(
                            AI->getType().getASTContext().TheNativeObjectType),
        "raw-pointer-to-ref result must be a class reference or NativeObject");
    require(AI->getOperand().getType().getSwiftType()->isEqual(
                            AI->getType().getASTContext().TheRawPointerType),
            "raw-pointer-to-ref operand must be NativeObject");
  }

  void checkConvertFunctionInst(ConvertFunctionInst *ICI) {
    auto opTI = requireObjectType(SILFunctionType, ICI->getOperand(),
                                  "convert_function operand");
    auto resTI = requireObjectType(SILFunctionType, ICI,
                                   "convert_function operand");

    // convert_function is required to be a no-op conversion.

    require(opTI->getAbstractCC() == resTI->getAbstractCC(),
            "convert_function cannot change function cc");
    require(opTI->getRepresentation() == resTI->getRepresentation(),
            "convert_function cannot change function representation");
  }

  void checkCondFailInst(CondFailInst *CFI) {
    require(CFI->getOperand().getType()
              == SILType::getBuiltinIntegerType(1, F.getASTContext()),
            "cond_fail operand must be a Builtin.Int1");
  }

  void checkReturnInst(ReturnInst *RI) {
    DEBUG(RI->print(llvm::dbgs()));

    CanSILFunctionType ti = F.getLoweredFunctionType();
    SILType functionResultType
      = F.mapTypeIntoContext(ti->getInterfaceResult().getSILType());
    SILType instResultType = RI->getOperand().getType();
    DEBUG(llvm::dbgs() << "function return type: ";
          functionResultType.dump();
          llvm::dbgs() << "return inst type: ";
          instResultType.dump(););
    require(functionResultType == instResultType,
            "return value type does not match return type of function");
  }

  void checkAutoreleaseReturnInst(AutoreleaseReturnInst *RI) {
    DEBUG(RI->print(llvm::dbgs()));

    CanSILFunctionType ti = F.getLoweredFunctionType();
    SILType functionResultType
      = F.mapTypeIntoContext(ti->getInterfaceResult().getSILType());
    SILType instResultType = RI->getOperand().getType();
    DEBUG(llvm::dbgs() << "function return type: ";
          functionResultType.dump();
          llvm::dbgs() << "return inst type: ";
          instResultType.dump(););
    require(functionResultType == instResultType,
            "return value type does not match return type of function");
    require(instResultType.isObject(),
            "autoreleased return value cannot be an address");
    require(instResultType.hasRetainablePointerRepresentation(),
            "autoreleased return value must be a reference type");
  }

  void checkSwitchIntInst(SwitchIntInst *SII) {
    requireObjectType(BuiltinIntegerType, SII->getOperand(),
                      "switch_int operand");

    auto ult = [](const APInt &a, const APInt &b) { return a.ult(b); };
    std::set<APInt, decltype(ult)> cases(ult);

    for (unsigned i = 0, e = SII->getNumCases(); i < e; ++i) {
      APInt value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SII->getCase(i);

      require(!cases.count(value),
              "multiple switch_int cases for same value");
      cases.insert(value);

      require(dest->bbarg_empty(),
              "switch_int case destination cannot take arguments");
    }
    if (SII->hasDefault())
      require(SII->getDefaultBB()->bbarg_empty(),
              "switch_int default destination cannot take arguments");
  }

  void checkSwitchEnumInst(SwitchEnumInst *SOI) {
    require(SOI->getOperand().getType().isObject(),
            "switch_enum operand must be an object");

    SILType uTy = SOI->getOperand().getType();
    EnumDecl *uDecl = uTy.getEnumOrBoundGenericEnum();
    require(uDecl, "switch_enum operand is not an enum");

    // Find the set of enum elements for the type so we can verify
    // exhaustiveness.
    // FIXME: We also need to consider if the enum is resilient, in which case
    // we're never guaranteed to be exhaustive.
    llvm::DenseSet<EnumElementDecl*> unswitchedElts;
    uDecl->getAllElements(unswitchedElts);

    // Verify the set of enum cases we dispatch on.
    for (unsigned i = 0, e = SOI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI->getCase(i);

      require(elt->getDeclContext() == uDecl,
              "switch_enum dispatches on enum element that is not part of "
              "its type");
      require(unswitchedElts.count(elt),
              "switch_enum dispatches on same enum element more than once");
      unswitchedElts.erase(elt);

      // The destination BB can take the argument payload, if any, as a BB
      // arguments, or it can ignore it and take no arguments.
      if (elt->hasArgumentType()) {
        require(dest->getBBArgs().size() == 0
                  || dest->getBBArgs().size() == 1,
                "switch_enum destination for case w/ args must take 0 or 1 "
                "arguments");

        if (dest->getBBArgs().size() == 1) {
          SILType eltArgTy = uTy.getEnumElementType(elt, F.getModule());
          SILType bbArgTy = dest->getBBArgs()[0]->getType();
          require(eltArgTy == bbArgTy,
                  "switch_enum destination bbarg must match case arg type");
          require(!dest->getBBArgs()[0]->getType().isAddress(),
                  "switch_enum destination bbarg type must not be an address");
        }

      } else {
        require(dest->getBBArgs().size() == 0,
                "switch_enum destination for no-argument case must take no "
                "arguments");
      }
    }

    // If the switch is non-exhaustive, we require a default.
    require(unswitchedElts.empty() || SOI->hasDefault(),
            "nonexhaustive switch_enum must have a default destination");
    if (SOI->hasDefault())
      require(SOI->getDefaultBB()->bbarg_empty(),
              "switch_enum default destination must take no arguments");
  }

  void checkSwitchEnumAddrInst(SwitchEnumAddrInst *SOI){
    require(SOI->getOperand().getType().isAddress(),
            "switch_enum_addr operand must be an object");

    SILType uTy = SOI->getOperand().getType();
    EnumDecl *uDecl = uTy.getEnumOrBoundGenericEnum();
    require(uDecl, "switch_enum_addr operand must be an enum");

    // Find the set of enum elements for the type so we can verify
    // exhaustiveness.
    // FIXME: We also need to consider if the enum is resilient, in which case
    // we're never guaranteed to be exhaustive.
    llvm::DenseSet<EnumElementDecl*> unswitchedElts;
    uDecl->getAllElements(unswitchedElts);

    // Verify the set of enum cases we dispatch on.
    for (unsigned i = 0, e = SOI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI->getCase(i);

      require(elt->getDeclContext() == uDecl,
              "switch_enum_addr dispatches on enum element that "
              "is not part of its type");
      require(unswitchedElts.count(elt),
              "switch_enum_addr dispatches on same enum element "
              "more than once");
      unswitchedElts.erase(elt);

      // The destination BB must not have BB arguments.
      require(dest->getBBArgs().size() == 0,
              "switch_enum_addr destination must take no BB args");
    }

    // If the switch is non-exhaustive, we require a default.
    require(unswitchedElts.empty() || SOI->hasDefault(),
            "nonexhaustive switch_enum_addr must have a default "
            "destination");
    if (SOI->hasDefault())
      require(SOI->getDefaultBB()->bbarg_empty(),
              "switch_enum_addr default destination must take "
              "no arguments");
  }

  void checkBranchInst(BranchInst *BI) {
    require(BI->getArgs().size() == BI->getDestBB()->bbarg_size(),
            "branch has wrong number of arguments for dest bb");
    require(std::equal(BI->getArgs().begin(), BI->getArgs().end(),
                      BI->getDestBB()->bbarg_begin(),
                      [](SILValue branchArg, SILArgument *bbArg) {
                        return branchArg.getType() == bbArg->getType();
                      }),
            "branch argument types do not match arguments for dest bb");
  }

  void checkCondBranchInst(CondBranchInst *CBI) {
    require(CBI->getCondition().getType() ==
             SILType::getBuiltinIntegerType(1,
                                 CBI->getCondition().getType().getASTContext()),
            "condition of conditional branch must have Int1 type");

    require(CBI->getTrueArgs().size() == CBI->getTrueBB()->bbarg_size(),
            "true branch has wrong number of arguments for dest bb");
    require(std::equal(CBI->getTrueArgs().begin(), CBI->getTrueArgs().end(),
                      CBI->getTrueBB()->bbarg_begin(),
                      [](SILValue branchArg, SILArgument *bbArg) {
                        return branchArg.getType() == bbArg->getType();
                      }),
            "true branch argument types do not match arguments for dest bb");

    require(CBI->getFalseArgs().size() == CBI->getFalseBB()->bbarg_size(),
            "false branch has wrong number of arguments for dest bb");
    require(std::equal(CBI->getFalseArgs().begin(), CBI->getFalseArgs().end(),
                      CBI->getFalseBB()->bbarg_begin(),
                      [](SILValue branchArg, SILArgument *bbArg) {
                        return branchArg.getType() == bbArg->getType();
                      }),
            "false branch argument types do not match arguments for dest bb");
  }

  void checkDynamicMethodBranchInst(DynamicMethodBranchInst *DMBI) {
    SILType operandType = DMBI->getOperand().getType();

    require(DMBI->getMember().getDecl()->isObjC(), "method must be [objc]");
    if (DMBI->getMember().getDecl()->isInstanceMember()) {
      require(operandType.getSwiftType()->is<BuiltinUnknownObjectType>(),
              "operand must have Builtin.UnknownObject type");
    } else {
      require(operandType.getSwiftType()->is<ExistentialMetatypeType>(),
              "operand must have metatype type");
      require(operandType.getSwiftType()->castTo<ExistentialMetatypeType>()
              ->getInstanceType()->is<ProtocolType>(),
              "operand must have metatype of protocol type");
      require(operandType.getSwiftType()->castTo<ExistentialMetatypeType>()
              ->getInstanceType()->castTo<ProtocolType>()->getDecl()
              ->isSpecificProtocol(KnownProtocolKind::AnyObject),
              "operand must have metatype of AnyObject type");
    }

    // Check that the branch argument is of the expected dynamic method type.
    require(DMBI->getHasMethodBB()->bbarg_size() == 1,
            "true bb for dynamic_method_br must take an argument");
    
    requireSameType(DMBI->getHasMethodBB()->bbarg_begin()[0]->getType(),
                    getDynamicMethodType(operandType, DMBI->getMember()),
              "bb argument for dynamic_method_br must be of the method's type");
  }
  
  void checkProjectBlockStorageInst(ProjectBlockStorageInst *PBSI) {
    require(PBSI->getOperand().getType().isAddress(),
            "operand must be an address");
    auto storageTy = PBSI->getOperand().getType().getAs<SILBlockStorageType>();
    require(storageTy, "operand must be a @block_storage type");
    
    require(PBSI->getType().isAddress(),
            "result must be an address");
    auto captureTy = PBSI->getType().getSwiftRValueType();
    require(storageTy->getCaptureType() == captureTy,
            "result must be the capture type of the @block_storage type");
  }
  
  void checkInitBlockStorageHeaderInst(InitBlockStorageHeaderInst *IBSHI) {
    require(IBSHI->getBlockStorage().getType().isAddress(),
            "block storage operand must be an address");
    auto storageTy
      = IBSHI->getBlockStorage().getType().getAs<SILBlockStorageType>();
    require(storageTy, "block storage operand must be a @block_storage type");
    
    require(IBSHI->getInvokeFunction().getType().isObject(),
            "invoke function operand must be a value");
    auto invokeTy
      = IBSHI->getInvokeFunction().getType().getAs<SILFunctionType>();
    require(invokeTy, "invoke function operand must be a function");
    require(invokeTy->getRepresentation() == FunctionType::Representation::Thin,
            "invoke function operand must be a thin function");
    require(invokeTy->getAbstractCC() == AbstractCC::C,
            "invoke function operand must be a cdecl function");
    require(invokeTy->getInterfaceParameters().size() >= 1,
            "invoke function must take at least one parameter");
    auto storageParam = invokeTy->getInterfaceParameters()[0];
    require(storageParam.getConvention() == ParameterConvention::Indirect_Inout,
            "invoke function must take block storage as @inout parameter");
    require(storageParam.getType() == storageTy,
            "invoke function must take block storage type as first parameter");
    
    require(IBSHI->getType().isObject(), "result must be a value");
    auto blockTy = IBSHI->getType().getAs<SILFunctionType>();
    require(blockTy, "result must be a function");
    require(blockTy->getAbstractCC() == AbstractCC::C,
            "result must be a cdecl block function");
    require(blockTy->getRepresentation() == FunctionType::Representation::Block,
            "result must be a cdecl block function");
    require(blockTy->getInterfaceResult() == invokeTy->getInterfaceResult(),
            "result must have same return type as invoke function");
    
    require(blockTy->getInterfaceParameters().size() + 1
              == invokeTy->getInterfaceParameters().size(),
          "result must match all parameters of invoke function but the first");
    auto blockParams = blockTy->getInterfaceParameters();
    auto invokeBlockParams = invokeTy->getInterfaceParameters().slice(1);
    for (unsigned i : indices(blockParams)) {
      require(blockParams[i] == invokeBlockParams[i],
          "result must match all parameters of invoke function but the first");
    }
  }

  void verifyEntryPointArguments(SILBasicBlock *entry) {
    SILFunctionType *ti = F.getLoweredFunctionType();

    DEBUG(llvm::dbgs() << "Argument types for entry point BB:\n";
          for (auto *arg : make_range(entry->bbarg_begin(), entry->bbarg_end()))
            arg->getType().dump();
          llvm::dbgs() << "Input types for SIL function type ";
          ti->print(llvm::dbgs());
          llvm::dbgs() << ":\n";
          for (auto input : ti->getInterfaceParameters())
            input.getSILType().dump(););

    require(entry->bbarg_size() == ti->getInterfaceParameters().size(),
            "entry point has wrong number of arguments");


    require(std::equal(entry->bbarg_begin(), entry->bbarg_end(),
                      ti->getInterfaceParameterSILTypes().begin(),
                      [&](SILArgument *bbarg, SILType ty) {
                        return bbarg->getType() == F.mapTypeIntoContext(ty);
                      }),
            "entry point argument types do not match function type");
  }

  void verifyEpilogBlock(SILFunction *F) {
    bool FoundEpilogBlock = false;
    for (auto &BB : *F) {
      if (isa<ReturnInst>(BB.getTerminator())) {
        require(FoundEpilogBlock == false,
                "more than one function epilog block");
        FoundEpilogBlock = true;
      }
    }
  }

  void verifyStackHeight(SILBasicBlock *BB,
       llvm::DenseMap<SILBasicBlock*, std::vector<AllocStackInst*>> &visitedBBs,
       std::vector<AllocStackInst*> stack) {

    auto found = visitedBBs.find(BB);
    if (found != visitedBBs.end()) {
      // Check that the stack height is consistent coming from all entry points
      // into this BB.
      require(stack == found->second,
             "inconsistent stack heights entering basic block");
      return;
    } else {
      visitedBBs.insert({BB, stack});
    }

    for (SILInstruction &i : *BB) {
      CurInstruction = &i;

      if (auto alloc = dyn_cast<AllocStackInst>(&i)) {
        stack.push_back(alloc);
      }
      if (auto dealloc = dyn_cast<DeallocStackInst>(&i)) {
        SILValue op = dealloc->getOperand();
        require(op.getResultNumber() == 0,
               "dealloc_stack operand is not local storage of alloc_inst");
        require(!stack.empty(),
               "dealloc_stack with empty stack");
        require(op.getDef() == stack.back(),
               "dealloc_stack does not match most recent alloc_stack");
        stack.pop_back();
      }
      if (isa<ReturnInst>(&i) || isa<AutoreleaseReturnInst>(&i)) {
        require(stack.empty(),
                "return with alloc_stacks that haven't been deallocated");
      }
      if (auto term = dyn_cast<TermInst>(&i)) {
        for (auto &successor : term->getSuccessors()) {
          verifyStackHeight(successor.getBB(), visitedBBs, stack);
        }
      }
    }
  }

  void visitSILBasicBlock(SILBasicBlock *BB) {
    // Make sure that each of the successors/predecessors of this basic block
    // have this basic block in its predecessor/successor list.
    for (const SILSuccessor &S : BB->getSuccs()) {
      SILBasicBlock *SuccBB = S.getBB();
      bool FoundSelfInSuccessor = false;
      for (const SILBasicBlock *PredBB : SuccBB->getPreds()) {
        if (PredBB == BB) {
          FoundSelfInSuccessor = true;
          break;
        }
      }
      require(FoundSelfInSuccessor, "Must be a predecessor of each successor.");
    }

    for (const SILBasicBlock *PredBB : BB->getPreds()) {
      bool FoundSelfInPredecessor = false;
      for (const SILSuccessor &S : PredBB->getSuccs()) {
        if (S.getBB() == BB) {
          FoundSelfInPredecessor = true;
          break;
        }
      }
      require(FoundSelfInPredecessor, "Must be a successor of each predecessor.");
    }
    
    SILVisitor::visitSILBasicBlock(BB);
  }

  void visitSILFunction(SILFunction *F) {
    PrettyStackTraceSILFunction stackTrace("verifying", F);

    if (F->getLoweredFunctionType()->isPolymorphic()) {
      require(F->getContextGenericParams(),
              "generic function definition must have context archetypes");
    }

    verifyEntryPointArguments(F->getBlocks().begin());
    verifyEpilogBlock(F);

    llvm::DenseMap<SILBasicBlock*, std::vector<AllocStackInst*>> visitedBBs;
    verifyStackHeight(F->begin(), visitedBBs, {});

    SILVisitor::visitSILFunction(F);
  }

  void verify() {
    visitSILFunction(const_cast<SILFunction*>(&F));
  }
};
} // end anonymous namespace

#undef require
#undef requireObjectType
#endif //NDEBUG

/// verify - Run the SIL verifier to make sure that the SILFunction follows
/// invariants.
void SILFunction::verify() const {
#ifndef NDEBUG
  if (isExternalDeclaration()) {
    assert(isAvailableExternally() &&
           "external declaration of internal SILFunction not allowed");
    return;
  }
  SILVerifier(*this).verify();
#endif
}

/// Verify that a vtable follows invariants.
void SILVTable::verify(const SILModule &M) const {
#ifndef NDEBUG
  for (auto &entry : getEntries()) {
    // All vtable entries must be decls in a class context.
    assert(entry.first.hasDecl() && "vtable entry is not a decl");
    ValueDecl *decl = entry.first.getDecl();
    auto theClass = dyn_cast_or_null<ClassDecl>(decl->getDeclContext());
    assert(theClass && "vtable entry must refer to a class member");

    // The class context must be the vtable's class, or a superclass thereof.
    auto c = getClass();
    do {
      if (c == theClass)
        break;
      if (auto ty = c->getSuperclass())
        c = ty->getClassOrBoundGenericClass();
      else
        c = nullptr;
    } while (c);
    assert(c && "vtable entry must refer to a member of the vtable's class");

    // All function vtable entries must be at their natural uncurry level.
    // FIXME: We should change this to uncurry level 1.
    assert(!entry.first.isCurried && "vtable entry must not be curried");

    // Foreign entry points shouldn't appear in vtables.
    assert(!entry.first.isForeign && "vtable entry must not be foreign");

    // TODO: Verify that property entries are dynamically dispatched under our
    // finalized property dynamic dispatch rules.
  }
#endif
}

/// Verify that a witness table follows invariants.
void SILWitnessTable::verify(const SILModule &M) const {
#ifndef NDEBUG
  if (isDeclaration())
    assert(getEntries().size() == 0 &&
           "A witness table declaration should not have any entries.");

  // Currently all witness tables have public conformances, thus witness tables
  // should not reference SILFunctions without public/public_external linkage.
  // FIXME: Once we support private conformances, update this.
  for (const Entry &E : getEntries())
    if (E.getKind() == SILWitnessTable::WitnessKind::Method) {
      SILFunction *F = E.getMethodWitness().Witness;
      assert(!isLessVisibleThan(F->getLinkage(), getLinkage()) &&
             "Witness tables should not reference less visible functions.");
    }
#endif
}

/// Verify that a global variable follows invariants.
void SILGlobalVariable::verify() const {
#ifndef NDEBUG
  assert(getLoweredType().isObject()
         && "global variable cannot have address type");
#endif
}

/// Verify the module.
void SILModule::verify() const {
#ifndef NDEBUG
  // Uniquing set to catch symbol name collisions.
  llvm::StringSet<> symbolNames;

  // Check all functions.
  for (const SILFunction &f : *this) {
    if (!symbolNames.insert(f.getName())) {
      llvm::errs() << "Symbol redefined: " << f.getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    f.verify();
  }

  // Check all globals.
  for (const SILGlobalVariable &g : getSILGlobals()) {
    if (!symbolNames.insert(g.getName())) {
      llvm::errs() << "Symbol redefined: " << g.getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    g.verify();
  }

  // Check all vtables.
  llvm::DenseSet<ClassDecl*> vtableClasses;
  for (const SILVTable &vt : getVTables()) {
    if (!vtableClasses.insert(vt.getClass()).second) {
      llvm::errs() << "Vtable redefined: " << vt.getClass()->getName() << "!\n";
      assert(false && "triggering standard assertion failure routine");
    }
    vt.verify(*this);
  }

  // Check all witness tables.
  DEBUG(llvm::dbgs() << "*** Checking witness tables for duplicates ***\n");
  llvm::DenseSet<NormalProtocolConformance*> wtableConformances;
  for (const SILWitnessTable &wt : getWitnessTables()) {
    DEBUG(llvm::dbgs() << "Witness Table:\n"; wt.dump());
    auto conformance = wt.getConformance();
    if (!wtableConformances.insert(conformance).second) {
      llvm::errs() << "Witness table redefined: ";
      conformance->printName(llvm::errs());
      assert(false && "triggering standard assertion failure routine");
    }
    wt.verify(*this);
  }
#endif
}
