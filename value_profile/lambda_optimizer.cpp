#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/Casting.h>

#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfData.inc"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace {

struct LambdaCaptureKey {
    std::string Name;    // e.g. "class.anon"
    unsigned FieldIndex; // capture index in the struct

    bool operator==(const LambdaCaptureKey &Other) const {
        return FieldIndex == Other.FieldIndex && Name == Other.Name;
    }
};

struct LambdaCaptureKeyHash {
    std::size_t operator()(const LambdaCaptureKey &Key) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(Key.Name);
        std::size_t h2 = std::hash<unsigned>{}(Key.FieldIndex);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct CaptureProfile {
    int64_t HotValue; // value we will bake in
    double Freq;      // frequency (0–1)
    uint64_t Count;   // occurrences (debug/info)
};

using CaptureProfileMap = std::unordered_map<LambdaCaptureKey, CaptureProfile, LambdaCaptureKeyHash>;

// ================= Profile loading =================

CaptureProfileMap loadCaptureProfiles(const std::string &Path) {
    CaptureProfileMap Profiles;

    std::ifstream in(Path);
    if (!in) {
        llvm::errs() << "[lambda-prof] Could not open profile file: " << Path << "\n";
        return Profiles;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;

        // Allow comment lines starting with '#'
        if (!line.empty() && line[0] == '#')
            continue;

        std::stringstream ss(line);
        std::string lambdaName, fieldStr, valueStr, countStr, freqStr;

        if (!std::getline(ss, lambdaName, ','))
            continue;
        if (!std::getline(ss, fieldStr, ','))
            continue;
        if (!std::getline(ss, valueStr, ','))
            continue;
        if (!std::getline(ss, countStr, ','))
            continue;
        if (!std::getline(ss, freqStr, ','))
            continue;

        unsigned fieldIndex = static_cast<unsigned>(std::stoul(fieldStr));
        int64_t value = static_cast<int64_t>(std::stoll(valueStr));
        uint64_t count = static_cast<uint64_t>(std::stoull(countStr));
        double freq = std::stod(freqStr);

        // Apply threshold: only keep hot captures
        if (freq < 0.8)
            continue;

        LambdaCaptureKey key{lambdaName, fieldIndex};
        CaptureProfile prof{value, freq, count};

        auto it = Profiles.find(key);
        if (it == Profiles.end() || freq > it->second.Freq) {
            Profiles[key] = prof; // keep hottest for this (lambda, field)
        }
    }

    llvm::errs() << "[lambda-prof] Loaded " << Profiles.size() << " hot capture entries from " << Path << "\n";
    for (const auto &Entry : Profiles) {
        const auto &key = Entry.first;
        const auto &prof = Entry.second;
        llvm::errs() << "  - " << key.Name << ", field " << key.FieldIndex << " -> value " << prof.HotValue
                     << " (freq=" << prof.Freq << ", count=" << prof.Count << ")\n";
    }

    return Profiles;
}

// ================= Shared helpers =================

static bool isProbablyUserCode(const Function &F) {
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

    // You can also require *something* that looks like your code:
    // e.g. your main file name, namespace, etc.
    // if (Demangled.find("main") == std::string::npos &&
    //     Demangled.find("MyProject") == std::string::npos)
    //   return false;

    return true;
}

static StructType *findClosureType(const Module &M, const LambdaCaptureKey &Key) {
    const std::string &TargetName = Key.Name;

    for (StructType *ST : M.getIdentifiedStructTypes()) {
        if (ST->isOpaque())
            continue;
        if (ST->hasName() && ST->getName() == TargetName)
            return ST;
    }

    llvm::errs() << "[lambda-opt] Could not find StructType for lambda '" << TargetName << "' in module.\n";
    return nullptr;
}

// Prefer the lambda's operator(), skip obvious std::function internals.
static Function *findLambdaOperatorFunc(Module &M, StructType *ClosureTy) {
    if (!ClosureTy) {
        llvm::errs() << "[lambda-opt] No ClosureType provided.\n";
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
        llvm::errs() << "[lambda-opt] Using best lambda-like operator for '" << ClosureTy->getName()
                     << "': " << BestLambdaLike->getName() << "\n";
        return BestLambdaLike;
    }

    if (!Fallback) {
        llvm::errs() << "[lambda-opt] Could not find operator() for lambda "
                        "struct '"
                     << ClosureTy->getName() << "'\n";
    }
    else {
        llvm::errs() << "[lambda-opt] Using fallback operator for lambda "
                        "struct '"
                     << ClosureTy->getName() << "': " << Fallback->getName() << "\n";
    }
    return Fallback;
}

static Function *cloneLambdaOperator(Function *OriginalFunc, const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
    if (!OriginalFunc)
        return nullptr;

    Module *M = OriginalFunc->getParent();
    std::string NewName = OriginalFunc->getName().str() + ".hot.field" + std::to_string(Key.FieldIndex) + ".value" +
                          std::to_string(Prof.HotValue);

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

    llvm::errs() << "[lambda-opt] Cloned operator(): " << OriginalFunc->getName() << " -> " << NewF->getName() << "\n";

    return NewF;
}

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
                llvm::errs() << "[lambda-opt] Capture field " << Key.FieldIndex << " in lambda '"
                             << ClosureTy->getName() << "' is not int/double; skipping.\n";
                continue;
            }

            LI->replaceAllUsesWith(ConstVal);
            ToErase.push_back(LI);
            Changed = true;

            llvm::errs() << "[lambda-opt] Specialized capture field " << Key.FieldIndex << " in clone '"
                         << CloneF->getName() << "' with value " << Prof.HotValue << " ("
                         << (FieldTy->isIntegerTy() ? "int" : "double") << ")\n";
        }
    }

    for (Instruction *I : ToErase)
        I->eraseFromParent();

    return Changed;
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
        llvm::errs() << "[lambda-opt] cloneAndSpecializeForCapture: no "
                        "matching loads in clone '"
                     << NewF->getName() << "'; erasing.\n";
        NewF->eraseFromParent();
        return nullptr;
    }

    llvm::errs() << "[lambda-opt] Escaping: created hot specialization '" << NewF->getName() << "' from '"
                 << OrigF->getName() << "' for lambda '" << Key.Name << "', field " << Key.FieldIndex << " = "
                 << Prof.HotValue << "\n";

    return NewF;
}

