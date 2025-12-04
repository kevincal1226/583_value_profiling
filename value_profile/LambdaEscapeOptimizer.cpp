// Pass that *detects* escaping lambdas and reports the operator() we think
// belongs to each profiled closure, then rewrites std::function construction
// to install a hot specialization when the capture matches the profiled value.

#include "llvm/ADT/DenseSet.h"
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

llvm::StructType *getLambdaStructFromArg(llvm::Value *Arg) {
    using namespace llvm;

    // Strip zext
    if (auto *ZExt = dyn_cast<ZExtInst>(Arg))
        Arg = ZExt->getOperand(0);

    // Expect a load
    auto *LI = dyn_cast<LoadInst>(Arg);
    if (!LI)
        return nullptr;
    Value *Ptr = LI->getPointerOperand();

    // Expect a GEP indexing into the closure struct
    auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
    if (!GEP)
        return nullptr;

    // THIS is now the reliable way
    Type *BaseTy = GEP->getSourceElementType();
    return dyn_cast<StructType>(BaseTy);
}

void handleStdFunctionCtor(CallInst *CI, CaptureProfileMap Profiles) {
    if (CI->arg_size() < 2)
        return;

    Value *CaptureArg = CI->getArgOperand(1); // i64 %coerce.val.ii
    if (auto *ST = getLambdaStructFromArg(CaptureArg)) {

        errs() << "Lambda closure struct: " << ST->getName() << "\n";

        auto name = ST->getName().str();

        LambdaCaptureKey LambdaCaptureKey{name, 0};

        if (!Profiles.contains(LambdaCaptureKey)) {
            errs() << "Did not find profile data for " << name << "\n";
        }

        errs() << "Found profile data for " << name << "\n";

        auto &data = Profiles[LambdaCaptureKey];

        Function *CalledFunction = CI->getCalledFunction();
    }
}

llvm::PreservedAnalyses LambdaEscapeOptimizer::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
    CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

    for (Function &F : M) {

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {

                if (auto *CI = dyn_cast<CallInst>(&I)) {

                    if (CI->getCalledFunction() == nullptr) {
                        continue;
                    }

                    auto name = CI->getCalledFunction()->getName();

                    if (name.str() == "") {
                        continue;
                    }

                    if (name.starts_with("_ZNSt3__18function") && name.contains("EEC") && name.contains("Z")) {
                        errs() << "func_name: " << demangleName(CI->getCalledFunction()->getName()) << "\n";

                        // FOUND FUNCTION

                        handleStdFunctionCtor(CI, Profiles);
                    }
                }
            }
        }
    }
}

} // namespace lambdaopt
