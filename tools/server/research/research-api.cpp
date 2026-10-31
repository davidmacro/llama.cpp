#include "research-api.h"
#include "research-capture.h"
#include "research-compile.h"
#include "research-math.h"
#include "research-store.h"

#include "server-common.h"
#include "server-context.h"

#include "build-info.h"
#include "common.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#define RESEARCH_BASE "/api/research/v1"
#define RESEARCH_STAGE_SEMANTICS "raw = model logits row at the sampling index, before any sampler; post_intervention = raw + effective bias (computed, not read back); later samplers (penalties, temperature, top-k/p) are not reflected"

struct research_error : std::runtime_error {
    int status;
    std::string code;
    research_error(int status, std::string code, const std::string & msg) : std::runtime_error(msg), status(status), code(std::move(code)) {}
};

static server_http_res_ptr make_res(const json & body, int status = 200) {
    auto res = std::make_unique<server_http_res>();
    res->status = status;
    res->data   = safe_json_to_str(body);
    return res;
}

static server_http_res_ptr make_err(int status, const std::string & code, const std::string & msg) {
    return make_res(json {{"error", {{"code", code}, {"message", msg}}}}, status);
}

static json parse_body(const server_http_req & req) {
    if (req.body.empty()) {
        return json::object();
    }
    json body = json::parse_no_throw(req.body);
    if (body.is_discarded() || !body.is_object()) {
        throw research_error(400, "INVALID_JSON", "request body must be a JSON object");
    }
    return body;
}

// string or null
static json opt_str(const json & j, const std::string & key) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return json(nullptr);
    }
    if (!j.at(key).is_string()) {
        throw research_error(400, "INVALID_FIELD", key + " must be a string");
    }
    return j.at(key);
}

static std::string req_str(const json & j, const std::string & key) {
    if (!j.contains(key) || !j.at(key).is_string() || j.at(key).get<std::string>().empty()) {
        throw research_error(400, "INVALID_FIELD", key + " is required");
    }
    return j.at(key).get<std::string>();
}

static long long param_int(const server_http_req & req, const std::string & key, long long def) {
    const std::string v = req.get_param(key);
    if (v.empty()) {
        return def;
    }
    try {
        return std::stoll(v);
    } catch (...) {
        throw research_error(400, "INVALID_PARAM", key + " must be an integer");
    }
}

struct research_api::impl {
    const common_params & params;
    server_context & ctx_server;

    bool enabled = false;
    research_store store;
    research_vocab rv;
    std::string build_info;

    // runs that are queued or running; removed once fully persisted
    std::mutex mtx_live;
    std::map<std::string, std::shared_ptr<research_run>> live;

    std::mutex mtx_q;
    std::condition_variable cv_q;
    std::deque<std::string> queue; // batch ids
    std::atomic<bool> stopping{false};
    std::thread worker;

    impl(const common_params & params, server_context & ctx_server) : params(params), ctx_server(ctx_server) {}

    std::shared_ptr<research_run> get_live(const std::string & id) {
        std::lock_guard<std::mutex> lock(mtx_live);
        auto it = live.find(id);
        return it == live.end() ? nullptr : it->second;
    }

    //
    // loaders
    //

    json must(const json & row, const std::string & what, const std::string & id) {
        if (row.is_null()) {
            throw research_error(404, "NOT_FOUND", what + " not found: " + id);
        }
        return row;
    }

    json get_project(const std::string & id) {
        return must(store.query_one("SELECT * FROM projects WHERE id = ?", {id}), "project", id);
    }

    json get_question(const std::string & id) {
        return must(store.query_one("SELECT * FROM questions WHERE id = ?", {id}), "question", id);
    }

    json get_design(const std::string & id) {
        return must(store.query_one("SELECT * FROM designs WHERE id = ?", {id}), "design", id);
    }

    json get_compiled(const std::string & id) {
        json row = must(store.query_one("SELECT plan_json FROM compiled_plans WHERE id = ?", {id}), "compiled plan", id);
        return row.at("plan");
    }

    json get_intervention(const std::string & id) {
        return must(store.query_one("SELECT * FROM interventions WHERE id = ?", {id}), "intervention", id);
    }

    json run_summary(json row) {
        auto run = get_live(row.at("id").get<std::string>());
        if (run && row.at("status").get<std::string>() == "running") {
            std::lock_guard<std::mutex> lock(run->mtx_hook);
            row["n_tokens"] = run->n_gen;
            row["captured"] = run->captured;
            row["skipped"]  = run->skipped;
            row["dropped"]  = run->dropped;
            row["text"]     = run->text;
        }
        row.erase("manifest");
        return row;
    }

    json get_run_row(const std::string & id) {
        return must(store.query_one("SELECT * FROM runs WHERE id = ?", {id}), "run", id);
    }

    json run_manifest(const std::string & id) {
        json row = get_run_row(id);
        json manifest = row.at("manifest");
        json out = run_summary(row);
        for (const auto & [k, v] : manifest.items()) {
            out[k] = v;
        }
        return out;
    }

    //
    // effective bias
    //

    static std::map<std::string, std::vector<llama_token>> plan_groups(const json & plan) {
        std::map<std::string, std::vector<llama_token>> groups;
        for (const auto & g : plan.at("groups")) {
            auto & m = groups[g.at("id").get<std::string>()];
            for (const auto & id : g.at("members")) {
                m.push_back(id.get<int>());
            }
        }
        return groups;
    }

    static std::vector<research_rule> parse_rules(const json & rules) {
        if (!rules.is_array()) {
            throw research_error(400, "INVALID_FIELD", "rules must be an array");
        }
        std::vector<research_rule> out;
        int n = 0;
        for (const auto & r : rules) {
            research_rule rule;
            n++;
            rule.id              = r.value("id", "r" + std::to_string(n));
            rule.target_group_id = r.value("target_group_id", std::string());
            rule.operation       = r.value("operation", std::string("add_logit_bias"));
            rule.enabled         = r.value("enabled", true);
            if (r.contains("delta") && !r.at("delta").is_null()) {
                if (!r.at("delta").is_number()) {
                    throw research_error(400, "INVALID_DELTA", "rule " + rule.id + ": delta must be a number");
                }
                rule.delta = r.at("delta").get<double>();
            } else if (rule.operation == "add_logit_bias") {
                throw research_error(400, "INVALID_DELTA", "rule " + rule.id + ": delta is required");
            }
            out.push_back(rule);
        }
        return out;
    }

