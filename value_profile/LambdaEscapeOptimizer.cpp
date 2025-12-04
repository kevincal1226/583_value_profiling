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

} // namespace lambdaopt
