#pragma once

// Parallel constrained decisions for finite JSON schemas, shared by llama-parallel-decision
// and llama-server's /decision endpoint.
//
// Every field of a schema has a finite set of allowed values. After a shared context, each
// field's value is scored as token paths following that field's own suffix; every scored path
// runs as its own sequence forked from the context (llama_memory_seq_cp), so all fields are
// evaluated in one batched llama_decode and cannot see each other. Small fields score every
// divergence node of their token trie at once and return the exact constrained distribution;
// larger fields walk the trie greedily.
//
// Prompts may carry images: a prompt is then a list of pieces, each either text tokens or an mtmd
// image chunk. Text pieces are batched across sequences; image chunks are encoded with the
// multimodal projector and decoded on their own sequence.

#include "llama.h"
#include "json.h"
#include "mtmd.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct common_chat_templates;

namespace llama_decision {

using tokens_t = std::vector<llama_token>;

// One piece of a prompt: text tokens, or (when image is set) one media chunk from mtmd_tokenize.
struct prompt_piece {
    tokens_t                                toks;
    std::shared_ptr<const mtmd_input_chunk> image;

    size_t    n_tokens() const; // KV cells the piece occupies
    llama_pos n_pos()    const; // positions it advances (differs from n_tokens for M-RoPE images)
};

using prompt_pieces = std::vector<prompt_piece>;

// One field as the scorer sees it: the text before its value and the allowed value texts.
struct field_input {
    std::string              suffix;     // e.g.  '  "fire": '
    std::vector<std::string> candidates; // allowed values, with the suffix's shared prefix removed
    std::string              terminator = "\n"; // text the model writes after a value; ends each path
};

struct options {
    std::string mode           = "auto"; // auto: tree up to tree_max values, else greedy; tree; greedy
    size_t      tree_max       = 128;
    bool        split_boundary = false;  // legacy: tokenise suffix and values separately
    bool        allow_cache    = true;   // reuse the cached static prefix when it matches
};

struct field_result {
    int                winner       = -1;
    float              path_score   = 1.0f;
    int                scored_nodes = 0;
    bool               tree         = false;
    std::vector<float> probs;            // tree fields: probability of every allowed value
};

struct result {
    std::vector<field_result> fields;
    bool   cache_hit      = false;
    size_t shared_tokens  = 0;
    size_t context_tokens = 0;
    size_t shared_image_tokens  = 0; // the image tokens counted in shared_tokens / context_tokens
    size_t context_image_tokens = 0;
    int    rows           = 0;
    int    rounds         = 0;
    double prefill_ms     = 0;
    double scoring_ms     = 0;
};

// Several contexts decided against one schema and one cached prefix. Items carry fields,
// context_tokens and rows; timings and cache state cover the whole batch.
struct batch_result {
    std::vector<result> items;
    bool   cache_hit     = false;
    size_t shared_tokens = 0;
    size_t shared_image_tokens = 0;
    int    rows          = 0;
    int    rounds        = 0;
    double prefill_ms    = 0;
    double scoring_ms    = 0;
};

// Scores decisions on an existing context with the sequence ids [seq_base, seq_base + n_seqs):
// one keeps the cached static prefix; the rest hold one trunk (prefix + context) per context in
// flight, then branches. The context needs a unified KV cache so branches share the trunk's cells.
class engine {
  public:
    // mctx (optional) encodes image pieces; without it only text prompts are accepted
    engine(llama_context * ctx, llama_seq_id seq_base, int n_seqs, mtmd_context * mctx = nullptr);

    result decide(const std::string & shared_text, const std::string & context_text,
                  const std::vector<field_input> & fields, const options & opt);

    // Contexts are prefilled together and their branches scored together, in groups sized to fit
    // the sequence budget; results keep the order of the contexts.
    batch_result decide_batch(const std::string & shared_text, const std::vector<std::string> & contexts,
                              const std::vector<field_input> & fields, const options & opt);

    // The same on tokenized prompts: the shared prefix and every context as text and image pieces.
    batch_result decide_batch(const prompt_pieces & shared, const std::vector<prompt_pieces> & contexts,
                              const std::vector<field_input> & fields, const options & opt);

    // Text with one media marker per bitmap (mtmd_tokenize) as pieces; plain text without bitmaps.
    prompt_pieces tokenize_pieces(const std::string & text, const std::vector<const mtmd_bitmap *> & bitmaps,
                                  bool add_special) const;

  private:
    struct prompt_part {
        const prompt_pieces * pieces;
        llama_pos             pos0;
        llama_seq_id          seq;
    };
    struct branch {
        llama_seq_id trunk;
        llama_pos    pos0;
        tokens_t     toks;
        tokens_t     cands;
    };

    llama_context     * ctx;
    mtmd_context      * mctx;
    const llama_vocab * vocab;
    llama_memory_t      mem;
    llama_seq_id        seq_snap, seq_pool;
    int                 n_pool;
    bool                pad_branches; // recurrent/hybrid model: branches in a decode need equal lengths
    prompt_pieces       cached;
    llama_pos           cached_pos_max = -1; // last position of seq_snap right after the cached prefix was decoded

    tokens_t tokenize(const std::string & text, bool add_special) const;
    void     decode_parts(const std::vector<prompt_part> & parts);
    void     decode_image(const mtmd_input_chunk * chunk, llama_pos pos0, llama_seq_id seq, std::string & encoded_id);
    bool     prepare_prefix(const prompt_pieces & shared, bool allow_cache);
    std::vector<std::vector<float>> score_branches(const std::vector<branch> & branches, llama_seq_id first, int n_free);
};

// ---- schema compiler (the C++ counterpart of llama-mojo's tools/prepare_decisions.py)

struct field_spec {
    std::string              name;
    std::string              type;        // boolean | enum | integer | number
    std::string              description;
    std::string              aggregate;   // mode | median | mean (median/mean: numeric fields)
    std::vector<common_json> values;      // typed values; index = candidate index
    std::vector<double>      numbers;     // numeric fields: the same values as doubles
    std::vector<std::string> encoded;     // JSON text of each value
};

struct compiled_schema {
    std::string              system_text; // fixed instructions + field catalogue (cacheable)
    std::vector<field_spec>  specs;
    std::vector<field_input> inputs;
};

// Accepts compact field specs {"name": {"type": ..., "description": ..., ...}} or a JSON Schema
// object with "properties" (boolean, string+enum, integer min/max, number min/max/multipleOf).
compiled_schema compile_schema(const common_json & schema, const std::string & instructions);

// The scorer input of one field: '  "name": ' plus the values' shared leading characters, then the rest of each value.
field_input make_input(const field_spec & f, const std::string & terminator = "\n");

// Renders system + user messages with the model's chat template (thinking disabled) and splits
// the prompt into the static prefix (cached across requests) and the per-request part: the
// context, the generation prompt and the opening brace of the JSON answer.
std::pair<std::string, std::string> render_prompt(const common_chat_templates * tmpls, bool use_jinja,
                                                  const std::string & system_text, const std::string & context);

// {"decision": {...}, "fields": {...}} from the scores, applying each numeric field's aggregate.
common_json assemble(const compiled_schema & cs, const result & r);

} // namespace llama_decision