    // returns entries, fills warnings; throws on invalid rules
    json effective_entries(const std::vector<research_rule> & rules, const json & plan, json & warnings) {
        std::map<llama_token, research_bias> bias;
        std::vector<std::string> warn;
        std::string err;
        if (!research_effective_bias(rules, plan_groups(plan), bias, warn, err)) {
            throw research_error(err.find("delta") != std::string::npos ? 400 : 422, "INVALID_RULE", err);
        }
        // reject a plan that masks every token of every watched group
        bool any_left = bias.empty();
        for (const auto & g : plan_groups(plan)) {
            for (llama_token id : g.second) {
                auto it = bias.find(id);
                if (it == bias.end() || !it->second.excluded) {
                    any_left = true;
                }
            }
        }
        if (!any_left) {
            warn.push_back("all watched tokens are excluded");
        }
        for (const auto & w : warn) {
            warnings.push_back(w);
        }
        json entries = json::array();
        for (const auto & [id, b] : bias) {
            entries.push_back(json {
                {"token_id", id},
                {"piece",    rv.valid(id) ? research_escape_piece(rv.pieces[id]) : std::string()},
                {"delta",    b.delta},
                {"excluded", b.excluded},
                {"rules",    b.rules},
            });
        }
        return entries;
    }

    static std::map<llama_token, research_bias> bias_from_entries(const json & entries) {
        std::map<llama_token, research_bias> out;
        for (const auto & e : entries) {
            research_bias b;
            b.delta    = e.value("delta", 0.0);
            b.excluded = e.value("excluded", false);
            out[e.at("token_id").get<int>()] = b;
        }
        return out;
    }

    //
    // batch runner
    //

