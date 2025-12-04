#include "LambdaEscapingSpecializer.hpp"

#include <set>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

namespace lambdaopt {

// Backwards-scan to find store to the capture field in the same block
// (unused right now but kept for completeness / experimentation).
static StoreInst *findCaptureStoreForCall(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key) {
    if (!CI || !ClosureTy)
        return nullptr;

    BasicBlock *BB = CI->getParent();
    if (!BB)
        return nullptr;

    if (CI->arg_empty())
        return nullptr;

    Value *ThisArg = CI->getArgOperand(0);
    Value *ThisBase = stripPointerCasts(ThisArg);

    for (auto It = BasicBlock::iterator(CI); It != BB->begin();) {
        --It;
        Instruction &I = *It;

        auto *SI = dyn_cast<StoreInst>(&I);
        if (!SI)
            continue;

        auto *GEP = dyn_cast<GetElementPtrInst>(stripPointerCasts(SI->getPointerOperand()));
        if (!GEP)
            continue;

        if (GEP->getSourceElementType() != ClosureTy)
            continue;

        if (GEP->getNumIndices() < 2)
            continue;

        auto IdxIt = GEP->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(IdxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++IdxIt)->get());
        if (!Idx0 || !Idx1)
            continue;

        if (Idx0->getSExtValue() != 0)
            continue;
        if (Idx1->getZExtValue() != Key.FieldIndex)
            continue;

        Value *GEPBase = stripPointerCasts(GEP->getPointerOperand());
        if (GEPBase != ThisBase)
            continue;

        return SI;
    }

    return nullptr;
}

// Depth-first recursive search for a store to Ptr across CFG (unused,
// but kept from original code).
static StoreInst *findStoreRecursive(Value *Ptr, BasicBlock *StartBB, Instruction *Before,
                                     std::set<BasicBlock *> &Visited) {
    if (!StartBB || Visited.count(StartBB))
        return nullptr;
    Visited.insert(StartBB);

    bool CheckBefore = (Before && Before->getParent() == StartBB);

    for (auto it = StartBB->rbegin(); it != StartBB->rend(); ++it) {
        Instruction &I = *it;
        if (CheckBefore && &I == Before)
            break;

        if (auto *SI = dyn_cast<StoreInst>(&I)) {
            if (stripPointerCasts(SI->getPointerOperand()) == stripPointerCasts(Ptr)) {
                return SI;
            }
        }
    }

    for (BasicBlock *Pred : predecessors(StartBB)) {
        if (StoreInst *S = findStoreRecursive(Ptr, Pred, nullptr, Visited))
            return S;
    }

    return nullptr;
}

// Find the *last* store to the given closure field in the block, before CI.
static StoreInst *findLastStoreToClosureFieldBefore(CallBase *CB, StructType *ClosureTy, const LambdaCaptureKey &Key) {
    if (!CB || !ClosureTy)
        return nullptr;

    BasicBlock *BB = CB->getParent();
    if (!BB)
        return nullptr;

    StoreInst *LastStore = nullptr;

    for (Instruction &I : *BB) {
        if (&I == CB)
            break;

        auto *SI = dyn_cast<StoreInst>(&I);
        if (!SI)
            continue;

        Value *Ptr = SI->getPointerOperand();
        auto *GEP = dyn_cast<GetElementPtrInst>(stripPointerCasts(Ptr));
        if (!GEP)
            continue;

        if (GEP->getSourceElementType() != ClosureTy)
            continue;

        if (GEP->getNumIndices() < 2)
            continue;

        auto idxIt = GEP->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
        if ((Idx0 == nullptr) || (Idx1 == nullptr))
            continue;

        if (Idx0->getSExtValue() != 0)
            continue;
        if (Idx1->getZExtValue() != Key.FieldIndex)
            continue;

        LastStore = SI;
    }

    return LastStore;
}

