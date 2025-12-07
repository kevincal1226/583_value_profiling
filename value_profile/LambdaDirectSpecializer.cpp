#include "LambdaDirectSpecializer.hpp"

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

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lambdaopt {

bool isProbablyUserCode(const Function &F) {
    if (F.isDeclaration())
        return false;

    std::string Mangled = F.getName().str();
    std::string Demangled = llvm::demangle(Mangled);

    // Very dumb but effective: skip obvious library / runtime code.
    if (Demangled.find("std::") != std::string::npos)
        return false;
    if (Demangled.find("__gnu_cxx::") != std::string::npos)
        return false;
    if (Demangled.find("llvm::") != std::string::npos)
        return false;
    if (Demangled.find("__clang") != std::string::npos)
        return false;
    if (Demangled.find("__cxxabiv1") != std::string::npos)
        return false;

    return true;
}

StructType *findClosureType(const Module &M, const LambdaCaptureKey &Key) {
    const std::string &TargetName = Key.Name;

    for (StructType *ST : M.getIdentifiedStructTypes()) {
        if (ST->isOpaque())
            continue;
        if (ST->hasName() && ST->getName() == TargetName)
            return ST;
    }

    errs() << "[lambda-opt] Could not find StructType for lambda '" << TargetName << "' in module.\n";
    return nullptr;
}

// Prefer the lambda's operator(), skip obvious std::function internals.
Function *findLambdaOperatorFunc(Module &M, StructType *ClosureTy) {
    if (!ClosureTy) {
        errs() << "[lambda-opt] No ClosureType provided.\n";
        return nullptr;
    }

    PointerType *ThisPtrTy = ClosureTy->getPointerTo();
    Function *BestLambdaLike = nullptr;
    Function *Fallback = nullptr;

    for (Function &F : M) {
        if (F.isDeclaration() || F.arg_empty())
            continue;

        Argument &FirstArg = *F.arg_begin();
        if (FirstArg.getType() != ThisPtrTy)
            continue;

        std::string Mangled = F.getName().str();
        std::string Demangled = llvm::demangle(Mangled);

        // Skip obvious std::function operators
        if (Demangled.find("std::") != std::string::npos && Demangled.find("function") != std::string::npos)
            continue;

        bool HasOperatorCall = (Demangled.find("operator()") != std::string::npos);

        bool LooksLikeLambda = (Demangled.find("$_") != std::string::npos) ||
                               (Demangled.find("lambda") != std::string::npos) ||
                               (Demangled.find(ClosureTy->getName().str()) != std::string::npos);

        if (HasOperatorCall && LooksLikeLambda) {
            // Strong match: likely the lambda's operator()
            return &F;
        }

        if (!BestLambdaLike && HasOperatorCall)
            BestLambdaLike = &F;
        else if (!Fallback)
            Fallback = &F;
    }

    if (BestLambdaLike) {
        errs() << "[lambda-opt] Using best lambda-like operator for '" << ClosureTy->getName()
               << "': " << BestLambdaLike->getName() << "\n";
        return BestLambdaLike;
    }

    if (!Fallback) {
        errs() << "[lambda-opt] Could not find operator() for lambda struct '" << ClosureTy->getName() << "'\n";
    }
    else {
        errs() << "[lambda-opt] Using fallback operator for lambda struct '" << ClosureTy->getName()
               << "': " << Fallback->getName() << "\n";
    }
    return Fallback;
}

Function *cloneLambdaOperator(Function *OriginalFunc, const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
    if (!OriginalFunc)
        return nullptr;

    Module *M = OriginalFunc->getParent();
    std::string NewName = OriginalFunc->getName().str();
    NewName += ".hot_lambda_operator";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    FunctionType *FTy = OriginalFunc->getFunctionType();
    Function *NewF = Function::Create(FTy, OriginalFunc->getLinkage(), NewName, M);

    NewF->setCallingConv(OriginalFunc->getCallingConv());

    ValueToValueMapTy VMap;
    {
        auto A = OriginalFunc->arg_begin();
        auto B = NewF->arg_begin();
        for (; A != OriginalFunc->arg_end(); ++A, ++B) {
            B->setName(A->getName());
            VMap[&*A] = &*B;
        }
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NewF, OriginalFunc, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

    errs() << "[lambda-opt] Cloned operator(): " << OriginalFunc->getName() << " -> " << NewF->getName() << "\n";

    return NewF;
}

Value *stripPointerCasts(Value *V) {
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

// Replace loads from closure field with constant in clone.
bool specializeCaptureInClone(Function *CloneF, StructType *ClosureTy, const LambdaCaptureKey &Key,
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

            // --- Support integer ---
            if (auto *IntTy = dyn_cast<IntegerType>(FieldTy)) {
                ConstVal = ConstantInt::get(IntTy, Prof.HotValue,
                                            /*isSigned*/ true);
            }
            // --- Support double (or other FP type) ---
            else if (FieldTy->isFloatingPointTy()) {
                ConstVal = ConstantFP::get(FieldTy, static_cast<double>(Prof.HotValue));
            }
            else {
                errs() << "[lambda-opt] Capture field " << Key.FieldIndex << " in lambda '" << ClosureTy->getName()
                       << "' is not int/double; skipping.\n";
                continue;
            }

            LI->replaceAllUsesWith(ConstVal);
            ToErase.push_back(LI);
            Changed = true;

            errs() << "[lambda-opt] Specialized capture field " << Key.FieldIndex << " in clone '" << CloneF->getName()
                   << "' with value " << Prof.HotValue << " (" << (FieldTy->isIntegerTy() ? "int" : "double") << ")\n";
        }
    }

    for (Instruction *I : ToErase)
        I->eraseFromParent();

    return Changed;
}

std::vector<unsigned> getAllCaptureFields(StructType *ClosureTy) {
    std::vector<unsigned> fields;
    if (!ClosureTy)
        return fields;

    unsigned Num = ClosureTy->getNumElements();
    for (unsigned i = 0; i < Num; ++i) {
        Type *T = ClosureTy->getElementType(i);
        if (T->isIntegerTy() || T->isFloatingPointTy())
            fields.push_back(i);
    }
    return fields;
}

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

llvm::PreservedAnalyses DirectLambdaSpecializer::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
    CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

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
    }

    return PreservedAnalyses::none();
}

} // namespace lambdaopt
