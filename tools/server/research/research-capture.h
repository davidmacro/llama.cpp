#pragma once

#include "research-compile.h"
#include "research-math.h"

#include "llama.h"
#include "json.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// per-run collector used by the slot sampling path, plus the in-memory event log for SSE and the DB writer

struct research_event {
    int64_t     seq;
    std::string name;
    common_json data;
};

struct research_run {
    std::string id;

    // config, immutable once the run is posted
    research_plan plan;
    std::map<llama_token, research_bias> bias; // effective bias of the arm
    std::vector<float> watch_bias;             // per watch index, -inf when excluded
    std::vector<uint8_t> watch_excluded;
    int32_t every_n    = 1;
    bool    normalized = false; // watch_normalized: compute full-vocab log Z
    int32_t top_k      = 0;
    const research_vocab * rv = nullptr;

    // sampling thread state, guarded by mtx_hook; closed = no more hooks accepted
    std::mutex mtx_hook;
    bool    closed   = false;
    int32_t n_gen    = 0;
    int32_t captured = 0;
    int32_t skipped  = 0;
    int32_t dropped  = 0;
    bool        has_pending = false;
    common_json pending;
    std::string text;

    std::atomic<bool> cancel{false};

    // event log, read by SSE readers and the DB writer
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<research_event> events;
    bool finished = false;

    void setup(const research_plan & p, const std::map<llama_token, research_bias> & b, int32_t every_n, bool normalized, int32_t top_k, const research_vocab * rv);

    // sampling hooks, called from the server loop; never touch SQLite
    void before_sample(const float * logits, int32_t n_vocab, int32_t ctx_pos);
    void on_token(llama_token id, int32_t ctx_pos);

    void push_event(const std::string & name, common_json data);
    void mark_finished();

    // build group metrics etc. from raw watch logits (also used by /counterfactual)
    static common_json group_metrics(
            const research_plan & plan,
            const std::vector<float> & raw,
            const std::vector<float> & watch_bias,
            const std::vector<uint8_t> & watch_excluded,
            double raw_log_z,
            double adj_log_z);
};

// JSON number, or null when not finite
common_json research_num(double v);
