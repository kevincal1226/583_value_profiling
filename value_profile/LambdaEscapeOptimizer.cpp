// Pass that *detects* escaping lambdas and reports the operator() we think
// belongs to each profiled closure, then rewrites std::function construction
// to install a hot specialization when the capture matches the profiled value.

#include <string>

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "LambdaCommon.hpp"
#include "LambdaEscapeOptimizer.hpp"

using namespace llvm;

namespace lambdaopt {

// ----------------- helpers -----------------

static std::string demangleName(StringRef Name) { return llvm::demangle(Name.str()); }

static Value *stripPointerCasts(Value *V) {
    while (true) {
        if (auto *BC = dyn_cast<BitCastInst>(V))
            V = BC->getOperand(0);
        else if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V))
            V = ASC->getOperand(0);
        else
            break;
    }
    return V;
}

static StructType *findClosureType(const Module &M, const LambdaCaptureKey &Key) {
    for (StructType *ST : M.getIdentifiedStructTypes()) {
        if (ST->isOpaque())
            continue;
        if (ST->hasName() && ST->getName() == Key.Name)
            return ST;
    }
    return nullptr;
}

// Scoring-based selection of the lambda operator for a closure type.
static Function *findLambdaOperatorFunc(Module &M, StructType *ClosureTy) {
    if (!ClosureTy)
        return nullptr;

    PointerType *ThisPtrTy = ClosureTy->getPointerTo();
    Function *Best = nullptr;
    int BestScore = -1;

    for (Function &F : M) {
        if (F.isDeclaration() || F.arg_empty())
            continue;

        Argument &FirstArg = *F.arg_begin();
        if (FirstArg.getType() != ThisPtrTy)
            continue;

        std::string Demangled = demangleName(F.getName());

        // Skip obvious library glue.
        if (Demangled.find("std::") != std::string::npos || Demangled.find("llvm::") != std::string::npos ||
            Demangled.find("__gnu_cxx::") != std::string::npos)
            continue;

        int Score = 0;
        bool HasOpCall = (Demangled.find("operator()") != std::string::npos);
        bool MentionsClosure = (Demangled.find(ClosureTy->getName().str()) != std::string::npos);
        bool LooksLambda = (Demangled.find("lambda") != std::string::npos) ||
                           (Demangled.find("$_") != std::string::npos);

        // If it doesn't look like an operator() or lambda-ish symbol at all, skip.
        if (!HasOpCall && !LooksLambda)
            continue;

        if (HasOpCall)
            Score += 5;
        if (MentionsClosure)
            Score += 3;
        if (LooksLambda)
            Score += 2;

        // Prefer functions with direct call sites.
        int DirectCalls = 0;
        for (User *U : F.users()) {
            if (auto *CI = dyn_cast<CallInst>(U)) {
                if (CI->getCalledFunction() == &F)
                    ++DirectCalls;
            }
        }
        Score += std::min(DirectCalls, 3); // cap to avoid dominating score

        errs() << "    [candidate] " << F.getName() << " score=" << Score << " direct_calls=" << DirectCalls
               << " demangled='" << Demangled << "'\n";

        if (Score > BestScore) {
            BestScore = Score;
            Best = &F;
        }
    }

    return Best;
}

// Replace loads from closure field with constant in clone.
static bool specializeCaptureInClone(Function *CloneF, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                     const CaptureProfile &Prof) {
    if (!CloneF || !ClosureTy || CloneF->arg_empty())
        return false;

    bool Changed = false;
    SmallVector<Instruction *, 8> ToErase;

    for (BasicBlock &BB : *CloneF) {
        for (Instruction &I : BB) {
            auto *LI = dyn_cast<LoadInst>(&I);
            if (!LI)
                continue;

            auto *GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand());
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

            Type *FieldTy = LI->getType();
            Constant *ConstVal = nullptr;

            if (auto *IntTy = dyn_cast<IntegerType>(FieldTy)) {
                ConstVal = ConstantInt::get(IntTy, Prof.HotValue, /*isSigned*/ true);
            }
            else if (FieldTy->isFloatingPointTy()) {
                ConstVal = ConstantFP::get(FieldTy, static_cast<double>(Prof.HotValue));
            }
            else {
                continue;
            }

            LI->replaceAllUsesWith(ConstVal);
            ToErase.push_back(LI);
            Changed = true;
        }
    }

    for (Instruction *I : ToErase)
        I->eraseFromParent();

    return Changed;
}

