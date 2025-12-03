#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

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

// ================= Shared helpers =================

bool isProbablyUserCode(const llvm::Function &F);

llvm::StructType *findClosureType(const llvm::Module &M, const LambdaCaptureKey &Key);

// Prefer the lambda's operator(), skip obvious std::function internals.
llvm::Function *findLambdaOperatorFunc(llvm::Module &M, llvm::StructType *ClosureTy);

// Clone a lambda operator() and give it a name encoding the hot capture value.
llvm::Function *cloneLambdaOperator(llvm::Function *OriginalFunc, const LambdaCaptureKey &Key,
                                    const CaptureProfile &Prof);

// Strip bitcasts / addrspace casts from a pointer value.
llvm::Value *stripPointerCasts(llvm::Value *V);

// Replace loads from closure field with constant in clone.
bool specializeCaptureInClone(llvm::Function *CloneF, llvm::StructType *ClosureTy, const LambdaCaptureKey &Key,
                              const CaptureProfile &Prof);

// Collect all integer / FP capture fields in a closure struct.
std::vector<unsigned> getAllCaptureFields(llvm::StructType *ClosureTy);

} // namespace lambdaopt