// Clone an arbitrary function (e.g., the std::function helper / ctor / invoker)
// and specialize loads from the closure field to the hot constant.
//
// We name it based on the original function name, closure type, field index,
// and hot value so we can reuse the clone if we see multiple call sites.
static Function *cloneAndSpecializeForCapture(Function *OrigF, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                              const CaptureProfile &Prof) {
    if (!OrigF || OrigF->isDeclaration())
        return nullptr;

    Module *M = OrigF->getParent();
    if (!M)
        return nullptr;

    // Deterministic name so we can reuse a clone if it already exists.
    std::string NewName = OrigF->getName().str();
    NewName += ".lambda_hot.";
    NewName += ClosureTy->getName().str();
    NewName += ".field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (Function *Existing = M->getFunction(NewName)) {
        return Existing;
    }

    FunctionType *FTy = OrigF->getFunctionType();
    Function *NewF = Function::Create(FTy, OrigF->getLinkage(), NewName, M);

    // Preserve calling convention and attributes.
    NewF->setCallingConv(OrigF->getCallingConv());
    NewF->copyAttributesFrom(OrigF);
    NewF->removeFnAttr(Attribute::NoInline);
    NewF->addFnAttr(Attribute::AlwaysInline);

    ValueToValueMapTy VMap;
    {
        auto A = OrigF->arg_begin();
        auto B = NewF->arg_begin();
        for (; A != OrigF->arg_end(); ++A, ++B) {
            B->setName(A->getName());
            VMap[&*A] = &*B;
        }
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NewF, OrigF, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

    // Now specialize loads from closure.field[Key.FieldIndex] to Prof.HotValue.
    bool Specialized = specializeCaptureInClone(NewF, ClosureTy, Key, Prof);
    if (!Specialized) {

        errs() << "[lambda-opt] cloneAndSpecializeForCapture: no "
                  "matching loads in clone '"
               << NewF->getName() << "'; erasing.\n";
        NewF->eraseFromParent();
        return nullptr;
    }

    errs() << "[lambda-opt] Escaping: created hot specialization '" << NewF->getName() << "' from '" << OrigF->getName()
           << "' for lambda '" << Key.Name << "', field " << Key.FieldIndex << " = " << Prof.HotValue << "\n";

    return NewF;
}