// Backwards-scan to find store to the capture field in the same block.
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

static std::vector<unsigned> getAllCaptureFields(StructType *ClosureTy) {
    std::vector<unsigned> fields;
    if (!ClosureTy)
        return fields;

    // Skip the vptr / magic LLVM indices; real captures start at field 0 or 1
    unsigned Num = ClosureTy->getNumElements();
    for (unsigned i = 0; i < Num; ++i) {
        Type *T = ClosureTy->getElementType(i);
        if (T->isIntegerTy() || T->isFloatingPointTy())
            fields.push_back(i);
    }
    return fields;
}

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
            llvm::errs() << "[lambda-opt] Field index " << Key.FieldIndex << " out of range for closure '"
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
            llvm::errs() << "[lambda-opt] Capture field " << Key.FieldIndex << " in closure '" << ClosureTy->getName()
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
            // Ordered equal: true if both are numbers and equal.
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

        llvm::errs() << "[lambda-opt] Rewrote call site of '" << OriginalOp->getName() << "' to guard on closure field "
                     << Key.FieldIndex << " == " << Prof.HotValue << " (type "
                     << (FieldTy->isIntegerTy() ? "int" : "fp") << ")\n";

        Changed = true;
    }

    return Changed;
}
// ================= Direct (non-escaping) specializer =================

class DirectLambdaSpecializer {
    CaptureProfileMap &Profiles;

  public:
    DirectLambdaSpecializer(CaptureProfileMap &P) : Profiles(P) {}

    bool run(Module &M) {
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
                llvm::errs() << "[lambda-opt] Direct: hot clone '" << HotClone->getName() << "' unused; erasing.\n";
                HotClone->eraseFromParent();
                continue;
            }

            llvm::errs() << "[lambda-opt] Direct: created hot operator clone '" << HotClone->getName()
                         << "' for lambda '" << Key.Name << "', field " << Key.FieldIndex << " = " << Prof.HotValue
                         << "\n";

            Changed = true;
        }

        return Changed;
    }
};

// ================= Escaping: branch once at std::function construction =================

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
        break;
    }

    if (CaptureArgIndex < 0) {
        return false;
    }

    // 2) Find the store to that closure field earlier in the block: this gives us "x".
    StoreInst *Store = findLastStoreToClosureFieldBefore(CI, ClosureTy, Key);
    if (Store == nullptr) {
        llvm::errs() << "[lambda-opt] Escaping: could not find store to "
                        "closure field before std::function ctor; skipping.\n";
        return false;
    }

    Value *CaptureVal = Store->getValueOperand(); // this is "x"
    auto *CaptureTy = dyn_cast<IntegerType>(CaptureVal->getType());
    auto *CtorArgTy = dyn_cast<IntegerType>(ZExt->getType());

    if (!CaptureTy || !CtorArgTy) {
        llvm::errs() << "[lambda-opt] Escaping: non-integer capture or ctor arg; "
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

    llvm::errs() << "[lambda-opt] Escaping: branched std::function construction for "
                    "closure '"
                 << ClosureTy->getName() << "', field " << Key.FieldIndex << " on value == " << Prof.HotValue
                 << " using specialized callee '" << HotCallee->getName() << "'\n";

    return true;
}

class EscapingLambdaSpecializer {
    CaptureProfileMap &Profiles;

  public:
    EscapingLambdaSpecializer(CaptureProfileMap &P) : Profiles(P) {}

    bool run(Module &M) {
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
};

// ================= Top-level pass =================

class LambdaOptimizer : public PassInfoMixin<LambdaOptimizer> {
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

        // DirectLambdaSpecializer(Profiles).run(M);
        EscapingLambdaSpecializer(Profiles).run(M);

        return PreservedAnalyses::none();
    }
};

} // namespace