    void worker_loop() {
        while (true) {
            std::string batch_id;
            {
                std::unique_lock<std::mutex> lock(mtx_q);
                cv_q.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) {
                    return;
                }
                batch_id = queue.front();
                queue.pop_front();
            }
            try {
                run_batch(batch_id);
            } catch (const std::exception & e) {
                SRV_ERR("research: batch %s failed: %s\n", batch_id.c_str(), e.what());
                store.exec("UPDATE batches SET status = 'failed', ended_at = ? WHERE id = ?", {research_now_iso(), batch_id});
            }
        }
    }

    void run_batch(const std::string & batch_id) {
        store.exec("UPDATE batches SET status = 'running' WHERE id = ?", {batch_id});
        json runs = store.query("SELECT id FROM runs WHERE batch_id = ? ORDER BY created_at, rowid", {batch_id});
        bool any_failed = false;
        for (const auto & r : runs) {
            if (stopping.load()) {
                return; // left as running; marked interrupted on next start
            }
            const std::string status = execute_run(r.at("id").get<std::string>());
            any_failed = any_failed || status == "failed";
        }
        store.exec("UPDATE batches SET status = ?, ended_at = ? WHERE id = ?", {any_failed ? "failed" : "completed", research_now_iso(), batch_id});
    }

    void writer_loop(std::shared_ptr<research_run> run) {
        size_t cursor = 0;
        while (true) {
            std::vector<research_event> batch;
            bool done = false;
            {
                std::unique_lock<std::mutex> lock(run->mtx);
                run->cv.wait(lock, [&] { return run->events.size() > cursor || run->finished; });
                batch.assign(run->events.begin() + cursor, run->events.end());
                cursor = run->events.size();
                done = run->finished;
            }
            if (!batch.empty()) {
                try {
                    store.begin();
                    for (const auto & e : batch) {
                        store.exec("INSERT OR REPLACE INTO run_events (run_id, event_seq, name, data_json) VALUES (?, ?, ?, ?)",
                                {run->id, (long long) e.seq, e.name, e.data.dump_safe()});
                        if (e.name == "research.token_accepted") {
                            store.exec("INSERT OR REPLACE INTO accepted_tokens (run_id, generation_index, token_id, piece, piece_b64, context_position) VALUES (?, ?, ?, ?, ?, ?)",
                                    {run->id, e.data.at("generation_index"), e.data.at("token_id"), e.data.at("piece"), e.data.at("piece_b64"), e.data.at("context_position")});
                        } else if (e.name == "research.observation") {
                            json obs = e.data;
                            obs.erase("run_id");
                            obs.erase("event_seq");
                            store.exec("INSERT OR REPLACE INTO observations (run_id, generation_index, obs_json) VALUES (?, ?, ?)",
                                    {run->id, e.data.at("generation_index"), obs.dump_safe()});
                        }
                    }
                    store.commit();
                } catch (const std::exception & ex) {
                    SRV_ERR("research: writer failed for run %s: %s\n", run->id.c_str(), ex.what());
                    try {
                        store.rollback();
                    } catch (...) {
                    }
                }
            }
            if (done) {
                std::lock_guard<std::mutex> lock(run->mtx);
                if (cursor == run->events.size()) {
                    return;
                }
            }
        }
    }

    // returns the final status
    std::string execute_run(const std::string & run_id) {
        auto run = get_live(run_id);
        json row = get_run_row(run_id);
        if (!run) {
            return row.at("status").get<std::string>();
        }
        json manifest = row.at("manifest");

        std::string status = "completed";
        std::string error;

        std::thread writer;
        if (run->cancel.load()) {
            status = "cancelled";
        } else {
            store.exec("UPDATE runs SET status = 'running', started_at = ? WHERE id = ?", {research_now_iso(), run_id});
            writer = std::thread([this, run] { writer_loop(run); });
            run->push_event("research.run_started", json {{"status", "running"}});

            try {
                const std::string prompt = manifest.at("prompt").get<std::string>();
                const bool chat = manifest.value("chat", true);
                const std::string rendered = chat ? ctx_server.research_render_chat(prompt) : prompt;
                manifest["rendered_prompt"] = rendered;
                store.exec("UPDATE runs SET manifest_json = ? WHERE id = ?", {manifest.dump_safe(), run_id});

                json data = json::object();
                const json sampling = manifest.value("sampling", json::object());
                for (const auto & [k, v] : sampling.items()) {
                    data[k] = v;
                }
                data["prompt"]       = rendered;
                data["n_predict"]    = manifest.at("n_predict");
                data["seed"]         = row.at("seed");
                data["cache_prompt"] = false;
                data["stream"]       = false;

                std::vector<llama_logit_bias> bias;
                for (const auto & [id, b] : run->bias) {
                    bias.push_back({id, b.excluded ? -INFINITY : (float) b.delta});
                }

                error = ctx_server.research_complete(data, bias, run, [this, run] {
                    return run->cancel.load() || stopping.load();
                });
                if (error == "cancelled") {
                    status = "cancelled";
                    error.clear();
                } else if (!error.empty()) {
                    status = "failed";
                }
            } catch (const std::exception & e) {
                status = "failed";
                error  = e.what();
            }
        }

        int32_t n_gen, captured, skipped, dropped;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(run->mtx_hook);
            run->closed = true;
            n_gen    = run->n_gen;
            captured = run->captured;
            skipped  = run->skipped;
            dropped  = run->dropped;
            text     = run->text;
        }
        run->push_event("research.run_finished", json {
            {"status", status}, {"n_tokens", n_gen}, {"captured", captured}, {"skipped", skipped}, {"dropped", dropped}, {"text", text},
            {"error", error.empty() ? json(nullptr) : json(error)},
        });
        if (!writer.joinable()) {
            writer = std::thread([this, run] { writer_loop(run); });
        }
        run->mark_finished();
        writer.join();

        store.exec("UPDATE runs SET status = ?, error = ?, n_tokens = ?, captured = ?, skipped = ?, dropped = ?, text = ?, ended_at = ? WHERE id = ?",
                {status, error.empty() ? json(nullptr) : json(error), n_gen, captured, skipped, dropped, text, research_now_iso(), run_id});
        {
            std::lock_guard<std::mutex> lock(mtx_live);
            live.erase(run_id);
        }
        SRV_INF("research: run %s %s, %d tokens, %d captured\n", run_id.c_str(), status.c_str(), n_gen, captured);
        return status;
    }

    //
    // handlers
    //

    json post_batch(const json & body) {
        const std::string qid = req_str(body, "question_id");
        const std::string did = req_str(body, "design_id");
        get_question(qid);
        json design = get_design(did);
        if (design.at("question_id").get<std::string>() != qid) {
            throw research_error(422, "DESIGN_QUESTION_MISMATCH", "design does not belong to question");
        }
        if (design.at("compiled_id").is_null()) {
            throw research_error(422, "NOT_COMPILED", "design must be compiled first");
        }
        const std::string compiled_id = design.at("compiled_id").get<std::string>();
        json plan = get_compiled(compiled_id);
        if (!plan.value("ok", false)) {
            throw research_error(422, "EMPTY_GROUP", "compiled plan is not ok (empty group)");
        }
        if (plan.value("tokenizer_fingerprint", std::string()) != rv.fingerprint) {
            throw research_error(409, "TOKENIZER_MISMATCH", "compiled plan was made for another tokenizer");
        }

        const std::string prompt = req_str(body, "prompt");
        const bool chat = body.value("chat", true);

        json seeds = body.value("seeds", json::array({0}));
        if (!seeds.is_array() || seeds.empty()) {
            throw research_error(400, "INVALID_FIELD", "seeds must be a non-empty array");
        }
        for (const auto & s : seeds) {
            if (!s.is_number_integer()) {
                throw research_error(400, "INVALID_FIELD", "seeds must be integers");
            }
        }
        const int n_predict = body.value("n_predict", 64);
        if (n_predict < 1 || n_predict > 8192) {
            throw research_error(400, "INVALID_FIELD", "n_predict must be in [1, 8192]");
        }

        json sampling = body.value("sampling", json::object());
        if (!sampling.is_object()) {
            throw research_error(400, "INVALID_FIELD", "sampling must be an object");
        }
        for (const char * k : {"grammar", "json_schema", "response_format", "grammar_lazy", "grammar_triggers"}) {
            if (sampling.contains(k)) {
                throw research_error(422, "GRAMMAR_NOT_ALLOWED", "grammar/json_schema are not allowed in research runs");
            }
        }
        for (const char * k : {"logit_bias", "prompt", "n_predict", "seed", "stream", "n_cmpl", "n"}) {
            if (sampling.contains(k)) {
                throw research_error(400, "INVALID_FIELD", std::string("sampling.") + k + " is not allowed");
            }
        }

        json collection = body.value("collection", json::object());
        const int every_n = collection.value("every_n", 5);
        const std::string mode = collection.value("mode", std::string("raw_watch"));
        const int top_k = collection.value("top_k", 0);
        if (every_n < 1) {
            throw research_error(400, "INVALID_FIELD", "collection.every_n must be >= 1");
        }
        if (mode != "raw_watch" && mode != "watch_normalized") {
            throw research_error(400, "INVALID_FIELD", "collection.mode must be raw_watch or watch_normalized");
        }
        if (top_k < 0 || top_k > 100) {
            throw research_error(400, "INVALID_FIELD", "collection.top_k must be in [0, 100]");
        }
        collection = json {{"every_n", every_n}, {"mode", mode}, {"top_k", top_k}};

        json arms = body.value("arms", json::array());
        if (!arms.is_array() || arms.empty()) {
            throw research_error(400, "INVALID_FIELD", "arms must be a non-empty array");
        }
        struct arm_info {
            std::string name;
            json intervention_id;
            json entries;
        };
        std::vector<arm_info> arm_list;
        for (const auto & a : arms) {
            arm_info ai;
            ai.name = req_str(a, "name");
            ai.intervention_id = opt_str(a, "intervention_id");
            ai.entries = json::array();
            if (!ai.intervention_id.is_null()) {
                json iv = get_intervention(ai.intervention_id.get<std::string>());
                if (iv.at("design_id").get<std::string>() != did) {
                    throw research_error(422, "INTERVENTION_DESIGN_MISMATCH", "intervention " + ai.intervention_id.get<std::string>() + " belongs to another design");
                }
                if (iv.at("compiled_id").get<std::string>() != compiled_id) {
                    throw research_error(409, "STALE_INTERVENTION", "intervention was made for an older compiled plan");
                }
                ai.entries = iv.at("effective");
            }
            arm_list.push_back(ai);
        }

        json effective_check = json::array();
        const std::string batch_id = research_new_id("bat_");
        const std::string now = research_now_iso();
        json run_ids = json::array();
        std::vector<std::shared_ptr<research_run>> new_runs;

        const research_plan rplan = research_plan::from_json(plan);

        store.begin();
        try {
            store.exec("INSERT INTO batches (id, question_id, design_id, request_json, status, created_at) VALUES (?, ?, ?, ?, 'queued', ?)",
                    {batch_id, qid, did, body.dump_safe(), now});
            for (const auto & ai : arm_list) {
                for (const auto & s : seeds) {
                    const std::string run_id = research_new_id("run_");
                    json eb = json::array();
                    for (const auto & e : ai.entries) {
                        eb.push_back(json {{"token_id", e.at("token_id")}, {"delta", e.at("delta")}, {"excluded", e.at("excluded")}});
                    }
                    const std::string hash_src = plan.value("fingerprint", std::string()) + "\n" + eb.dump() + "\n" + sampling.dump() + "\n" +
                        collection.dump() + "\n" + prompt + "\n" + (chat ? "chat" : "raw") + "\n" + std::to_string(n_predict) + "\n" + s.dump();
                    json manifest = {
                        {"prompt",                prompt},
                        {"chat",                  chat},
                        {"rendered_prompt",       nullptr},
                        {"n_predict",             n_predict},
                        {"sampling",              sampling},
                        {"collection",            collection},
                        {"design_id",             did},
                        {"compiled_id",           compiled_id},
                        {"compiled",              plan},
                        {"effective_bias",        eb},
                        {"model_id",              rv.model_id},
                        {"tokenizer_fingerprint", rv.fingerprint},
                        {"llama_build",           build_info},
                        {"stage_semantics",       RESEARCH_STAGE_SEMANTICS},
                        {"speculative",           false},
                        {"plan_hash",             research_sha256(hash_src)},
                    };
                    store.exec("INSERT INTO runs (id, batch_id, question_id, arm, intervention_id, seed, status, manifest_json, created_at) VALUES (?, ?, ?, ?, ?, ?, 'queued', ?, ?)",
                            {run_id, batch_id, qid, ai.name, ai.intervention_id, s, manifest.dump_safe(), now});

                    auto run = std::make_shared<research_run>();
                    run->id = run_id;
                    run->setup(rplan, bias_from_entries(ai.entries), every_n, mode == "watch_normalized", top_k, &rv);
                    new_runs.push_back(run);
                    run_ids.push_back(run_id);
                }
            }
            store.commit();
        } catch (...) {
            store.rollback();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(mtx_live);
            for (auto & r : new_runs) {
                live[r->id] = r;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mtx_q);
            queue.push_back(batch_id);
        }
        cv_q.notify_one();

        return json {{"batch_id", batch_id}, {"run_ids", run_ids}, {"status", "queued"}};
    }

    json post_intervention(const std::string & qid, const json & body) {
        get_question(qid);
        const std::string did = req_str(body, "design_id");
        json design = get_design(did);
        if (design.at("question_id").get<std::string>() != qid) {
            throw research_error(422, "DESIGN_QUESTION_MISMATCH", "design does not belong to question");
        }
        if (design.at("compiled_id").is_null()) {
            throw research_error(422, "NOT_COMPILED", "design must be compiled first");
        }
        const std::string compiled_id = design.at("compiled_id").get<std::string>();
        json plan = get_compiled(compiled_id);
        const json rules_in = body.value("rules", json::array());
        auto rules = parse_rules(rules_in);
        json warnings = json::array();
        json entries = effective_entries(rules, plan, warnings);

        json rules_out = json::array();
        for (const auto & r : rules) {
            rules_out.push_back(json {
                {"id", r.id}, {"target_group_id", r.target_group_id}, {"operation", r.operation},
                {"delta", r.operation == "exclude" ? json(nullptr) : json(r.delta)}, {"enabled", r.enabled},
            });
        }
        json rev = store.query_one("SELECT COALESCE(MAX(revision), 0) + 1 AS r FROM interventions WHERE question_id = ?", {qid});
        const std::string id = research_new_id("int_");
        store.exec("INSERT INTO interventions (id, question_id, design_id, compiled_id, revision, state, rules_json, effective_json, warnings_json, created_at) VALUES (?, ?, ?, ?, ?, 'draft', ?, ?, ?, ?)",
                {id, qid, did, compiled_id, rev.at("r"), rules_out.dump_safe(), entries.dump_safe(), warnings.dump_safe(), research_now_iso()});
        return get_intervention(id);
    }

    json post_compile(const std::string & did) {
        json design = get_design(did);
        if (design.at("state").get<std::string>() == "approved") {
            throw research_error(409, "IMMUTABLE", "approved designs are immutable");
        }
        json plan = research_compile(rv, design.at("spec"));
        const std::string cid = research_new_id("cmp_");
        plan["compiled_id"] = cid;
        store.begin();
        try {
            store.exec("INSERT INTO compiled_plans (id, design_id, fingerprint, tokenizer_fingerprint, plan_json, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                    {cid, did, plan.at("fingerprint"), plan.at("tokenizer_fingerprint"), plan.dump_safe(), research_now_iso()});
            store.exec("UPDATE designs SET compiled_id = ? WHERE id = ?", {cid, did});
            store.commit();
        } catch (...) {
            store.rollback();
            throw;
        }
        return plan;
    }

    json design_full(const std::string & did) {
        json d = get_design(did);
        if (!d.at("compiled_id").is_null()) {
            d["compiled"] = get_compiled(d.at("compiled_id").get<std::string>());
        }
        return d;
    }

    json get_observations(const std::string & run_id, long long from, long long to) {
        json row = get_run_row(run_id);
        json plan = row.at("manifest").at("compiled");
        json groups = json::array();
        for (const auto & g : plan.at("groups")) {
            groups.push_back(json {{"id", g.at("id")}, {"name", g.value("name", g.at("id").get<std::string>())}});
        }
        json obs = json::array();
        for (const auto & r : store.query("SELECT obs_json FROM observations WHERE run_id = ? AND generation_index >= ? AND generation_index <= ? ORDER BY generation_index",
                    {run_id, from, to})) {
            obs.push_back(r.at("obs"));
        }
        return json {{"run_id", run_id}, {"watch_ids", plan.at("watch_ids")}, {"groups", groups}, {"observations", obs}};
    }

    json get_tokens(const std::string & run_id) {
        get_run_row(run_id);
        return json {{"run_id", run_id}, {"tokens", store.query(
                "SELECT generation_index, token_id, piece, piece_b64, context_position FROM accepted_tokens WHERE run_id = ? ORDER BY generation_index", {run_id})}};
    }

    json counterfactual(const json & body) {
        const std::string run_id = req_str(body, "run_id");
        const std::string iid    = req_str(body, "intervention_id");
        if (!body.contains("generation_index") || !body.at("generation_index").is_number_integer()) {
            throw research_error(400, "INVALID_FIELD", "generation_index is required");
        }
        const long long gi = body.at("generation_index").get<long long>();
        json row  = get_run_row(run_id);
        json plan = row.at("manifest").at("compiled");
        json o = store.query_one("SELECT obs_json FROM observations WHERE run_id = ? AND generation_index = ?", {run_id, gi});
        if (o.is_null()) {
            throw research_error(404, "NOT_CAPTURED", "no observation at this generation_index");
        }
        json obs = o.at("obs");
        if (!obs.contains("raw")) {
            throw research_error(422, "NOT_CAPTURED", "observation has no raw logits");
        }
        json iv = get_intervention(iid);

        research_plan rp = research_plan::from_json(plan);
        json warnings = json::array();
        if (iv.at("compiled_id") != row.at("manifest").at("compiled_id")) {
            warnings.push_back("intervention was compiled against another plan; tokens outside the watch set are ignored");
        }
        auto bias = bias_from_entries(iv.at("effective"));
        std::vector<float> raw = obs.at("raw").get<std::vector<float>>();
        std::vector<float> wb(raw.size(), 0.0f);
        std::vector<uint8_t> wx(raw.size(), 0);
        std::set<llama_token> watch(rp.watch_ids.begin(), rp.watch_ids.end());
        for (size_t i = 0; i < rp.watch_ids.size(); i++) {
            auto it = bias.find(rp.watch_ids[i]);
            if (it != bias.end()) {
                wb[i] = it->second.excluded ? -INFINITY : (float) it->second.delta;
                wx[i] = it->second.excluded;
            }
        }
        double raw_log_z = NAN;
        double adj_log_z = NAN;
        if (obs.contains("raw_log_z") && obs.at("raw_log_z").is_number()) {
            raw_log_z = obs.at("raw_log_z").get<double>();
            std::vector<std::pair<float, research_bias>> changed;
            bool complete = true;
            for (const auto & [id, b] : bias) {
                if (!watch.count(id)) {
                    complete = false;
                    continue;
                }
                const size_t wi = std::lower_bound(rp.watch_ids.begin(), rp.watch_ids.end(), id) - rp.watch_ids.begin();
                changed.emplace_back(raw[wi], b);
            }
            if (!complete || !research_adjusted_log_z(raw_log_z, changed, adj_log_z)) {
                adj_log_z = NAN;
            }
        }
        std::vector<float> zero_b(raw.size(), 0.0f);
        std::vector<uint8_t> zero_x(raw.size(), 0);
        json adjusted = json::array();
        for (size_t i = 0; i < raw.size(); i++) {
            adjusted.push_back(research_num(wx[i] ? -INFINITY : raw[i] + wb[i]));
        }
        return json {
            {"run_id", run_id}, {"generation_index", gi}, {"intervention_id", iid},
            {"watch_ids", rp.watch_ids},
            {"raw",      {{"logits", raw}, {"log_z", research_num(raw_log_z)}, {"groups", research_run::group_metrics(rp, raw, zero_b, zero_x, raw_log_z, raw_log_z)}}},
            {"adjusted", {{"logits", adjusted}, {"log_z", research_num(adj_log_z)}, {"groups", research_run::group_metrics(rp, raw, wb, wx, raw_log_z, adj_log_z)}}},
            {"warnings", warnings},
        };
    }

    server_http_res_ptr export_run(const std::string & run_id, const std::string & format) {
        json manifest = run_manifest(run_id);
        json tokens   = get_tokens(run_id).at("tokens");
        json obs      = get_observations(run_id, 0, INT32_MAX).at("observations");
        auto res = std::make_unique<server_http_res>();
        if (format == "csv") {
            std::ostringstream ss;
            ss << "run_id,generation_index,stage,token_id,logit\n";
            json watch_ids = manifest.at("compiled").at("watch_ids");
            for (const auto & o : obs) {
                if (!o.contains("raw")) {
                    continue;
                }
                const long long gi = o.at("generation_index").get<long long>();
                for (const char * stage : {"raw", "adjusted"}) {
                    const json & arr = o.at(stage);
                    for (size_t i = 0; i < arr.size(); i++) {
                        ss << run_id << "," << gi << "," << stage << "," << watch_ids.at(i).get<int>() << ",";
                        if (arr.at(i).is_null()) {
                            ss << "-inf";
                        } else {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%.9g", arr.at(i).get<double>());
                            ss << buf;
                        }
                        ss << "\n";
                    }
                }
            }
            res->content_type = "text/csv; charset=utf-8";
            res->data = ss.str();
        } else if (format == "jsonl" || format.empty()) {
            std::string out;
            json m = manifest;
            m["type"] = "manifest";
            out += safe_json_to_str(m) + "\n";
            for (const auto & t : tokens) {
                json tt = t;
                tt["type"] = "token";
                out += safe_json_to_str(tt) + "\n";
            }
            for (const auto & o : obs) {
                json oo = o;
                oo["type"] = "observation";
                out += safe_json_to_str(oo) + "\n";
            }
            res->content_type = "application/x-ndjson; charset=utf-8";
            res->data = out;
        } else {
            throw research_error(400, "INVALID_PARAM", "format must be jsonl or csv");
        }
        res->headers["Content-Disposition"] = "attachment; filename=\"" + run_id + "." + (format == "csv" ? "csv" : "jsonl") + "\"";
        return res;
    }

    static std::string sse_event(const research_event & e) {
        return "id: " + std::to_string(e.seq) + "\nevent: " + e.name + "\ndata: " + safe_json_to_str(e.data) + "\n\n";
    }

    server_http_res_ptr run_events(const server_http_req & req, const std::string & run_id) {
        long long after = param_int(req, "after", -1);
        if (after < 0) {
            const auto it = req.headers.find("Last-Event-ID");
            after = 0;
            if (it != req.headers.end()) {
                try {
                    after = std::stoll(it->second);
                } catch (...) {
                }
            }
        }
        auto res = std::make_unique<server_http_res>();
        res->content_type = "text/event-stream";
        res->headers["Cache-Control"] = "no-cache";

        auto run = get_live(run_id);
        if (!run) {
            json row = get_run_row(run_id);
            std::string out;
            bool has_finish = false;
            for (const auto & e : store.query("SELECT event_seq, name, data_json FROM run_events WHERE run_id = ? AND event_seq > ? ORDER BY event_seq", {run_id, after})) {
                research_event ev{e.at("event_seq").get<long long>(), e.at("name").get<std::string>(), e.at("data")};
                has_finish = has_finish || ev.name == "research.run_finished";
                out += sse_event(ev);
            }
            if (!has_finish) {
                // e.g. interrupted runs: report the terminal state once
                json last = store.query_one("SELECT COALESCE(MAX(event_seq), 0) AS s FROM run_events WHERE run_id = ?", {run_id});
                const long long seq = last.at("s").get<long long>() + 1;
                if (seq > after) {
                    research_event ev{seq, "research.run_finished", json {
                        {"run_id", run_id}, {"event_seq", seq}, {"status", row.at("status")}, {"n_tokens", row.at("n_tokens")},
                        {"captured", row.at("captured")}, {"skipped", row.at("skipped")}, {"dropped", row.at("dropped")}, {"text", row.at("text")},
                    }};
                    out += sse_event(ev);
                }
            }
            res->data = out;
            return res;
        }

        auto cursor = std::make_shared<size_t>((size_t) std::max(0LL, after));
        auto stop = stopping_flag();
        res->next = [run, cursor, stop](std::string & output) -> bool {
            std::unique_lock<std::mutex> lock(run->mtx);
            run->cv.wait_for(lock, std::chrono::seconds(10), [&] { return run->events.size() > *cursor || run->finished || stop->load(); });
            if (run->events.size() > *cursor) {
                for (size_t i = *cursor; i < run->events.size(); i++) {
                    output += sse_event(run->events[i]);
                }
                *cursor = run->events.size();
                return !run->finished;
            }
            if (run->finished || stop->load()) {
                return false;
            }
            output = ": ping\n\n";
            return true;
        };
        return res;
    }

    std::shared_ptr<std::atomic<bool>> stop_shared = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> stopping_flag() { return stop_shared; }

    json vocab_search(const server_http_req & req) {
        const std::string q    = req.get_param("query");
        const std::string mode = req.get_param("mode", "substring");
        const long long limit  = std::clamp(param_int(req, "limit", 50), 1LL, 200LL);
        const long long offset = std::max(0LL, param_int(req, "offset", 0));
        const bool inc_special = param_int(req, "include_special", 0) != 0;
        if (mode != "substring" && mode != "exact" && mode != "prefix") {
            throw research_error(400, "INVALID_PARAM", "mode must be substring, exact or prefix");
        }
        auto lower = [](std::string s) {
            for (auto & c : s) {
                c = (char) std::tolower((unsigned char) c);
            }
            return s;
        };
        const std::string ql = lower(q);
        json tokens = json::array();
        long long n_match = 0;
        for (llama_token id = 0; id < rv.n_vocab; id++) {
            if (!inc_special && rv.special[id]) {
                continue;
            }
            const std::string & p = rv.pieces[id];
            bool m;
            if (mode == "exact") {
                m = p == q;
            } else if (mode == "prefix") {
                m = lower(p).rfind(ql, 0) == 0;
            } else {
                m = q.empty() || lower(p).find(ql) != std::string::npos;
            }
            if (!m) {
                continue;
            }
            if (n_match >= offset && (long long) tokens.size() < limit) {
                tokens.push_back(rv.token_json(id));
            }
            n_match++;
        }
        return json {{"n_vocab", rv.n_vocab}, {"total", n_match}, {"offset", offset}, {"tokens", tokens}};
    }
};