// Clone and specialize an arbitrary function for the hot capture.
// If RequireSpecialization is false, we keep the clone even when no loads
// were specialized (useful for std::function thunks / ctors where we still
// want to retarget nested calls or vtables).
static Function *cloneAndSpecializeForCapture(Function *OrigF, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                              const CaptureProfile &Prof, bool RequireSpecialization = true) {
    if (!OrigF || OrigF->isDeclaration())
        return nullptr;

    Module *M = OrigF->getParent();
    if (!M)
        return nullptr;

    std::string NewName = OrigF->getName().str();
    NewName += ".lambda_hot.";
    NewName += ClosureTy->getName().str();
    NewName += ".field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (Function *Existing = M->getFunction(NewName))
        return Existing;

    FunctionType *FTy = OrigF->getFunctionType();
    Function *NewF = Function::Create(FTy, OrigF->getLinkage(), NewName, M);
    NewF->setCallingConv(OrigF->getCallingConv());
    NewF->copyAttributesFrom(OrigF);
    NewF->removeFnAttr(Attribute::NoInline);
    NewF->addFnAttr(Attribute::AlwaysInline);

    ValueToValueMapTy VMap;
    auto A = OrigF->arg_begin();
    auto B = NewF->arg_begin();
    for (; A != OrigF->arg_end(); ++A, ++B) {
        B->setName(A->getName());
        VMap[&*A] = &*B;
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NewF, OrigF, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

    bool Specialized = specializeCaptureInClone(NewF, ClosureTy, Key, Prof);
    if (!Specialized && RequireSpecialization) {
        NewF->eraseFromParent();
        return nullptr;
    }

    return NewF;
}

// Find a vtable global for this lambda's std::function instantiation.
static ConstantArray *extractVTableArray(Constant *Init) {
    if (auto *Arr = dyn_cast<ConstantArray>(Init))
        return Arr;
    if (auto *CS = dyn_cast<ConstantStruct>(Init)) {
        if (CS->getNumOperands() == 1)
            if (auto *Arr = dyn_cast<ConstantArray>(CS->getOperand(0)))
                return Arr;
    }
    return nullptr;
}

static GlobalVariable *findFunctionVTableForClosure(Module &M, const LambdaCaptureKey &Key) {
    GlobalVariable *FirstMatch = nullptr;
    for (GlobalVariable &GV : M.globals()) {
        if (!GV.hasInitializer())
            continue;
        if (!GV.getName().contains("__function6__func"))
            continue;
        if (!extractVTableArray(GV.getInitializer()))
            continue;
        if (GV.getName().contains(Key.Name))
            return &GV; // strong match on closure name
        if (!FirstMatch)
            FirstMatch = &GV; // fallback: remember first function vtable we see
    }
    return FirstMatch;
}

// Extract the call-slot function (index 8 in libc++ vtable layout used here).
static Function *getCallThunkFromVTable(GlobalVariable *VTableGV) {
    if (!VTableGV || !VTableGV->hasInitializer())
        return nullptr;

    ConstantArray *Arr = extractVTableArray(VTableGV->getInitializer());
    if (!Arr)
        return nullptr;

    const unsigned CallIndex = 8; // null, RTTI, d1, d0, clone, clone(base), destroy, destroy_dealloc, call, target, target_type
    if (Arr->getNumOperands() <= CallIndex)
        return nullptr;

    if (auto *CE = dyn_cast<ConstantExpr>(Arr->getOperand(CallIndex))) {
        if (CE->isCast() && isa<Function>(CE->getOperand(0)))
            return cast<Function>(CE->getOperand(0));
    }
    else if (auto *F = dyn_cast<Function>(Arr->getOperand(CallIndex))) {
        return F;
    }
    return nullptr;
}

// Clone vtable with hot call thunk in the call slot.
static GlobalVariable *cloneVTableWithHotCall(Module &M, GlobalVariable *OrigVT, Function *HotCallThunk,
                                              const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
    if (!OrigVT || !HotCallThunk || !OrigVT->hasInitializer())
        return nullptr;

    Constant *OrigInit = OrigVT->getInitializer();
    ConstantArray *Arr = extractVTableArray(OrigInit);
    if (!Arr)
        return nullptr;

    SmallVector<Constant *, 16> Elts;
    Elts.reserve(Arr->getNumOperands());

    const unsigned CallIndex = 8;
    for (unsigned i = 0; i < Arr->getNumOperands(); ++i) {
        Constant *Op = Arr->getOperand(i);
        if (i == CallIndex) {
            Op = ConstantExpr::getBitCast(HotCallThunk, Op->getType());
        }
        Elts.push_back(Op);
    }

    Constant *NewArrayInit = ConstantArray::get(Arr->getType(), Elts);
    Constant *NewInit = NewArrayInit;
    if (auto *CS = dyn_cast<ConstantStruct>(OrigInit)) {
        SmallVector<Constant *, 1> Fields;
        Fields.push_back(NewArrayInit);
        NewInit = ConstantStruct::get(cast<StructType>(OrigVT->getValueType()), Fields);
    }

    std::string NewName = OrigVT->getName().str();
    NewName += ".lambda_hot.";
    NewName += Key.Name;
    NewName += ".field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    auto *NewGV = new GlobalVariable(M, OrigVT->getValueType(), OrigVT->isConstant(), OrigVT->getLinkage(), NewInit,
                                     NewName, /*InsertBefore*/ nullptr, OrigVT->getThreadLocalMode(),
                                     OrigVT->getAddressSpace());
    NewGV->setUnnamedAddr(OrigVT->getUnnamedAddr());
    NewGV->setAlignment(OrigVT->getAlign());
    return NewGV;
}

