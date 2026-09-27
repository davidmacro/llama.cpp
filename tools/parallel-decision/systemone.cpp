#include "systemone.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace llama_decision {
namespace systemone {

namespace {

// ---------------------------------------------------------------- validation

const char * const expected_tags = "'noul', 'choice', 'score'";

common_json at_key(const common_json & loc, const std::string & key) {
    common_json l = loc;
    l.push_back(key);
    return l;
}

common_json at_idx(const common_json & loc, size_t idx) {
    common_json l = loc;
    l.push_back((int64_t) idx);
    return l;
}

struct errors {
    common_json list = common_json::array();

    void add(const common_json & loc, const std::string & type, const std::string & msg, const common_json & input,
             const common_json & ctx = common_json()) {
        common_json e = common_json::object();
        e["type"]  = type;
        e["loc"]   = loc;
        e["msg"]   = msg;
        e["input"] = input;
        if (!ctx.is_null()) {
            e["ctx"] = ctx;
        }
        list.push_back(e);
    }

    void missing(const common_json & loc, const std::string & key, const common_json & parent) {
        add(at_key(loc, key), "missing", "Field required", parent);
    }
};

// str | dict[str, Any] | list[Any] (| None): a failed union reports every member
void check_any(errors & errs, const common_json & loc, const common_json & v, bool allow_null) {
    if (v.is_string() || v.is_object() || v.is_array() || (allow_null && v.is_null())) {
        return;
    }
    errs.add(at_key(loc, "str"),            "string_type", "Input should be a valid string",     v);
    errs.add(at_key(loc, "dict[str,any]"),  "dict_type",   "Input should be a valid dictionary", v);
    errs.add(at_key(loc, "list[any]"),      "list_type",   "Input should be a valid list",       v);
}

common_json too_short(const char * field_type, size_t n) {
    return common_json{ { "field_type", field_type }, { "min_length", 1 }, { "actual_length", (int64_t) n } };
}

void check_question(errors & errs, const common_json & loc, const common_json & q) {
    if (!q.is_object()) {
        errs.add(loc, "model_attributes_type", "Input should be a valid dictionary or object to extract fields from", q);
        return;
    }
    if (!q.contains("type")) {
        errs.add(loc, "union_tag_not_found", "Unable to extract tag using discriminator 'type'", q,
                 common_json{ { "discriminator", "'type'" } });
        return;
    }
    const common_json & tag = q.at("type");
    const std::string   t   = tag.is_string() ? tag.get<std::string>() : tag.dump();
    if (!tag.is_string() || (t != "noul" && t != "choice" && t != "score")) {
        errs.add(loc, "union_tag_invalid",
                 "Input tag '" + t + "' found using 'type' does not match any of the expected tags: " + expected_tags, q,
                 common_json{ { "discriminator", "'type'" }, { "tag", t }, { "expected_tags", expected_tags } });
        return;
    }
    const common_json l = at_key(loc, t);
    if (q.contains("instructions")) {
        check_any(errs, at_key(l, "instructions"), q.at("instructions"), true);
    }

    if (t == "noul") {
        if (!q.contains("criteria") || q.at("criteria").is_null()) {
            return;
        }
        const common_json & c  = q.at("criteria");
        const common_json   lc = at_key(l, "criteria");
        if (!c.is_object()) {
            errs.add(lc, "model_type", "Input should be a valid dictionary or instance of NoulCriteria", c,
                     common_json{ { "class_name", "NoulCriteria" } });
            return;
        }
        for (const char * key : { "true", "false" }) {
            if (c.contains(key)) {
                check_any(errs, at_key(lc, key), c.at(key), true);
            }
        }
        return;
    }

    if (!q.contains("criteria")) {
        errs.missing(l, "criteria", q);
        return;
    }
    const common_json & c  = q.at("criteria");
    const common_json   lc = at_key(l, "criteria");
    if (t == "choice") {
        if (!c.is_object()) {
            errs.add(lc, "dict_type", "Input should be a valid dictionary", c);
            return;
        }
        for (const auto & [key, val] : c.items()) {
            check_any(errs, at_key(lc, key), val, true);
        }
        // local policy: the official schema has no bounds here
        if (c.empty() || c.size() > max_options) {
            errs.add(lc, "value_error", "Value error, criteria must define 1-" + std::to_string(max_options) + " choices", c);
        }
        return;
    }

    // score
    if (!c.is_array()) {
        errs.add(lc, "list_type", "Input should be a valid list", c);
        return;
    }
    if (c.empty()) {
        errs.add(lc, "too_short", "List should have at least 1 item after validation, not 0", c, too_short("List", 0));
        return;
    }
    for (size_t i = 0; i < c.size(); ++i) {
        check_any(errs, at_idx(lc, i), c.at(i), false);
    }
    if (c.size() > max_options) {
        errs.add(lc, "value_error", "Value error, criteria must define at most " + std::to_string(max_options) + " levels", c);
    }
}

// ---------------------------------------------------------------- prompt

std::string json_text(const std::string & s) {
    return common_json::make(s).dump();
}

// instructions and criteria descriptions: strings as they are, other JSON as compact text
std::string text_of(const common_json & v) {
    if (v.is_null()) {
        return "";
    }
    return v.is_string() ? v.get<std::string>() : v.dump();
}

std::string describe(const common_json & q, const char * key) {
    return q.contains(key) ? text_of(q.at(key)) : "";
}

// ---------------------------------------------------------------- answers

// probabilities in [0, 1] that sum to 1; a one-hot on the winner if the scores are unusable
std::vector<double> clean_probs(const field_result & fr, size_t n) {
    std::vector<double> p(n, 0.0);
    double sum = 0;
    if (fr.probs.size() == n) {
        for (size_t i = 0; i < n; ++i) {
            const double x = fr.probs[i];
            p[i] = std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0;
            sum += p[i];
        }
    }
    if (!(sum > 0)) {
        std::fill(p.begin(), p.end(), 0.0);
        p[fr.winner >= 0 && (size_t) fr.winner < n ? fr.winner : 0] = 1.0;
        return p;
    }
    for (auto & x : p) {
        x /= sum;
    }
    return p;
}

double confidence(const std::vector<double> & p, confidence_mode mode) {
    if (mode == confidence_mode::max || p.size() < 2) {
        return p.size() < 2 ? 1.0 : *std::max_element(p.begin(), p.end());
    }
    double h = 0;
    for (double x : p) {
        if (x > 0) {
            h -= x * std::log(x);
        }
    }
    return std::clamp(1.0 - h / std::log((double) p.size()), 0.0, 1.0);
}

size_t argmax(const std::vector<double> & p) {
    return (size_t) (std::max_element(p.begin(), p.end()) - p.begin());
}

} // namespace

confidence_mode parse_confidence(const std::string & name) {
    if (name == "entropy") {
        return confidence_mode::entropy;
    }
    if (name == "max") {
        return confidence_mode::max;
    }
    throw std::invalid_argument("confidence must be entropy or max");
}

common_json validate(const common_json & body) {
    errors errs;
    const common_json root = common_json::array({ "body" });
    if (!body.is_object()) {
        errs.add(root, "model_attributes_type", "Input should be a valid dictionary or object to extract fields from", body);
        return errs.list;
    }

    if (!body.contains("state")) {
        errs.missing(root, "state", body);
    } else {
        check_any(errs, at_key(root, "state"), body.at("state"), false);
    }

    if (!body.contains("model")) {
        errs.missing(root, "model", body);
    } else if (!body.at("model").is_string()) {
        errs.add(at_key(root, "model"), "string_type", "Input should be a valid string", body.at("model"));
    }

    if (!body.contains("questions")) {
        errs.missing(root, "questions", body);
    } else {
        const common_json & qs = body.at("questions");
        const common_json   lq = at_key(root, "questions");
        if (!qs.is_object()) {
            errs.add(lq, "dict_type", "Input should be a valid dictionary", qs);
        } else if (qs.empty()) {
            errs.add(lq, "too_short", "Dictionary should have at least 1 item after validation, not 0", qs,
                     too_short("Dictionary", 0));
        } else {
            for (const auto & [name, q] : qs.items()) {
                check_question(errs, at_key(lq, name), q);
            }
            if (qs.size() > max_questions) {
                errs.add(lq, "value_error", "Value error, at most " + std::to_string(max_questions) + " questions are supported", qs);
            }
        }
    }
    return errs.list;
}

common_json body_errors(const std::string & body, const std::string & parse_error) {
    errors errs;
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
        errs.add(common_json::array({ "body" }), "missing", "Field required", common_json());
    } else {
        common_json loc = common_json::array({ "body" });
        loc.push_back((int64_t) 0);
        errs.add(loc, "json_invalid", "JSON decode error", common_json::object(), common_json{ { "error", parse_error } });
    }
    return errs.list;
}