research_api::research_api(const common_params & params, server_context & ctx_server) : pimpl(std::make_unique<impl>(params, ctx_server)) {}

research_api::~research_api() {
    stop();
}

void research_api::stop() {
    auto & p = *pimpl;
    p.stopping.store(true);
    p.stop_shared->store(true);
    {
        std::lock_guard<std::mutex> lock(p.mtx_live);
        for (auto & [id, run] : p.live) {
            run->cancel.store(true);
            run->cv.notify_all();
        }
    }
    p.cv_q.notify_all();
    if (p.worker.joinable()) {
        p.worker.join();
    }
}

bool research_api::init() {
    auto & p = *pimpl;
    if (p.params.research_db.empty()) {
        return true;
    }
    std::string err;
    if (!research_math_selftest(err)) {
        SRV_ERR("research: math self-test failed: %s\n", err.c_str());
        return false;
    }
    try {
        p.store.open(p.params.research_db);
    } catch (const std::exception & e) {
        SRV_ERR("%s\n", e.what());
        return false;
    }
    const auto meta = p.ctx_server.get_meta();
    p.build_info = meta.build_info;
    p.rv.init(llama_get_model(p.ctx_server.get_llama_context()), meta.model_name);
    p.enabled = true;
    p.worker = std::thread([&p] { p.worker_loop(); });
    SRV_INF("research: enabled, db = %s, n_vocab = %d, tokenizer = %s\n", p.params.research_db.c_str(), p.rv.n_vocab, p.rv.fingerprint.c_str());
    return true;
}