// Replace any reference to OrigVT inside F with HotVT (bitcasted as needed).
static bool rewriteVTableUses(Function *F, GlobalVariable *OrigVT, GlobalVariable *HotVT) {
    if (!F || !OrigVT || !HotVT)
        return false;

    bool Changed = false;
    for (BasicBlock &BB : *F) {
        for (Instruction &I : BB) {
            for (unsigned idx = 0; idx < I.getNumOperands(); ++idx) {
                Value *Op = I.getOperand(idx);
                if (Op != OrigVT)
                    continue;
                Value *NewOp = HotVT;
                if (Op->getType() != HotVT->getType())
                    NewOp = ConstantExpr::getBitCast(HotVT, Op->getType());
                I.setOperand(idx, NewOp);
                Changed = true;
            }
        }
    }
    return Changed;
}

// Fallback: replace any std::function vtable store inside F with HotVT,
// even if we failed to match the exact original vtable symbol.
static bool forceHotVTableStores(Function *F, GlobalVariable *HotVT) {
    if (!F || !HotVT)
        return false;
    bool Changed = false;

    for (BasicBlock &BB : *F) {
        for (Instruction &I : BB) {
            auto *SI = dyn_cast<StoreInst>(&I);
            if (!SI)
                continue;
            auto *CE = dyn_cast<ConstantExpr>(SI->getValueOperand());
            if (!CE)
                continue;
            if (CE->getOpcode() != Instruction::GetElementPtr)
                continue;
            auto *GV = dyn_cast<GlobalVariable>(CE->getOperand(0));
            if (!GV || !GV->getName().contains("__function6__func"))
                continue;

            SmallVector<Value *, 4> Idxs(CE->op_begin() + 1, CE->op_end()); // skip operand 0 (the GV)
            Value *NewGEP = ConstantExpr::getGetElementPtr(HotVT->getValueType(), HotVT, Idxs, /*InBounds=*/false);
            SI->setOperand(0, NewGEP);
            Changed = true;
        }
    }
    return Changed;
}

