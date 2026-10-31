#pragma once

#include "llama.h"
#include "json.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// vocab introspection, selector resolution, compiled plan + fingerprint

#define RESEARCH_COMPILER_VERSION "research-compiler-1"

std::string research_sha256(const std::string & data); // "sha256:<hex>"
std::string research_base64(const std::string & data);
std::string research_escape_piece(const std::string & piece);
bool research_valid_utf8(const std::string & s);

struct research_vocab {
    const llama_vocab * vocab = nullptr;
    int32_t n_vocab = 0;
    std::string model_id;
    std::string fingerprint;

    std::vector<std::string> pieces;  // raw bytes, special tokens rendered
    std::vector<const char *> types;  // normal|control|user_defined|byte|unused|unknown
    std::vector<uint8_t> special;
    std::unordered_map<std::string, std::vector<llama_token>> by_piece;

    void init(const llama_model * model, const std::string & model_id);

    bool valid(int64_t id) const { return id >= 0 && id < n_vocab; }
    common_json token_json(llama_token id) const;
};

struct research_plan_group {
    std::string id;
    std::string name;
    std::vector<llama_token> members;
    std::vector<uint32_t> watch_idx;
};

// runtime view of a compiled plan
struct research_plan {
    std::string fingerprint;
    std::string tokenizer_fingerprint;
    std::vector<llama_token> watch_ids;
    std::vector<research_plan_group> groups;

    // from CompiledPlan json
    static research_plan from_json(const common_json & plan);
};

// compile a DesignSpec; returns the CompiledPlan json (compiled_id null)
common_json research_compile(const research_vocab & rv, const common_json & spec);
