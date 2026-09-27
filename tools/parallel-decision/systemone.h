#pragma once

// TypeSafe System One wire format (POST /v1/systemone) on top of the decision engine.
//
// Each question becomes one engine field: noul -> true/false, choice -> the criteria keys as JSON
// strings, score -> the level indices 0..n-1. Every field is scored in tree mode, so each answer
// carries the exact distribution over its allowed values.
//
// Extension: a choice question may set "x_labels": "names" (default, the model writes the option
// name), "letters" (A..Z) or "numbers" (1..N), listed next to each name in the prompt. Answers
// stay keyed by the option names.

#include "decision-engine.h"
#include "json.h"

#include <string>

namespace llama_decision {
namespace systemone {

// local policy, not part of the official schema
constexpr size_t max_questions = 32;
constexpr size_t max_options   = 255;

enum class confidence_mode {
    entropy, // 1 - normalised entropy of the distribution
    max,     // highest probability
};

// throws std::invalid_argument for an unknown name
confidence_mode parse_confidence(const std::string & name);

// FastAPI style validation errors: [{"type", "loc", "msg", "input", "ctx"}]; empty when valid.
common_json validate(const common_json & body);

// Errors for a body that is empty or is not valid JSON.
common_json body_errors(const std::string & body, const std::string & parse_error);

// Parses and validates a request body into parsed; returns the errors (empty when valid).
common_json parse_request(const std::string & body, common_json & parsed);

// Response body of a failed request: {"detail": detail}.
std::string error_body(const common_json & detail);

// The state as the user message: strings as they are, objects and arrays as indented JSON.
std::string render_state(const common_json & state);

// Engine fields and the cacheable system text for validated questions.
compiled_schema compile(const common_json & questions);

// Prompt layout (extension "x_layout", server option --systemone-layout):
//   questions_first: all questions in the system turn (cached across requests), the state in the user turn,
//                    each question scored as the first key of the answer object;
//   state_first:     the state in the user turn, then per question a branch with only that question's block,
//                    the end of the turn and '{"name": ' (as llamacpp-jev and openjev-sglang lay it out).
enum class layout {
    questions_first,
    state_first,
};

// throws std::invalid_argument for an unknown name
layout parse_layout(const std::string & name);

struct prompt {
    compiled_schema          cs;
    std::string              shared;   // cached static prefix
    std::vector<std::string> contexts; // per state: the dynamic part the branches fork from
};

// Everything decide_batch needs for validated questions and rendered states.
prompt build(const common_chat_templates * tmpls, bool use_jinja, const common_json & questions,
             const std::vector<std::string> & states, layout lay);

// The "answers" object for validated questions, in request order.
common_json answers(const common_json & questions, const result & r, confidence_mode mode);

// Modification date of a file as YYYY-MM-DD (UTC), or "" if it cannot be read.
std::string file_date(const std::string & path);

} // namespace systemone
} // namespace llama_decision