// Rewrite any operand that is a GEP/bitcast of OrigVT inside the whole module.
static bool rewriteVTableInModule(Module &M, GlobalVariable *OrigVT, GlobalVariable *HotVT) {
    if (!OrigVT || !HotVT)
        return false;
    bool Changed = false;

    auto rewriteOperand = [&](Instruction &I, unsigned idx, ConstantExpr *CE) {
        SmallVector<Value *, 8> Ops(CE->op_begin(), CE->op_end());
        if (Ops.empty())
            return false;
        if (Ops[0] != OrigVT)
            return false;
        Ops[0] = HotVT;
        Constant *NewCE = nullptr;
        if (CE->getOpcode() == Instruction::GetElementPtr)
            NewCE = ConstantExpr::getGetElementPtr(HotVT->getValueType(), HotVT, ArrayRef<Value *>(Ops).slice(1),
                                                   /*InBounds=*/false);
        else if (CE->isCast())
            NewCE = ConstantExpr::getCast(CE->getOpcode(), HotVT, CE->getType());
        else
            return false;
        I.setOperand(idx, NewCE);
        return true;
    };

    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                for (unsigned idx = 0; idx < I.getNumOperands(); ++idx) {
                    Value *Op = I.getOperand(idx);
                    if (Op == OrigVT) {
                        Value *NewOp = HotVT;
                        if (Op->getType() != HotVT->getType())
                            NewOp = ConstantExpr::getBitCast(HotVT, Op->getType());
                        I.setOperand(idx, NewOp);
                        Changed = true;
                        continue;
                    }
                    if (auto *CE = dyn_cast<ConstantExpr>(Op)) {
                        if (rewriteOperand(I, idx, CE))
                            Changed = true;
                    }
                }
            }
        }
    }
    return Changed;
}

static Function *getDirectFunction(CallBase *CB) {
    if (!CB)
        return nullptr;
    if (Function *F = CB->getCalledFunction())
        return F;
    if (auto *CE = dyn_cast<ConstantExpr>(CB->getCalledOperand()))
        if (CE->isCast())
            if (auto *F = dyn_cast<Function>(CE->getOperand(0)))
                return F;
    return nullptr;
}

// If a clone still calls the cold lambda operator(), redirect it to the hot clone.
static bool rewriteCallsToOperator(Function *F, Function *ColdOp, Function *HotOp) {
    if (!F || !ColdOp || !HotOp)
        return false;

    bool Changed = false;
    for (BasicBlock &BB : *F) {
        for (Instruction &I : BB) {
            auto *CB = dyn_cast<CallBase>(&I);
            if (!CB)
                continue;
            Function *Callee = getDirectFunction(CB);
            if (Callee != ColdOp)
                continue;

            if (HotOp->getFunctionType() == CB->getFunctionType()) {
                CB->setCalledFunction(HotOp);
            }
            else {
                Value *NewCallee = ConstantExpr::getBitCast(HotOp, CB->getFunctionType()->getPointerTo());
                CB->setCalledFunction(FunctionCallee(CB->getFunctionType(), NewCallee));
            }
            Changed = true;
        }
    }
    return Changed;
}

