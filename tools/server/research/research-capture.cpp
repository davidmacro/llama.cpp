#include "research-capture.h"

#include <algorithm>
#include <cmath>
#include <numeric>

using json = common_json;

json research_num(double v) {
    if (std::isfinite(v)) {
        return json(v);
    }
    return json(nullptr);
}

void research_run::setup(const research_plan & p, const std::map<llama_token, research_bias> & b, int32_t n, bool norm, int32_t k, const research_vocab * v) {
    plan       = p;
    bias       = b;
    every_n    = std::max(1, n);
    normalized = norm;
    top_k      = std::clamp(k, 0, 100);
    rv         = v;
    watch_bias.assign(plan.watch_ids.size(), 0.0f);
    watch_excluded.assign(plan.watch_ids.size(), 0);
    for (size_t i = 0; i < plan.watch_ids.size(); i++) {
        auto it = bias.find(plan.watch_ids[i]);
        if (it != bias.end()) {
            watch_bias[i]     = it->second.excluded ? -INFINITY : (float) it->second.delta;
            watch_excluded[i] = it->second.excluded;
        }
    }
}

json research_run::group_metrics(
        const research_plan & plan,
        const std::vector<float> & raw,
        const std::vector<float> & wb,
        const std::vector<uint8_t> & wx,
        double raw_log_z,
        double adj_log_z) {
    std::vector<float> adj(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        adj[i] = wx[i] ? -INFINITY : raw[i] + wb[i];
    }
    const double raw_all = research_logsumexp(raw.data(), raw.size());
    const double adj_all = research_logsumexp(adj.data(), adj.size());

    json groups = json::array();
    for (const auto & g : plan.groups) {
        std::vector<double> r;
        std::vector<double> a;
        for (uint32_t wi : g.watch_idx) {
            r.push_back(raw[wi]);
            a.push_back(adj[wi]);
        }
        const double rl = research_logsumexp(r);
        const double al = research_logsumexp(a);
        json item = {
            {"id",      g.id},
            {"raw_lse", research_num(rl)},
            {"adj_lse", research_num(al)},
            {"raw_rel", std::isfinite(raw_all) ? research_num(std::exp(rl - raw_all)) : json(nullptr)},
            {"adj_rel", std::isfinite(adj_all) ? json(std::isfinite(al) ? std::exp(al - adj_all) : 0.0) : json(nullptr)},
            {"raw_abs", std::isfinite(raw_log_z) ? research_num(std::exp(rl - raw_log_z)) : json(nullptr)},
            {"adj_abs", std::isfinite(adj_log_z) ? json(std::isfinite(al) ? std::exp(al - adj_log_z) : 0.0) : json(nullptr)},
        };
        groups.push_back(item);
    }
    return groups;
}

void research_run::before_sample(const float * logits, int32_t n_vocab, int32_t ctx_pos) {
    std::lock_guard<std::mutex> lock(mtx_hook);
    if (closed) {
        return;
    }
    has_pending = false;
    if (n_gen % every_n != 0) {
        skipped++;
        return;
    }

    json obs = {
        {"generation_index", n_gen},
        {"context_position", ctx_pos},
        {"selected_token_id", nullptr},
    };

    if (logits == nullptr) {
        obs["capture_state"] = "logits_unavailable";
        dropped++;
        pending     = obs;
        has_pending = true;
        return;
    }

    std::vector<float> raw(plan.watch_ids.size());
    json adj_arr = json::array();
    for (size_t i = 0; i < plan.watch_ids.size(); i++) {
        raw[i] = logits[plan.watch_ids[i]];
        adj_arr.push_back(research_num(watch_excluded[i] ? -INFINITY : raw[i] + watch_bias[i]));
    }

    double raw_log_z = NAN;
    double adj_log_z = NAN;
    if (normalized) {
        raw_log_z = research_logsumexp(logits, n_vocab);
        std::vector<std::pair<float, research_bias>> changed;
        for (const auto & [id, b] : bias) {
            changed.emplace_back(logits[id], b);
        }
        if (!research_adjusted_log_z(raw_log_z, changed, adj_log_z)) {
            adj_log_z = NAN;
        }
    }

    obs["raw"]            = raw;
    obs["adjusted"]       = adj_arr;
    obs["raw_log_z"]      = research_num(raw_log_z);
    obs["adjusted_log_z"] = research_num(adj_log_z);
    obs["groups"]         = group_metrics(plan, raw, watch_bias, watch_excluded, raw_log_z, adj_log_z);

    json topk = json::array();
    if (top_k > 0) {
        std::vector<int32_t> idx(n_vocab);
        std::iota(idx.begin(), idx.end(), 0);
        const int32_t k = std::min(top_k, n_vocab);
        std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [logits](int32_t a, int32_t b) {
            return logits[a] > logits[b];
        });
        for (int32_t i = 0; i < k; i++) {
            topk.push_back(json {{"id", idx[i]}, {"logit", logits[idx[i]]}});
        }
    }
    obs["top_k"]         = topk;
    obs["capture_state"] = "ok";

    captured++;
    pending     = std::move(obs);
    has_pending = true;
}

void research_run::on_token(llama_token id, int32_t ctx_pos) {
    std::lock_guard<std::mutex> lock(mtx_hook);
    if (closed) {
        return;
    }
    const std::string piece = rv ? rv->pieces[id] : std::string();
    text += piece;

    if (has_pending) {
        pending["selected_token_id"] = id;
        push_event("research.observation", std::move(pending));
        pending     = json();
        has_pending = false;
    }

    push_event("research.token_accepted", json {
        {"generation_index", n_gen},
        {"token_id",         id},
        {"piece",            research_valid_utf8(piece) ? piece : research_escape_piece(piece)},
        {"piece_b64",        research_base64(piece)},
        {"context_position", ctx_pos},
    });
    n_gen++;
}

void research_run::push_event(const std::string & name, json data) {
    std::lock_guard<std::mutex> lock(mtx);
    const int64_t seq = (int64_t) events.size() + 1;
    data["run_id"]    = id;
    data["event_seq"] = seq;
    events.push_back({seq, name, std::move(data)});
    cv.notify_all();
}

void research_run::mark_finished() {
    std::lock_guard<std::mutex> lock(mtx);
    finished = true;
    cv.notify_all();
}
