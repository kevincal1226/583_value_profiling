#include "LambdaCommon.hpp"

#include <fstream>
#include <sstream>

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace lambdaopt {

// ================= Profile loading =================

CaptureProfileMap loadCaptureProfiles(const std::string &Path) {
    CaptureProfileMap Profiles;

    std::ifstream in(Path);
    if (!in) {
        errs() << "[lambda-prof] Could not open profile file: " << Path << "\n";
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

    errs() << "[lambda-prof] Loaded " << Profiles.size() << " hot capture entries from " << Path << "\n";
    for (const auto &Entry : Profiles) {
        const auto &key = Entry.first;
        const auto &prof = Entry.second;
        errs() << "  - " << key.Name << ", field " << key.FieldIndex << " -> value " << prof.HotValue
               << " (freq=" << prof.Freq << ", count=" << prof.Count << ")\n";
    }

    return Profiles;
}

// ================= Shared helpers =================

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
    NewName += ".hot.field";
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

} // namespace lambdaopt