// Rewrite a std::function ctor call so we *branch once* on the capture value:
//
//   if (x == HotValue)
//       std::function(..., HotValue);
//   else
//       std::function(..., x);
//
// After this, the returned std::function is either hot or cold, but not both.
// Rewrite a std::function ctor call so we *branch once* on the capture value:
//
//   if (x == HotValue)
//       std::function(..., HotValue);
//   else
//       std::function(..., x);
//
// Supports two patterns:
//  1) Scalar integer capture passed as zext(load(field))
//  2) Closure passed by-value as an aggregate ([N x i64] load from the closure)
static bool rewriteStdFunctionConstructionWithBranch(CallBase *CB, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                                     const CaptureProfile &Prof) {
    if (!CB || !ClosureTy)
        return false;

    // 0) We need the original callee and a hot specialization of it.
    Function *OrigCallee = CB->getCalledFunction();
    if (!OrigCallee)
        return false;
    if (OrigCallee->getName().contains(".lambda_hot."))
        return false;
    if (BasicBlock *BB = CB->getParent()) {
        StringRef BBName = BB->getName();
        if (BBName.contains("lambda.sf.hot") || BBName.contains("lambda.sf.cold"))
            return false; // don't rewrite the calls we just synthesized
    }

    Function *HotCallee = cloneAndSpecializeForCapture(OrigCallee, ClosureTy, Key, Prof);
    if (!HotCallee) {
        // Fallback: even without a specialized callee, we can still branch and
        // rewrite the capture value so the constructed std::function gets the
        // hot value baked in.
        HotCallee = OrigCallee;
    }

    // ---- Pattern A: scalar capture as zext(load(field)) ----
    int ScalarCaptureArgIndex = -1;
    ZExtInst *ZExt = nullptr;
    LoadInst *ScalarLoad = nullptr;
    GetElementPtrInst *ScalarLdGEP = nullptr;

    // ---- Pattern B: aggregate closure passed by-value ----
    int AggArgIndex = -1;
    LoadInst *AggLoad = nullptr; // e.g. [2 x i64] load
    Value *AggPtr = nullptr;

    for (unsigned i = 0; i < CB->arg_size(); ++i) {
        Value *Arg = CB->getArgOperand(i);

        // Pattern A: zext(load(gep(closure.field[fieldIndex])))
        if (auto *Z = dyn_cast<ZExtInst>(Arg)) {
            auto *L = dyn_cast<LoadInst>(Z->getOperand(0));
            if (!L)
                continue;

            Value *Ptr = L->getPointerOperand();
            auto *G = dyn_cast<GetElementPtrInst>(stripPointerCasts(Ptr));
            if (!G)
                continue;

            if (G->getSourceElementType() != ClosureTy)
                continue;
            if (G->getNumIndices() < 2)
                continue;

            auto idxIt = G->idx_begin();
            auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
            auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
            if (!Idx0 || !Idx1)
                continue;

            if (Idx0->getSExtValue() != 0)
                continue;
            if (Idx1->getZExtValue() != Key.FieldIndex)
                continue;

            ScalarCaptureArgIndex = static_cast<int>(i);
            ZExt = Z;
            ScalarLoad = L;
            ScalarLdGEP = G;
            (void)ScalarLdGEP;
            continue;
        }

        // Pattern B: whole closure loaded as [N x i64]
        if (auto *L = dyn_cast<LoadInst>(Arg)) {
            if (L->getType()->isArrayTy()) {
                // Quick sanity: only used by this call
                if (!L->hasOneUse() || L->user_back() != CB)
                    continue;

                // Optional: we could require the pointer to originate from
                // a GEP of ClosureTy, but in practice this pattern is
                // generated from %class.anon allocas.
                AggArgIndex = static_cast<int>(i);
                AggLoad = L;
                AggPtr = L->getPointerOperand();
                continue;
            }
        }
    }

    // If we matched neither pattern, this ctor is not in a form we handle.
    if (ScalarCaptureArgIndex < 0 && AggArgIndex < 0)
        return false;

    // 2) Find the store to that closure field earlier in the block: this gives us "x".
    StoreInst *Store = findLastStoreToClosureFieldBefore(CB, ClosureTy, Key);
    if (!Store) {
        llvm::errs() << "[lambda-opt] Escaping: could not find store to "
                        "closure field before std::function ctor; skipping.\n";
        return false;
    }

    Value *CaptureVal = Store->getValueOperand(); // this is "x"
    auto *CaptureTy = dyn_cast<IntegerType>(CaptureVal->getType());
    if (!CaptureTy) {
        llvm::errs() << "[lambda-opt] Escaping: non-integer capture value; "
                        "skipping.\n";
        return false;
    }

    ConstantInt *HotCapture = ConstantInt::get(CaptureTy, Prof.HotValue, /*isSigned=*/true);

    BasicBlock *OrigBB = CB->getParent();
    Function *ParentF = OrigBB->getParent();
    LLVMContext &Ctx = ParentF->getContext();

    // We need to handle both call and invoke sites.
    bool IsInvoke = isa<InvokeInst>(CB);

    BasicBlock *MergeBB = nullptr;
    BasicBlock *UnwindDest = nullptr;

    if (IsInvoke) {
        auto *II = cast<InvokeInst>(CB);
        UnwindDest = II->getUnwindDest();
        MergeBB = BasicBlock::Create(Ctx, "lambda.sf.merge", ParentF, II->getNormalDest());
    }
    else {
        MergeBB = OrigBB->splitBasicBlock(CB->getIterator(), "lambda.sf.merge");
    }

    // 3) Split block at the ctor call
    Instruction *OldTerm = OrigBB->getTerminator();
    IRBuilder<> CmpBuilder(OldTerm);

    // Branch on "x" (the value stored to the closure)
    Value *IsHot = CmpBuilder.CreateICmpEQ(CaptureVal, HotCapture, "lambda.sf.is_hot");

    // Create hot and cold blocks
    auto *HotBB = BasicBlock::Create(Ctx, "lambda.sf.hot", ParentF, MergeBB);
    auto *ColdBB = BasicBlock::Create(Ctx, "lambda.sf.cold", ParentF, MergeBB);

    // 4) Build arg lists for ctor (baseline: original args)
    SmallVector<Value *, 8> ColdArgs;
    ColdArgs.reserve(CB->arg_size());
    for (unsigned i = 0; i < CB->arg_size(); ++i)
        ColdArgs.push_back(CB->getArgOperand(i));

    SmallVector<Value *, 8> HotArgs = ColdArgs;

    // Pattern A (scalar): override ctor arg with hot constant (zext’d)
    if (ScalarCaptureArgIndex >= 0) {
        auto *CtorArgTy = dyn_cast<IntegerType>(ZExt->getType());
        if (!CtorArgTy) {
            llvm::errs() << "[lambda-opt] Escaping: unexpected non-int ctor "
                            "arg type in scalar pattern; skipping.\n";
            return false;
        }

        ConstantInt *HotCtorArg = ConstantInt::get(CtorArgTy, Prof.HotValue, /*isSigned=*/true);
        HotArgs[ScalarCaptureArgIndex] = HotCtorArg;
    }

    // NOTE: for Pattern B (aggregate), we will *not* directly reuse AggLoad.
    // Instead, we re-materialize loads in each branch after we possibly store
    // the hot capture into the closure. We will patch the args below.

    // 5) Cold path: original semantics (but we may rebuild aggregate arg)
    // Replace the original terminator with our guard once we've consumed CB.
    bool DetachedInvoke = false;
    if (IsInvoke) {
        CB->removeFromParent(); // detach so we can insert a branch
        DetachedInvoke = true;
    }
    else {
        OldTerm->eraseFromParent();
    }
    IRBuilder<> BrBuilder(OrigBB);
    BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

    Value *ColdCall = nullptr;
    {
        IRBuilder<> B(ColdBB);

        // If we had an aggregate arg, rebuild the load *here*,
        // so we don't depend on the original AggLoad in OrigBB.
        if (AggArgIndex >= 0 && AggLoad && AggPtr) {
            Value *NewAggLoad = B.CreateLoad(AggLoad->getType(), AggPtr, AggLoad->getName() + ".sf.cold");
            ColdArgs[AggArgIndex] = NewAggLoad;
        }

        if (IsInvoke) {
            auto *II = cast<InvokeInst>(CB);
            ColdCall = B.CreateInvoke(OrigCallee, MergeBB, UnwindDest, ColdArgs, CB->getName() + ".cold");
        }
        else {
            ColdCall = B.CreateCall(OrigCallee, ColdArgs, CB->getName() + ".cold");
            B.CreateBr(MergeBB);
        }
    }

    // 6) Hot path:
    //    - overwrite closure field with HotValue (so any remaining loads see it)
    //    - call the *specialized* callee
    Value *HotCall = nullptr;
    {
        IRBuilder<> B(HotBB);

        // Overwrite the capture field with the hot value:
        Value *CapturePtr = Store->getPointerOperand();
        B.CreateStore(HotCapture, CapturePtr);

        // If aggregate arg, re-load it *after* the store, so it sees the hot
        // capture bit pattern in the closure.
        if (AggArgIndex >= 0 && AggLoad && AggPtr) {
            Value *NewAggLoad = B.CreateLoad(AggLoad->getType(), AggPtr, AggLoad->getName() + ".sf.hot");
            HotArgs[AggArgIndex] = NewAggLoad;
        }

        if (IsInvoke) {
            auto *II = cast<InvokeInst>(CB);
            HotCall = B.CreateInvoke(HotCallee, MergeBB, UnwindDest, HotArgs, CB->getName() + ".hot");
        }
        else {
            HotCall = B.CreateCall(HotCallee, HotArgs, CB->getName() + ".hot");
            B.CreateBr(MergeBB);
        }
    }

    // 7) Merge ctor result if it is used (often it is not)
    if (!CB->getType()->isVoidTy()) {
        IRBuilder<> MB(MergeBB);
        auto InsertIt = MergeBB->getFirstNonPHIOrDbgOrAlloca();
        if (InsertIt != MergeBB->end())
            MB.SetInsertPoint(&*InsertIt);

        PHINode *PHI = MB.CreatePHI(CB->getType(), 2, CB->getName() + ".sf.sel");
        PHI->addIncoming(HotCall, HotBB);
        PHI->addIncoming(ColdCall, ColdBB);
        if (!CB->use_empty())
            CB->replaceAllUsesWith(PHI);
    }

    // For invoke, redirect the merge block to the original normal dest and
    // update incoming edges.
    if (IsInvoke) {
        auto *II = cast<InvokeInst>(CB);
        BasicBlock *NormalDest = II->getNormalDest();
        BranchInst::Create(NormalDest, MergeBB);

        // Any PHI nodes that expected OrigBB now expect MergeBB.
        for (PHINode &PN : NormalDest->phis()) {
            PN.replaceIncomingBlockWith(OrigBB, MergeBB);
        }
    }

    // 8) Clean up: original call + (optionally) original AggLoad
    if (AggLoad && AggLoad->use_empty())
        AggLoad->eraseFromParent();

    if (DetachedInvoke) {
        CB->dropAllReferences();
        delete CB;
    }
    else {
        CB->eraseFromParent();
    }

    llvm::errs() << "[lambda-opt] Escaping: branched std::function "
                    "construction for closure '"
                 << ClosureTy->getName() << "', field " << Key.FieldIndex << " on value == " << Prof.HotValue
                 << " using specialized callee '" << HotCallee->getName() << "'\n";

    return true;
}

bool EscapingLambdaSpecializer::run(Module &M) {
    bool Changed = false;

    for (auto &[Key, Prof] : Profiles) {
        // Skip noisy stdlib helper structs that show up in the profile.
        if (Key.Name.find("class.anon") == std::string::npos)
            continue;

        StructType *CT = findClosureType(M, Key);
        if (CT == nullptr) {
            continue;
        }

        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                SmallVector<CallBase *, 8> Calls;
                for (Instruction &I : BB)
                    if (auto *CB = dyn_cast<CallBase>(&I))
                        Calls.push_back(CB);

                for (CallBase *CB : Calls) {
                    if (rewriteStdFunctionConstructionWithBranch(CB, CT, Key, Prof)) {
                        Changed = true;
                    }
                }
            }
        }
    }

    return Changed;
}

} // namespace lambdaopt
