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

namespace lambdaopt {

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

CaptureProfileMap loadCaptureProfiles(const std::string &Path);
} // namespace lambdaopt