std::vector<research_route> research_api::routes() {
    impl * p = pimpl.get();
    using fn_t = std::function<server_http_res_ptr(const server_http_req &)>;

    auto wrap = [p](fn_t fn, bool always = false) -> server_http_context::handler_t {
        return [p, fn = std::move(fn), always](const server_http_req & req) -> server_http_res_ptr {
            try {
                if (!p->enabled && !always) {
                    return make_err(404, "RESEARCH_DISABLED", "research module is disabled, start the server with --research-db PATH");
                }
                return fn(req);
            } catch (const research_error & e) {
                return make_err(e.status, e.code, e.what());
            } catch (const common_json_error & e) {
                return make_err(400, "INVALID_REQUEST", e.what());
            } catch (const std::invalid_argument & e) {
                return make_err(400, "INVALID_REQUEST", e.what());
            } catch (const std::exception & e) {
                SRV_ERR("research: %s\n", e.what());
                return make_err(500, "INTERNAL", e.what());
            }
        };
    };
    auto id_of = [](const server_http_req & req) { return req.get_param("id"); };

    std::vector<research_route> r;
    auto add = [&](const char * method, const std::string & path, fn_t fn, bool always = false) {
        r.push_back({method, RESEARCH_BASE + path, wrap(std::move(fn), always)});
    };

    add("GET", "/status", [p](const server_http_req &) {
        if (!p->enabled) {
            return make_res(json {{"enabled", false}, {"schema_version", RESEARCH_SCHEMA_VERSION}});
        }
        return make_res(json {
            {"enabled", true}, {"model_id", p->rv.model_id}, {"n_vocab", p->rv.n_vocab},
            {"tokenizer_fingerprint", p->rv.fingerprint}, {"schema_version", RESEARCH_SCHEMA_VERSION},
            {"llama_build", p->build_info},
        });
    }, true);

    // projects
    add("GET", "/projects", [p](const server_http_req &) {
        return make_res(json {{"projects", p->store.query("SELECT * FROM projects ORDER BY created_at DESC")}});
    });
    add("POST", "/projects", [p](const server_http_req & req) {
        json body = parse_body(req);
        const std::string id = research_new_id("prj_");
        const std::string now = research_now_iso();
        json desc = opt_str(body, "description");
        p->store.exec("INSERT INTO projects (id, title, description, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
                {id, req_str(body, "title"), desc.is_null() ? json("") : desc, now, now});
        return make_res(p->get_project(id), 201);
    });
    add("GET", "/projects/:id", [p, id_of](const server_http_req & req) {
        json prj = p->get_project(id_of(req));
        prj["questions"] = p->store.query("SELECT * FROM questions WHERE project_id = ? ORDER BY created_at", {id_of(req)});
        return make_res(prj);
    });
    add("POST", "/projects/:id/questions", [p, id_of](const server_http_req & req) {
        json body = parse_body(req);
        p->get_project(id_of(req));
        const std::string id = research_new_id("q_");
        const std::string now = research_now_iso();
        p->store.exec("INSERT INTO questions (id, project_id, question_text, hypothesis, notes, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?, 'draft', ?, ?)",
                {id, id_of(req), req_str(body, "question_text"), opt_str(body, "hypothesis"), opt_str(body, "notes"), now, now});
        return make_res(p->get_question(id), 201);
    });

    // questions
    add("GET", "/questions/:id", [p, id_of](const server_http_req & req) {
        return make_res(p->get_question(id_of(req)));
    });
    add("PATCH", "/questions/:id", [p, id_of](const server_http_req & req) {
        json body = parse_body(req);
        const std::string id = id_of(req);
        p->get_question(id);
        for (const char * k : {"question_text", "hypothesis", "notes", "status"}) {
            if (!body.contains(k)) {
                continue;
            }
            json v = opt_str(body, k);
            if (std::string(k) == "status" && (v.is_null() || (v.get<std::string>() != "draft" && v.get<std::string>() != "active" && v.get<std::string>() != "archived"))) {
                throw research_error(400, "INVALID_FIELD", "status must be draft, active or archived");
            }
            if (std::string(k) == "question_text" && (v.is_null() || v.get<std::string>().empty())) {
                throw research_error(400, "INVALID_FIELD", "question_text must not be empty");
            }
            p->store.exec(std::string("UPDATE questions SET ") + k + " = ?, updated_at = ? WHERE id = ?", {v, research_now_iso(), id});
        }
        return make_res(p->get_question(id));
    });

    // designs
    add("GET", "/questions/:id/designs", [p, id_of](const server_http_req & req) {
        p->get_question(id_of(req));
        return make_res(json {{"designs", p->store.query("SELECT * FROM designs WHERE question_id = ? ORDER BY revision", {id_of(req)})}});
    });
    add("POST", "/questions/:id/designs", [p, id_of](const server_http_req & req) {
        json body = parse_body(req);
        const std::string qid = id_of(req);
        p->get_question(qid);
        if (!body.contains("spec") || !body.at("spec").is_object() || !body.at("spec").contains("groups") || !body.at("spec").at("groups").is_array()) {
            throw research_error(400, "INVALID_SPEC", "spec.groups must be an array");
        }
        const std::string id = research_new_id("dsg_");
        p->store.begin();
        try {
            json rev = p->store.query_one("SELECT COALESCE(MAX(revision), 0) + 1 AS r FROM designs WHERE question_id = ?", {qid});
            p->store.exec("INSERT INTO designs (id, question_id, revision, state, spec_json, created_at) VALUES (?, ?, ?, 'draft', ?, ?)",
                    {id, qid, rev.at("r"), body.at("spec").dump_safe(), research_now_iso()});
            p->store.commit();
        } catch (...) {
            p->store.rollback();
            throw;
        }
        return make_res(p->get_design(id), 201);
    });
    add("GET", "/designs/:id", [p, id_of](const server_http_req & req) {
        return make_res(p->design_full(id_of(req)));
    });
    add("POST", "/compile-preview", [p](const server_http_req & req) {
        json body = parse_body(req);
        if (!body.contains("spec")) {
            throw research_error(400, "INVALID_SPEC", "spec is required");
        }
        return make_res(research_compile(p->rv, body.at("spec")));
    });
    add("POST", "/designs/:id/compile", [p, id_of](const server_http_req & req) {
        return make_res(p->post_compile(id_of(req)));
    });
    add("POST", "/designs/:id/approve", [p, id_of](const server_http_req & req) {
        const std::string id = id_of(req);
        json d = p->get_design(id);
        if (d.at("state").get<std::string>() == "approved") {
            return make_res(p->design_full(id));
        }
        if (d.at("compiled_id").is_null()) {
            throw research_error(422, "NOT_COMPILED", "design must be compiled first");
        }
        json plan = p->get_compiled(d.at("compiled_id").get<std::string>());
        if (!plan.value("ok", false)) {
            throw research_error(422, "EMPTY_GROUP", "compiled plan has an empty group");
        }
        p->store.exec("UPDATE designs SET state = 'approved', approved_at = ?, content_hash = ? WHERE id = ?", {research_now_iso(), plan.at("fingerprint"), id});
        return make_res(p->design_full(id));
    });

    // interventions
    add("GET", "/questions/:id/interventions", [p, id_of](const server_http_req & req) {
        p->get_question(id_of(req));
        return make_res(json {{"interventions", p->store.query("SELECT * FROM interventions WHERE question_id = ? ORDER BY revision", {id_of(req)})}});
    });
    add("POST", "/questions/:id/interventions", [p, id_of](const server_http_req & req) {
        return make_res(p->post_intervention(id_of(req), parse_body(req)), 201);
    });
    add("GET", "/interventions/:id", [p, id_of](const server_http_req & req) {
        return make_res(p->get_intervention(id_of(req)));
    });
    add("POST", "/interventions/:id/approve", [p, id_of](const server_http_req & req) {
        const std::string id = id_of(req);
        json iv = p->get_intervention(id);
        if (iv.at("state").get<std::string>() != "approved") {
            p->store.exec("UPDATE interventions SET state = 'approved', approved_at = ? WHERE id = ?", {research_now_iso(), id});
        }
        return make_res(p->get_intervention(id));
    });
    add("GET", "/interventions/:id/effective-bias", [p, id_of](const server_http_req & req) {
        json iv = p->get_intervention(id_of(req));
        return make_res(json {{"entries", iv.at("effective")}, {"warnings", iv.at("warnings")}});
    });

    // vocab
    add("GET", "/vocab", [p](const server_http_req & req) {
        return make_res(p->vocab_search(req));
    });
    add("GET", "/tokens/:id", [p, id_of](const server_http_req & req) {
        long long id = -1;
        try {
            id = std::stoll(id_of(req));
        } catch (...) {
        }
        if (!p->rv.valid(id)) {
            throw research_error(404, "NOT_FOUND", "token id out of range");
        }
        return make_res(p->rv.token_json((llama_token) id));
    });
    add("POST", "/tokenize-preview", [p](const server_http_req & req) {
        json body = parse_body(req);
        const std::string text = body.value("text", std::string());
        const auto toks = common_tokenize(p->rv.vocab, text, body.value("add_special", false), body.value("parse_special", false));
        json out = json::array();
        for (llama_token id : toks) {
            out.push_back(p->rv.token_json(id));
        }
        return make_res(json {{"tokens", out}});
    });

    // proposals
    add("POST", "/proposals", [p](const server_http_req & req) {
        json body = parse_body(req);
        const std::string qid = req_str(body, "question_id");
        p->get_question(qid);
        const std::string id = research_new_id("prop_");
        json proposal = body.contains("proposal") ? body.at("proposal") : json(nullptr);
        p->store.exec("INSERT INTO proposals (id, question_id, model, prompt, raw_output, proposal_json, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                {id, qid, opt_str(body, "model"), opt_str(body, "prompt"), opt_str(body, "raw_output"), proposal.dump_safe(), research_now_iso()});
        return make_res(p->store.query_one("SELECT * FROM proposals WHERE id = ?", {id}), 201);
    });
    add("GET", "/questions/:id/proposals", [p, id_of](const server_http_req & req) {
        p->get_question(id_of(req));
        return make_res(json {{"proposals", p->store.query("SELECT * FROM proposals WHERE question_id = ? ORDER BY created_at", {id_of(req)})}});
    });

    // batches and runs
    add("POST", "/batches", [p](const server_http_req & req) {
        return make_res(p->post_batch(parse_body(req)), 202);
    });
    add("GET", "/batches/:id", [p, id_of](const server_http_req & req) {
        json b = p->must(p->store.query_one("SELECT * FROM batches WHERE id = ?", {id_of(req)}), "batch", id_of(req));
        json runs = json::array();
        for (const auto & r : p->store.query("SELECT * FROM runs WHERE batch_id = ? ORDER BY created_at, rowid", {id_of(req)})) {
            runs.push_back(p->run_summary(r));
        }
        b["runs"] = runs;
        return make_res(b);
    });
    add("GET", "/runs", [p](const server_http_req & req) {
        const std::string qid = req.get_param("question_id");
        const std::string bid = req.get_param("batch_id");
        json runs = json::array();
        for (const auto & r : p->store.query("SELECT * FROM runs WHERE (? = '' OR question_id = ?) AND (? = '' OR batch_id = ?) ORDER BY created_at DESC, rowid", {qid, qid, bid, bid})) {
            runs.push_back(p->run_summary(r));
        }
        return make_res(json {{"runs", runs}});
    });
    add("GET", "/runs/:id", [p, id_of](const server_http_req & req) {
        return make_res(p->run_manifest(id_of(req)));
    });
    add("GET", "/runs/:id/tokens", [p, id_of](const server_http_req & req) {
        return make_res(p->get_tokens(id_of(req)));
    });
    add("GET", "/runs/:id/observations", [p, id_of](const server_http_req & req) {
        return make_res(p->get_observations(id_of(req), param_int(req, "from", 0), param_int(req, "to", INT32_MAX)));
    });
    add("GET", "/runs/:id/events", [p, id_of](const server_http_req & req) {
        return p->run_events(req, id_of(req));
    });
    add("POST", "/runs/:id/cancel", [p, id_of](const server_http_req & req) {
        const std::string id = id_of(req);
        p->get_run_row(id);
        if (auto run = p->get_live(id)) {
            run->cancel.store(true);
        }
        return make_res(p->run_summary(p->get_run_row(id)));
    });
    add("GET", "/runs/:id/export", [p, id_of](const server_http_req & req) {
        return p->export_run(id_of(req), req.get_param("format", "jsonl"));
    });
    add("POST", "/counterfactual", [p](const server_http_req & req) {
        return make_res(p->counterfactual(parse_body(req)));
    });

    return r;
}
