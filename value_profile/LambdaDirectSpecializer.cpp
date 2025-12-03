#include "LambdaDirectSpecializer.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lambdaopt {

// Direct-call specialization: if (capture == HotValue) call HotClone() else OriginalOp().
//
// We only specialize a single capture field given by Key.FieldIndex,
// but this works fine even if the closure has multiple captures in the
// struct; we just ignore the others here.
static bool specializeConstructionAndCall(Function *OriginalOp, Function *HotClone, StructType *ClosureTy,
                                          const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
    if (!OriginalOp || !HotClone || !ClosureTy)
        return false;

    bool Changed = false;
    LLVMContext &Ctx = OriginalOp->getContext();

    // Collect all direct call sites to OriginalOp (ignore indirect calls etc.)
    SmallVector<CallInst *, 8> CallSites;
    for (User *U : OriginalOp->users()) {
        if (auto *CI = dyn_cast<CallInst>(U)) {
            if (CI->getCalledFunction() == OriginalOp)
                CallSites.push_back(CI);
        }
    }

    for (CallInst *CI : CallSites) {
        if (CI->arg_empty()) {
            // We expect the first argument to be the lambda "this" pointer.
            continue;
        }

        // 1) Get the closure pointer ("this") and strip bitcasts etc.
        Value *ThisArg = CI->getArgOperand(0);
        Value *ThisBase = stripPointerCasts(ThisArg);

        // 2) Build a GEP to the specific capture field we’re specializing.
        if (Key.FieldIndex >= ClosureTy->getNumElements()) {
            errs() << "[lambda-opt] Field index " << Key.FieldIndex << " out of range for closure '"
                   << ClosureTy->getName() << "'; skipping call site.\n";
            continue;
        }

        Type *FieldTy = ClosureTy->getElementType(Key.FieldIndex);

        IRBuilder<> BeforeCall(CI);
        Value *FieldPtr = BeforeCall.CreateStructGEP(ClosureTy,             // struct type
                                                     ThisBase,              // pointer to closure
                                                     Key.FieldIndex,        // field index
                                                     "lambda.capture.ptr"); // name

        // Load the capture value *at* the call site.
        LoadInst *Load = BeforeCall.CreateLoad(FieldTy, FieldPtr, "lambda.capture");

        Value *CapturedVal = Load;

        // 3) Build the "hot" constant of the correct type.
        Value *HotConst = nullptr;
        if (auto *IntTy = dyn_cast<IntegerType>(FieldTy)) {
            APInt HotAP(IntTy->getBitWidth(), static_cast<uint64_t>(Prof.HotValue),
                        /*isSigned=*/true);
            HotConst = ConstantInt::get(IntTy, HotAP);
        }
        else if (FieldTy->isFloatingPointTy()) {
            HotConst = ConstantFP::get(FieldTy, static_cast<double>(Prof.HotValue));
        }
        else {
            errs() << "[lambda-opt] Capture field " << Key.FieldIndex << " in closure '" << ClosureTy->getName()
                   << "' is neither int nor FP; skipping call site.\n";
            continue;
        }

        // 4) Split block at the call instruction.
        BasicBlock *OrigBB = CI->getParent();
        BasicBlock *MergeBB = OrigBB->splitBasicBlock(CI, "lambda.merge");

        // The split leaves a dummy unconditional branch at the end of OrigBB.
        Instruction *OldTerm = OrigBB->getTerminator();

        // 5) Build comparison (int: icmp, double: fcmp).
        IRBuilder<> CmpBuilder(OldTerm);
        Value *IsHot = nullptr;

        if (FieldTy->isIntegerTy()) {
            IsHot = CmpBuilder.CreateICmpEQ(CapturedVal, HotConst, "lambda.is_hot");
        }
        else { // floating point
            IsHot = CmpBuilder.CreateFCmpOEQ(CapturedVal, HotConst, "lambda.is_hot");
        }

        // 6) Create hot and cold blocks.
        Function *ParentF = OrigBB->getParent();
        auto *HotBB = BasicBlock::Create(Ctx, "lambda.hot", ParentF, MergeBB);
        auto *ColdBB = BasicBlock::Create(Ctx, "lambda.cold", ParentF, MergeBB);

        // Replace the dummy branch with a conditional branch.
        OldTerm->eraseFromParent();
        IRBuilder<> BrBuilder(OrigBB);
        BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

        // 7) Prepare argument list for both calls.
        SmallVector<Value *, 8> Args;
        Args.reserve(CI->arg_size());
        for (unsigned i = 0; i < CI->arg_size(); ++i)
            Args.push_back(CI->getArgOperand(i));

        Type *RetTy = OriginalOp->getReturnType();
        Value *HotCall = nullptr;
        Value *ColdCall = nullptr;

        // Hot path: call the specialized clone.
        {
            IRBuilder<> HotBuilder(HotBB);
            if (RetTy->isVoidTy()) {
                HotBuilder.CreateCall(HotClone, Args);
                HotBuilder.CreateBr(MergeBB);
            }
            else {
                HotCall = HotBuilder.CreateCall(HotClone, Args, "lambda.call.hot");
                HotBuilder.CreateBr(MergeBB);
            }
        }

        // Cold path: call the original operator().
        {
            IRBuilder<> ColdBuilder(ColdBB);
            if (RetTy->isVoidTy()) {
                ColdBuilder.CreateCall(OriginalOp, Args);
                ColdBuilder.CreateBr(MergeBB);
            }
            else {
                ColdCall = ColdBuilder.CreateCall(OriginalOp, Args, "lambda.call.cold");
                ColdBuilder.CreateBr(MergeBB);
            }
        }

        // 8) If the call returns a value, merge with a PHI in MergeBB.
        if (!RetTy->isVoidTy()) {
            IRBuilder<> MergeBuilder(MergeBB);
            MergeBuilder.SetInsertPoint(&*MergeBB->begin());

            PHINode *PHI = MergeBuilder.CreatePHI(RetTy, 2, "lambda.call.sel");
            PHI->addIncoming(HotCall, HotBB);
            PHI->addIncoming(ColdCall, ColdBB);

            CI->replaceAllUsesWith(PHI);
        }

        // 9) Remove the original call.
        CI->eraseFromParent();

        errs() << "[lambda-opt] Rewrote call site of '" << OriginalOp->getName() << "' to guard on closure field "
               << Key.FieldIndex << " == " << Prof.HotValue << " (type " << (FieldTy->isIntegerTy() ? "int" : "fp")
               << ")\n";

        Changed = true;
    }

    return Changed;
}

