#pragma once

#include "LambdaCommon.hpp"
#include "llvm/IR/Module.h"

namespace lambdaopt {

class EscapingLambdaSpecializer {
    CaptureProfileMap Profiles;

  public:
    explicit EscapingLambdaSpecializer(CaptureProfileMap &P) : Profiles(P) {}

    // Specialize escaping lambdas by branching once at std::function construction.
    bool run(llvm::Module &M);
};

} // namespace lambdaopt