common_json parse_request(const std::string & body, common_json & parsed) {
    try {
        parsed = common_json::parse(body);
    } catch (const common_json_error & e) {
        return body_errors(body, e.what());
    }
    return validate(parsed);
}

std::string error_body(const common_json & detail) {
    common_json out = common_json::object();
    out["detail"] = detail;
    return out.dump_safe();
}

std::string render_state(const common_json & state) {
    return state.is_string() ? state.get<std::string>() : state.dump(2);
}

compiled_schema compile(const common_json & questions) {
    compiled_schema cs;
    // every branch scores its question as the first key of the answer object, so the model would
    // write "," after the value when more keys follow
    const std::string terminator = questions.size() > 1 ? "," : "\n";

    std::string catalog;
    for (const auto & [name, q] : questions.items()) {
        const std::string type = q.at("type").get<std::string>();
        field_spec f;
        f.name        = name;
        f.aggregate   = "mode";
        f.description = describe(q, "instructions");

        std::string allowed;
        if (type == "noul") {
            const common_json crit = q.contains("criteria") && q.at("criteria").is_object() ? q.at("criteria") : common_json::object();
            const std::string yes  = describe(crit, "true");
            const std::string no   = describe(crit, "false");
            f.type    = "boolean";
            f.values  = { common_json(true), common_json(false) };
            f.encoded = { "true", "false" };
            allowed   = "- true: " + (yes.empty() ? std::string("yes, or the statement is true") : yes) +
                        "\n- false: " + (no.empty() ? std::string("no, or the statement is false") : no);
        } else if (type == "choice") {
            f.type = "enum";
            for (const auto & [key, val] : q.at("criteria").items()) {
                const std::string desc = text_of(val);
                f.values.push_back(common_json(key));
                f.encoded.push_back(json_text(key));
                allowed += (allowed.empty() ? "" : "\n") + std::string("- ") + json_text(key) + (desc.empty() ? "" : ": " + desc);
            }
        } else {
            f.type = "integer";
            const common_json & levels = q.at("criteria");
            for (size_t i = 0; i < levels.size(); ++i) {
                f.values.push_back(common_json((int64_t) i));
                f.encoded.push_back(std::to_string(i));
                f.numbers.push_back((double) i);
                allowed += (i ? "\n" : "") + std::string("- ") + std::to_string(i) + ": " + text_of(levels.at(i));
            }
        }
        const char * kind = type == "noul" ? "yes/no" : type == "choice" ? "choice" : "score";
        catalog += "\n\n" + json_text(name) + " (" + kind + ")" + (f.description.empty() ? "" : "\n" + f.description) +
                   "\nAllowed answers:\n" + allowed;

        cs.inputs.push_back(make_input(f, terminator));
        cs.specs.push_back(std::move(f));
    }
    cs.system_text = "You evaluate the content in the user message and answer every question below. "
                     "Each question lists its allowed answers. Reply with one JSON object that maps each question "
                     "name to one allowed answer, written exactly as listed.\n\nQuestions:" + catalog;
    return cs;
}

