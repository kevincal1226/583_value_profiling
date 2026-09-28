#pragma once

#include "LambdaCommon.hpp"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace lambdaopt {

class DirectLambdaSpecializer : public llvm::PassInfoMixin<DirectLambdaSpecializer> {
  public:
    // Specialize direct lambda operator() calls based on hot capture values.
    llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

} // namespace lambdaopt