// In the cloned std::function ctor, swap stored vtable to hot vtable.
static void rewriteCtorVTableStore(Function *CtorClone, GlobalVariable *OrigVT, GlobalVariable *HotVT) {
    if (!CtorClone || !OrigVT || !HotVT)
        return;

    SmallVector<StoreInst *, 4> Stores;
    for (BasicBlock &BB : *CtorClone) {
        for (Instruction &I : BB) {
            if (auto *SI = dyn_cast<StoreInst>(&I)) {
                if (auto *CE = dyn_cast<ConstantExpr>(SI->getValueOperand())) {
                    if (CE->getOpcode() == Instruction::GetElementPtr && CE->getOperand(0) == OrigVT) {
                        Stores.push_back(SI);
                    }
                }
            }
        }
    }

    for (StoreInst *SI : Stores) {
        auto *OldCE = cast<ConstantExpr>(SI->getValueOperand());
        SmallVector<Value *, 4> Idxs(OldCE->op_begin() + 1, OldCE->op_end()); // skip operand 0 (the GV)
        Type *ElemTy = OrigVT->getValueType();
        Value *NewGEP = ConstantExpr::getGetElementPtr(ElemTy, HotVT, Idxs);
        SI->setOperand(0, NewGEP);
    }
}

// Inside a cloned ctor, any delegation calls (e.g., C1 -> C2) or helper calls
// will still point at the *cold* versions. If those helpers touch the lambda
// closure, we need to redirect them to their own hot clones so the hot path
// does not fall back to constructing a cold lambda.
static bool retargetNestedCtorCalls(Function *CtorClone, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                    const CaptureProfile &Prof, GlobalVariable *OrigVT, GlobalVariable *HotVT,
                                    Function *HotOp) {
    if (!CtorClone)
        return false;

    bool Changed = false;
    SmallVector<CallBase *, 8> Calls;

    for (BasicBlock &BB : *CtorClone)
        for (Instruction &I : BB)
            if (auto *CB = dyn_cast<CallBase>(&I))
                if (CB->getCalledFunction() && !CB->getCalledFunction()->getName().contains(".lambda_hot."))
                    Calls.push_back(CB);

    for (CallBase *CB : Calls) {
        Function *Callee = getDirectFunction(CB);
        if (!Callee)
            continue;

        // Try to build a hot clone of this callee. If specialization fails, skip.
        Function *Hot = cloneAndSpecializeForCapture(Callee, ClosureTy, Key, Prof,
                                                     /*RequireSpecialization=*/false);
        if (!Hot)
            continue;

        // Ensure the nested clone installs the hot vtable too (if it has one).
        rewriteCtorVTableStore(Hot, OrigVT, HotVT);
        if (HotOp)
            rewriteCallsToOperator(Hot, Callee, HotOp);

        if (Hot->getFunctionType() == CB->getFunctionType()) {
            CB->setCalledFunction(Hot);
        }
        else {
            Value *NewCallee = ConstantExpr::getBitCast(Hot, CB->getFunctionType()->getPointerTo());
            CB->setCalledFunction(FunctionCallee(CB->getFunctionType(), NewCallee));
        }
        Changed = true;
    }

    return Changed;
}

// Find the last store to closure.field before CI in the same block.
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

        auto *GEP = dyn_cast<GetElementPtrInst>(stripPointerCasts(SI->getPointerOperand()));
        if (!GEP)
            continue;

        if (GEP->getSourceElementType() != ClosureTy)
            continue;
        if (GEP->getNumIndices() < 2)
            continue;

        auto idxIt = GEP->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
        if (!Idx0 || !Idx1)
            continue;
        if (Idx0->getSExtValue() != 0)
            continue;
        if (Idx1->getZExtValue() != Key.FieldIndex)
            continue;

        LastStore = SI;
    }

    return LastStore;
}

