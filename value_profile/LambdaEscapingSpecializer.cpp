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
static StoreInst *findLastStoreToClosureFieldBefore(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key) {
    if (!CI || !ClosureTy)
        return nullptr;

    BasicBlock *BB = CI->getParent();
    if (!BB)
        return nullptr;

    StoreInst *LastStore = nullptr;

    for (Instruction &I : *BB) {
        if (&I == CI)
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
static bool rewriteStdFunctionConstructionWithBranch(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                                     const CaptureProfile &Prof) {
    if ((CI == nullptr) || (ClosureTy == nullptr)) {
        return false;
    }

    // 0) We need the original callee and a hot specialization of it.
    Function *OrigCallee = CI->getCalledFunction();
    if (!OrigCallee) {
        return false;
    }

    Function *HotCallee = cloneAndSpecializeForCapture(OrigCallee, ClosureTy, Key, Prof);
    if (!HotCallee) {
        // If we can't build a hot specialization, just bail out and leave this
        // call untouched.
        return false;
    }

    // 1) Find ctor argument that is zext(load(closure.field[fieldIndex]))
    int CaptureArgIndex = -1;
    ZExtInst *ZExt = nullptr;
    LoadInst *Load = nullptr;
    GetElementPtrInst *LdGEP = nullptr;

    for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value *Arg = CI->getArgOperand(i);

        auto *Z = dyn_cast<ZExtInst>(Arg);
        if (Z == nullptr) {
            continue;
        }

        auto *L = dyn_cast<LoadInst>(Z->getOperand(0));
        if (L == nullptr) {
            continue;
        }

        Value *Ptr = L->getPointerOperand();
        auto *G = dyn_cast<GetElementPtrInst>(stripPointerCasts(Ptr));
        if (G == nullptr) {
            continue;
        }

        if (G->getSourceElementType() != ClosureTy) {
            continue;
        }
        if (G->getNumIndices() < 2) {
            continue;
        }

        auto idxIt = G->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
        if ((Idx0 == nullptr) || (Idx1 == nullptr)) {
            continue;
        }

        if (Idx0->getSExtValue() != 0) {
            continue;
        }
        if (Idx1->getZExtValue() != Key.FieldIndex) {
            continue;
        }

        CaptureArgIndex = static_cast<int>(i);
        ZExt = Z;
        Load = L;
        LdGEP = G;
        (void)LdGEP; // kept only for debugging / completeness
        break;
    }

    if (CaptureArgIndex < 0) {
        return false;
    }

    // 2) Find the store to that closure field earlier in the block: this gives us "x".
    StoreInst *Store = findLastStoreToClosureFieldBefore(CI, ClosureTy, Key);
    if (Store == nullptr) {
        errs() << "[lambda-opt] Escaping: could not find store to "
                  "closure field before std::function ctor; skipping.\n";
        return false;
    }

    Value *CaptureVal = Store->getValueOperand(); // this is "x"
    auto *CaptureTy = dyn_cast<IntegerType>(CaptureVal->getType());
    auto *CtorArgTy = dyn_cast<IntegerType>(ZExt->getType());

    if (!CaptureTy || !CtorArgTy) {
        errs() << "[lambda-opt] Escaping: non-integer capture or ctor arg; "
                  "skipping.\n";
        return false;
    }

    ConstantInt *HotCapture = ConstantInt::get(CaptureTy, Prof.HotValue, /*isSigned=*/true);
    ConstantInt *HotCtorArg = ConstantInt::get(CtorArgTy, Prof.HotValue, /*isSigned=*/true);

    BasicBlock *OrigBB = CI->getParent();
    Function *ParentF = OrigBB->getParent();
    LLVMContext &Ctx = ParentF->getContext();

    // 3) Split block at the ctor call
    BasicBlock *MergeBB = OrigBB->splitBasicBlock(CI, "lambda.sf.merge");

    Instruction *OldTerm = OrigBB->getTerminator();
    IRBuilder<> CmpBuilder(OldTerm);

    // Branch on "x" (the value stored to the closure)
    Value *IsHot = CmpBuilder.CreateICmpEQ(CaptureVal, HotCapture, "lambda.sf.is_hot");

    // Create hot and cold blocks
    auto *HotBB = BasicBlock::Create(Ctx, "lambda.sf.hot", ParentF, MergeBB);
    auto *ColdBB = BasicBlock::Create(Ctx, "lambda.sf.cold", ParentF, MergeBB);

    OldTerm->eraseFromParent();
    IRBuilder<> BrBuilder(OrigBB);
    BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

    // 4) Build arg lists for ctor
    SmallVector<Value *, 8> ColdArgs;
    ColdArgs.reserve(CI->arg_size());
    for (unsigned i = 0; i < CI->arg_size(); ++i)
        ColdArgs.push_back(CI->getArgOperand(i));

    SmallVector<Value *, 8> HotArgs = ColdArgs;
    HotArgs[CaptureArgIndex] = HotCtorArg;

    // 5) Cold path: original semantics
    Value *ColdCall = nullptr;
    {
        IRBuilder<> B(ColdBB);
        ColdCall = B.CreateCall(OrigCallee, ColdArgs, CI->getName() + ".cold");
        B.CreateBr(MergeBB);
    }

    // 6) Hot path:
    //    - overwrite closure field with HotValue (so any remaining loads see 10)
    //    - call the *specialized* callee, which also has the hot value baked in
    Value *HotCall = nullptr;
    {
        IRBuilder<> B(HotBB);
        // Reuse the same field pointer as the store
        Value *CapturePtr = Store->getPointerOperand();
        B.CreateStore(HotCapture, CapturePtr);
        HotCall = B.CreateCall(HotCallee, HotArgs, CI->getName() + ".hot");
        B.CreateBr(MergeBB);
    }

    // 7) Merge ctor result if it is used (often it is not)
    if (!CI->use_empty()) {
        IRBuilder<> MB(MergeBB);
        MB.SetInsertPoint(&*MergeBB->begin());
        PHINode *PHI = MB.CreatePHI(CI->getType(), 2, CI->getName() + ".sf.sel");
        PHI->addIncoming(HotCall, HotBB);
        PHI->addIncoming(ColdCall, ColdBB);
        CI->replaceAllUsesWith(PHI);
    }

    CI->eraseFromParent();

    errs() << "[lambda-opt] Escaping: branched std::function construction for "
              "closure '"
           << ClosureTy->getName() << "', field " << Key.FieldIndex << " on value == " << Prof.HotValue
           << " using specialized callee '" << HotCallee->getName() << "'\n";

    return true;
}

bool EscapingLambdaSpecializer::run(Module &M) {
    bool Changed = false;

    for (auto &[Key, Prof] : Profiles) {
        StructType *CT = findClosureType(M, Key);
        if (CT == nullptr) {
            continue;
        }

        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                SmallVector<CallInst *, 8> Calls;
                for (Instruction &I : BB)
                    if (auto *CI = dyn_cast<CallInst>(&I))
                        Calls.push_back(CI);

                for (CallInst *CI : Calls) {
                    if (rewriteStdFunctionConstructionWithBranch(CI, CT, Key, Prof)) {
                        Changed = true;
                    }
                }
            }
        }
    }

    return Changed;
}

} // namespace lambdaopt
