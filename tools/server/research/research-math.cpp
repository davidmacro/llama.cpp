#include "research-math.h"

#include <algorithm>
#include <cmath>
#include <set>

double research_logsumexp(const float * x, size_t n) {
    double m = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        m = std::max(m, (double) x[i]);
    }
    if (!std::isfinite(m)) {
        return m; // empty, all -inf, or +inf
    }
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        s += std::exp((double) x[i] - m);
    }
    return m + std::log(s);
}

double research_logsumexp(const std::vector<double> & x) {
    double m = -INFINITY;
    for (double v : x) {
        m = std::max(m, v);
    }
    if (!std::isfinite(m)) {
        return m;
    }
    double s = 0.0;
    for (double v : x) {
        s += std::exp(v - m);
    }
    return m + std::log(s);
}

bool research_effective_bias(
        const std::vector<research_rule> & rules,
        const std::map<std::string, std::vector<llama_token>> & groups,
        std::map<llama_token, research_bias> & out,
        std::vector<std::string> & warnings,
        std::string & err) {
    out.clear();
    std::set<std::string> seen_ids;
    for (const auto & r : rules) {
        if (!seen_ids.insert(r.id).second) {
            err = "duplicate rule id: " + r.id;
            return false;
        }
        auto it = groups.find(r.target_group_id);
        if (it == groups.end()) {
            err = "rule " + r.id + ": unknown group " + r.target_group_id;
            return false;
        }
        if (r.operation != "add_logit_bias" && r.operation != "exclude") {
            err = "rule " + r.id + ": unknown operation " + r.operation;
            return false;
        }
        if (r.operation == "add_logit_bias") {
            if (!std::isfinite(r.delta)) {
                err = "rule " + r.id + ": delta must be finite";
                return false;
            }
            if (std::fabs(r.delta) > RESEARCH_MAX_ABS_DELTA) {
                err = "rule " + r.id + ": |delta| must be <= 100";
                return false;
            }
        }
        if (!r.enabled) {
            continue;
        }
        if (it->second.empty()) {
            warnings.push_back("rule " + r.id + ": group " + r.target_group_id + " has no members");
        }
        for (llama_token id : it->second) {
            auto & b = out[id];
            b.rules.push_back(r.id);
            if (r.operation == "exclude") {
                b.excluded = true;
            } else {
                if ((b.delta > 0 && r.delta < 0) || (b.delta < 0 && r.delta > 0)) {
                    warnings.push_back("token " + std::to_string(id) + ": conflicting signs in rules");
                }
                b.delta += r.delta;
            }
        }
    }
    for (const auto & [id, b] : out) {
        if (b.rules.size() > 1) {
            warnings.push_back("token " + std::to_string(id) + " is targeted by " + std::to_string(b.rules.size()) + " rules (overlap)");
        }
    }
    return true;
}

bool research_adjusted_log_z(double raw_log_z, const std::vector<std::pair<float, research_bias>> & changed, double & out) {
    if (!std::isfinite(raw_log_z)) {
        return false;
    }
    double s = 0.0;
    for (const auto & [z, b] : changed) {
        const double p = std::exp((double) z - raw_log_z);
        if (b.excluded) {
            s -= p;
        } else {
            s += p * std::expm1(b.delta);
        }
    }
    const double f = 1.0 + s;
    if (!(f > 1e-12)) {
        return false;
    }
    out = raw_log_z + std::log(f);
    return true;
}

static bool near(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

bool research_math_selftest(std::string & err) {
    std::vector<std::string> warn;
    // fixture 1: z = [ln 8, ln 1, ln 1], bias -ln 4 on token 0 -> group mass 0.8 -> 0.5
    {
        const float z[3] = { (float) std::log(8.0), 0.0f, 0.0f };
        const double lz = research_logsumexp(z, 3);
        if (!near(std::exp(z[0] - lz), 0.8)) {
            err = "fixture 1: raw mass";
            return false;
        }
        std::map<llama_token, research_bias> bias;
        if (!research_effective_bias({{"r1", "a", "add_logit_bias", -std::log(4.0), true}}, {{"a", {0}}}, bias, warn, err)) {
            return false;
        }
        double alz = 0;
        if (!research_adjusted_log_z(lz, {{z[0], bias[0]}}, alz) || !near(std::exp(z[0] + bias[0].delta - alz), 0.5)) {
            err = "fixture 1: adjusted mass";
            return false;
        }
    }
    // fixture 2: H_A = {0,1}, H_B = {1,2}, biases -2 and +1 -> token 1 gets -1
    {
        std::map<llama_token, research_bias> bias;
        if (!research_effective_bias({{"ra", "a", "add_logit_bias", -2.0, true}, {"rb", "b", "add_logit_bias", 1.0, true}},
                {{"a", {0, 1}}, {"b", {1, 2}}}, bias, warn, err)) {
            return false;
        }
        if (bias.size() != 3 || !near(bias[0].delta, -2.0) || !near(bias[1].delta, -1.0) || !near(bias[2].delta, 1.0)) {
            err = "fixture 2: overlap sum";
            return false;
        }
    }
    // fixture 3: exclude token 0 -> others renormalize; exclude all -> error
    {
        const float z[3] = { (float) std::log(8.0), 0.0f, 0.0f };
        const double lz = research_logsumexp(z, 3);
        std::map<llama_token, research_bias> bias;
        if (!research_effective_bias({{"rx", "a", "exclude", 0.0, true}, {"rd", "a", "add_logit_bias", 5.0, true}}, {{"a", {0}}}, bias, warn, err)) {
            return false;
        }
        double alz = 0;
        if (!research_adjusted_log_z(lz, {{z[0], bias[0]}}, alz) || !near(std::exp(z[1] - alz), 0.5)) {
            err = "fixture 3: exclusion renormalization";
            return false;
        }
        research_bias ex;
        ex.excluded = true;
        if (research_adjusted_log_z(lz, {{z[0], ex}, {z[1], ex}, {z[2], ex}}, alz)) {
            err = "fixture 3: mask-all must fail";
            return false;
        }
        std::map<llama_token, research_bias> bad;
        if (research_effective_bias({{"rn", "a", "add_logit_bias", NAN, true}}, {{"a", {0}}}, bad, warn, err) ||
            research_effective_bias({{"rn", "a", "add_logit_bias", 101.0, true}}, {{"a", {0}}}, bad, warn, err)) {
            err = "fixture 3: invalid delta accepted";
            return false;
        }
    }
    err.clear();
    return true;
}
