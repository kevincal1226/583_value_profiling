#pragma once

#include "LambdaCommon.hpp"
#include "llvm/IR/Module.h"

namespace lambdaopt {

class DirectLambdaSpecializer {
    CaptureProfileMap Profiles;

  public:
    explicit DirectLambdaSpecializer(CaptureProfileMap &P) : Profiles(P) {}

    // Specialize direct lambda operator() calls based on hot capture values.
    bool run(llvm::Module &M);
};

} // namespace lambdaopt
