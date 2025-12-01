
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

namespace {

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
        // simple combine
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct CaptureProfile {
    int64_t HotValue; // value we will bake in
    double Freq;      // frequency (0–1)
    uint64_t Count;   // occurrences (debug/info)
};

using CaptureProfileMap = std::unordered_map<LambdaCaptureKey, CaptureProfile, LambdaCaptureKeyHash>;

CaptureProfileMap loadCaptureProfiles(const std::string &Path) {
    CaptureProfileMap Profiles;

    std::ifstream in(Path);
    if (!in) {
        llvm::errs() << "[lambda-prof] Could not open profile file: " << Path << "\n";
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

    // Optional debug dump:
    llvm::errs() << "[lambda-prof] Loaded " << Profiles.size() << " hot capture entries from " << Path << "\n";
    for (const auto &Entry : Profiles) {
        const auto &key = Entry.first;
        const auto &prof = Entry.second;
        llvm::errs() << "  - " << key.Name << ", field " << key.FieldIndex << " -> value " << prof.HotValue
                     << " (freq=" << prof.Freq << ", count=" << prof.Count << ")\n";
    }

    return Profiles;
}

class LambdaOptimizer : public PassInfoMixin<LambdaOptimizer> {

  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        // later we’ll probably make this configurable
        CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

        // next phases (we’ll write later):
        //   - find StructType* and operator() for each entry
        //   - clone and specialize
        //   - rewrite call sites

        return PreservedAnalyses::none(); // for now we’ll be mutating later
    }
};

} // namespace