// ================= Direct (non-escaping) specializer =================

bool DirectLambdaSpecializer::run(Module &M) {
    bool Changed = false;

    for (auto &[Key, Prof] : Profiles) {
        StructType *CT = findClosureType(M, Key);
        if (!CT)
            continue;

        Function *OriginalOp = findLambdaOperatorFunc(M, CT);
        if (!OriginalOp)
            continue;

        if (!isProbablyUserCode(*OriginalOp))
            continue;

        Function *HotClone = cloneLambdaOperator(OriginalOp, Key, Prof);
        if (!HotClone)
            continue;

        HotClone->removeFnAttr(Attribute::NoInline);
        HotClone->addFnAttr(Attribute::AlwaysInline);

        bool Body = specializeCaptureInClone(HotClone, CT, Key, Prof);
        bool Calls = specializeConstructionAndCall(OriginalOp, HotClone, CT, Key, Prof);

        if (!Body && !Calls) {
            errs() << "[lambda-opt] Direct: hot clone '" << HotClone->getName() << "' unused; erasing.\n";
            HotClone->eraseFromParent();
            continue;
        }

        errs() << "[lambda-opt] Direct: created hot operator clone '" << HotClone->getName() << "' for lambda '"
               << Key.Name << "', field " << Key.FieldIndex << " = " << Prof.HotValue << "\n";

        Changed = true;
    }

    return Changed;
}

} // namespace lambdaopt
