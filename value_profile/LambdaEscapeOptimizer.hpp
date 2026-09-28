// Escaping lambda optimizer: focuses on identifying the correct lambda
// closures/operator() bodies for hot capture specialization. This header
// intentionally only declares the pass; the implementation lives in
// LambdaEscapeOptimizer.cpp.

#pragma once

#include "llvm/IR/PassManager.h"

namespace lambdaopt {

class LambdaEscapeOptimizer : public llvm::PassInfoMixin<LambdaEscapeOptimizer> {
  public:
    llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

} // namespace lambdaopt
