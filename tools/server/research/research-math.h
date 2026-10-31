#pragma once

#include "llama.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// research math: logsumexp, group masses, effective bias, adjusted log Z

#define RESEARCH_MAX_ABS_DELTA 100.0

struct research_rule {
    std::string id;
    std::string target_group_id;
    std::string operation; // add_logit_bias | exclude
    double      delta   = 0.0;
    bool        enabled = true;
};

struct research_bias {
    double delta    = 0.0;
    bool   excluded = false;
    std::vector<std::string> rules; // ids of the rules that touch this token
};

// stable log(sum(exp(x))); -inf when empty or all -inf
double research_logsumexp(const float * x, size_t n);
double research_logsumexp(const std::vector<double> & x);

// effective bias per token: additive over enabled add_logit_bias rules, exclude wins
// returns false and sets err on invalid rules (unknown group, NaN/inf delta, |delta| > max)
bool research_effective_bias(
        const std::vector<research_rule> & rules,
        const std::map<std::string, std::vector<llama_token>> & groups,
        std::map<llama_token, research_bias> & out,
        std::vector<std::string> & warnings,
        std::string & err);

// adjusted log Z = raw_log_z + log(1 + sum P_raw(i) (exp(d_i) - 1)) over changed tokens
// changed = (raw logit, bias); returns false when no probability mass is left (all excluded)
bool research_adjusted_log_z(double raw_log_z, const std::vector<std::pair<float, research_bias>> & changed, double & out);

// checks design doc 13 fixtures 1-3; returns false and sets err on failure
bool research_math_selftest(std::string & err);