common_json answers(const common_json & questions, const result & r, confidence_mode mode) {
    common_json out = common_json::object();
    size_t i = 0;
    for (const auto & [name, q] : questions.items()) {
        const field_result & fr   = r.fields.at(i++);
        const std::string    type = q.at("type").get<std::string>();
        common_json a = common_json::object();
        a["type"] = type;
        if (type == "noul") {
            a["noul"] = clean_probs(fr, 2)[0];
        } else if (type == "choice") {
            const common_json & crit = q.at("criteria");
            const auto p = clean_probs(fr, crit.size());
            common_json probs = common_json::object();
            std::string best;
            size_t k = 0;
            for (const auto & [key, val] : crit.items()) {
                probs[key] = p[k];
                if (k == argmax(p)) {
                    best = key;
                }
                ++k;
            }
            a["choice"]        = best;
            a["confidence"]    = confidence(p, mode);
            a["probabilities"] = probs;
        } else {
            const common_json & levels = q.at("criteria");
            const auto p = clean_probs(fr, levels.size());
            common_json legend = common_json::object();
            common_json probs  = common_json::object();
            double score = 0;
            for (size_t k = 0; k < levels.size(); ++k) {
                legend[std::to_string(k)] = levels.at(k);
                probs[std::to_string(k)]  = p[k];
                score += (double) k * p[k];
            }
            a["score"]         = score;
            a["confidence"]    = confidence(p, mode);
            a["legend"]        = legend;
            a["probabilities"] = probs;
        }
        out[name] = a;
    }
    return out;
}

std::string file_date(const std::string & path) {
#ifdef _WIN32
    // plain stat() fails with EOVERFLOW on files over 2 GB
    struct _stat64 st;
    if (path.empty() || _stat64(path.c_str(), &st) != 0) {
        return "";
    }
#else
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) {
        return "";
    }
#endif
    const std::time_t t = st.st_mtime;
    std::tm tm {};
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) {
        return "";
    }
#else
    if (gmtime_r(&t, &tm) == nullptr) {
        return "";
    }
#endif
    char buf[16];
    return std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) ? std::string(buf) : "";
}

} // namespace systemone
} // namespace llama_decision
