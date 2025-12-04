// Pass that *detects* escaping lambdas and reports the operator() we think
// belongs to each profiled closure. No rewriting yet: we want robust
// identification first.

#include <string>

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
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

        // For now, stop at identification: no IR is mutated.
    }

    return PreservedAnalyses::all();
}

} // namespace lambdaopt