// Rewrite std::function construction call to branch on hot capture and install specialized callable.
static bool rewriteStdFunctionConstructionWithBranch(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                                     const CaptureProfile &Prof) {
    if (!CI || !ClosureTy)
        return false;

    Function *OrigCallee = CI->getCalledFunction();
    if (!OrigCallee)
        return false;

    // Skip already-hot clones to avoid infinite cloning.
    if (OrigCallee->getName().contains(".lambda_hot."))
        return false;

    // Build a hot clone of the lambda's operator() so the call thunk can use it.
    Function *ColdOp = findLambdaOperatorFunc(*CI->getModule(), ClosureTy);
    Function *HotOp = cloneAndSpecializeForCapture(ColdOp, ClosureTy, Key, Prof);

    Function *HotCallee =
        cloneAndSpecializeForCapture(OrigCallee, ClosureTy, Key, Prof, /*RequireSpecialization=*/false);
    if (!HotCallee)
        return false;

    // Build hot call thunk and vtable if possible.
    Module *M = OrigCallee->getParent();
    GlobalVariable *OrigVT = findFunctionVTableForClosure(*M, Key);
    Function *OrigCallThunk = getCallThunkFromVTable(OrigVT);
    Function *HotCallThunk =
        cloneAndSpecializeForCapture(OrigCallThunk, ClosureTy, Key, Prof, /*RequireSpecialization=*/false);
    GlobalVariable *HotVT = cloneVTableWithHotCall(*M, OrigVT, HotCallThunk, Key, Prof);
    if (HotVT) {
        rewriteCtorVTableStore(HotCallee, OrigVT, HotVT);
        rewriteVTableUses(HotCallee, OrigVT, HotVT);
        rewriteVTableUses(HotCallThunk, OrigVT, HotVT);
        rewriteVTableInModule(*M, OrigVT, HotVT); // last-resort rewrite for any remaining uses
        forceHotVTableStores(HotCallee, HotVT);
        forceHotVTableStores(HotCallThunk, HotVT);
    }
    // Redirect any cold operator() calls inside the hot ctor or call thunk.
    rewriteCallsToOperator(HotCallee, ColdOp, HotOp);
    rewriteCallsToOperator(HotCallThunk, ColdOp, HotOp);

    // If the ctor delegates (e.g., C1 -> C2), make sure nested ctor calls use hot clones too.
    retargetNestedCtorCalls(HotCallee, ClosureTy, Key, Prof, OrigVT, HotVT, HotOp);

    // Find arg that is zext(load(gep closure.field))
    int CaptureArgIndex = -1;
    ZExtInst *ZExt = nullptr;
    LoadInst *Load = nullptr;
    GetElementPtrInst *LdGEP = nullptr;

    for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value *Arg = CI->getArgOperand(i);
        auto *Z = dyn_cast<ZExtInst>(Arg);
        if (!Z)
            continue;

        auto *L = dyn_cast<LoadInst>(Z->getOperand(0));
        if (!L)
            continue;

        auto *G = dyn_cast<GetElementPtrInst>(stripPointerCasts(L->getPointerOperand()));
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

        CaptureArgIndex = static_cast<int>(i);
        ZExt = Z;
        Load = L;
        LdGEP = G;
        break;
    }

    if (CaptureArgIndex < 0)
        return false;

    // Find the store to that field earlier in the block to get the SSA value.
    StoreInst *Store = findLastStoreToClosureFieldBefore(CI, ClosureTy, Key);
    if (!Store)
        return false;

    Value *CaptureVal = Store->getValueOperand();
    auto *CaptureTy = dyn_cast<IntegerType>(CaptureVal->getType());
    auto *CtorArgTy = dyn_cast<IntegerType>(ZExt->getType());
    if (!CaptureTy || !CtorArgTy)
        return false;

    ConstantInt *HotCapture = ConstantInt::get(CaptureTy, Prof.HotValue, /*isSigned=*/true);
    ConstantInt *HotCtorArg = ConstantInt::get(CtorArgTy, Prof.HotValue, /*isSigned=*/true);

    BasicBlock *OrigBB = CI->getParent();
    Function *ParentF = OrigBB->getParent();
    LLVMContext &Ctx = ParentF->getContext();

    BasicBlock *MergeBB = OrigBB->splitBasicBlock(CI, "lambda.sf.merge");
    Instruction *OldTerm = OrigBB->getTerminator();

    IRBuilder<> CmpBuilder(OldTerm);
    Value *IsHot = CmpBuilder.CreateICmpEQ(CaptureVal, HotCapture, "lambda.sf.is_hot");

    auto *HotBB = BasicBlock::Create(Ctx, "lambda.sf.hot", ParentF, MergeBB);
    auto *ColdBB = BasicBlock::Create(Ctx, "lambda.sf.cold", ParentF, MergeBB);

    OldTerm->eraseFromParent();
    IRBuilder<> BrBuilder(OrigBB);
    BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

    SmallVector<Value *, 8> ColdArgs;
    ColdArgs.reserve(CI->arg_size());
    for (unsigned i = 0; i < CI->arg_size(); ++i)
        ColdArgs.push_back(CI->getArgOperand(i));

    SmallVector<Value *, 8> HotArgs = ColdArgs;
    HotArgs[CaptureArgIndex] = HotCtorArg;

    Value *ColdCall = nullptr;
    {
        IRBuilder<> B(ColdBB);
        ColdCall = B.CreateCall(OrigCallee, ColdArgs, CI->getName() + ".cold");
        B.CreateBr(MergeBB);
    }

    Value *HotCall = nullptr;
    {
        IRBuilder<> B(HotBB);
        Value *CapturePtr = Store->getPointerOperand();
        B.CreateStore(HotCapture, CapturePtr);
        HotCall = B.CreateCall(HotCallee, HotArgs, CI->getName() + ".hot");
        B.CreateBr(MergeBB);
    }

    if (!CI->use_empty()) {
        IRBuilder<> MB(MergeBB);
        MB.SetInsertPoint(&*MergeBB->begin());
        PHINode *PHI = MB.CreatePHI(CI->getType(), 2, CI->getName() + ".sf.sel");
        PHI->addIncoming(HotCall, HotBB);
        PHI->addIncoming(ColdCall, ColdBB);
        CI->replaceAllUsesWith(PHI);
    }

    CI->eraseFromParent();

    errs() << "[lambda-escape] rewrote std::function ctor call for closure '" << ClosureTy->getName()
           << "' field " << Key.FieldIndex << " hot=" << Prof.HotValue << " callee=" << OrigCallee->getName()
           << " -> " << HotCallee->getName() << "\n";

    return true;
}

