#include "LambdaCommon.hpp"
#include "LambdaDirectSpecializer.hpp"
#include "LambdaEscapingSpecializer.hpp"

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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace lambdaopt {

class LambdaOptimizer : public PassInfoMixin<LambdaOptimizer> {
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        // TODO: parameterize this path via pass options / env if needed.
        CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

        bool Changed = false;

        // Keep behavior identical to your original file:
        // Direct specialization exists but is disabled by default.
        //
        Changed |= DirectLambdaSpecializer(Profiles).run(M);
        Changed |= EscapingLambdaSpecializer(Profiles).run(M);

        (void)Changed; // currently we always report "none()" regardless

        return PreservedAnalyses::none();
    }
};

} // namespace lambdaopt