// ----------------- pass body -----------------

PreservedAnalyses LambdaEscapeOptimizer::run(Module &M, ModuleAnalysisManager &) {
    CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

    for (const auto &[Key, Prof] : Profiles) {
        errs() << "[lambda-escape] Profile entry: closure='" << Key.Name << "' field=" << Key.FieldIndex
               << " hot=" << Prof.HotValue << "\n";

        StructType *ClosureTy = findClosureType(M, Key);
        if (!ClosureTy) {
            errs() << "  -> closure type not found in module\n";
            continue;
        }

        errs() << "  -> closure type found: " << ClosureTy->getName() << "\n";
        errs() << "  -> scanning candidate operator() bodies:\n";

        Function *Op = findLambdaOperatorFunc(M, ClosureTy);
        if (!Op) {
            errs() << "  -> operator() not found for closure\n";
            continue;
        }

        int DirectCalls = 0;
        for (User *U : Op->users()) {
            if (auto *CI = dyn_cast<CallInst>(U)) {
                if (CI->getCalledFunction() == Op)
                    ++DirectCalls;
            }
        }

        errs() << "  -> selected operator(): " << Op->getName() << " (direct calls: " << DirectCalls << ")\n";

        // Scan for ctor call sites and attempt rewrite.
        bool Any = false;
        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                SmallVector<CallInst *, 8> Calls;
                for (Instruction &I : BB)
                    if (auto *CI = dyn_cast<CallInst>(&I))
                        Calls.push_back(CI);

                for (CallInst *CI : Calls) {
                    if (rewriteStdFunctionConstructionWithBranch(CI, ClosureTy, Key, Prof)) {
                        Any = true;
                    }
                }
            }
        }

        if (!Any) {
            errs() << "  -> no std::function ctor sites rewritten for this closure\n";
        }
    }

    return PreservedAnalyses::all();
}

} // namespace lambdaopt
