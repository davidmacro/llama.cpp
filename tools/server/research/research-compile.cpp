#include "research-compile.h"

#include "common.h"
#include "hash/hash.h"

#include <algorithm>
#include <map>
#include <set>

using json = common_json;

std::string research_sha256(const std::string & data) {
    return "sha256:" + hash_sha256_hex(data.data(), data.size());
}

std::string research_base64(const std::string & data) {
    static const char * tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = ((uint8_t) data[i] << 16) | ((uint8_t) data[i + 1] << 8) | (uint8_t) data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = ((uint8_t) data[i] << 16);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = ((uint8_t) data[i] << 16) | ((uint8_t) data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// length of a valid UTF-8 sequence at s[i], 0 if invalid
static size_t utf8_len(const std::string & s, size_t i) {
    const uint8_t c = (uint8_t) s[i];
    size_t n = 0;
    if (c < 0x80) {
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (size_t k = 1; k < n; k++) {
        if (((uint8_t) s[i + k] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

bool research_valid_utf8(const std::string & s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_len(s, i);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

std::string research_escape_piece(const std::string & piece) {
    std::string out;
    char buf[8];
    for (size_t i = 0; i < piece.size();) {
        const uint8_t c = (uint8_t) piece[i];
        if (c == '\n') {
            out += "\\n";
            i++;
        } else if (c == '\t') {
            out += "\\t";
            i++;
        } else if (c == '\r') {
            out += "\\r";
            i++;
        } else if (c == '\\') {
            out += "\\\\";
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
            i++;
        } else {
            const size_t n = utf8_len(piece, i);
            if (n == 0) {
                snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
                i++;
            } else {
                out.append(piece, i, n);
                i += n;
            }
        }
    }
    return out;
}

static const char * attr_type(llama_token_attr a) {
    if (a & LLAMA_TOKEN_ATTR_CONTROL)      return "control";
    if (a & LLAMA_TOKEN_ATTR_USER_DEFINED) return "user_defined";
    if (a & LLAMA_TOKEN_ATTR_BYTE)         return "byte";
    if (a & LLAMA_TOKEN_ATTR_UNUSED)       return "unused";
    if (a & LLAMA_TOKEN_ATTR_NORMAL)       return "normal";
    return "unknown";
}

void research_vocab::init(const llama_model * model, const std::string & mid) {
    vocab    = llama_model_get_vocab(model);
    n_vocab  = llama_vocab_n_tokens(vocab);
    model_id = mid;

    pieces.assign(n_vocab, std::string());
    types.assign(n_vocab, "unknown");
    special.assign(n_vocab, 0);
    by_piece.clear();

    std::string fp_buf = std::to_string(n_vocab) + "\n";
    for (llama_token id = 0; id < n_vocab; id++) {
        const llama_token_attr a = llama_vocab_get_attr(vocab, id);
        pieces[id] = common_token_to_piece(vocab, id, true);
        types[id]  = attr_type(a);
        special[id] = (a & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_UNKNOWN | LLAMA_TOKEN_ATTR_UNUSED)) || llama_vocab_is_control(vocab, id) || llama_vocab_is_eog(vocab, id);
        by_piece[pieces[id]].push_back(id);

        fp_buf += pieces[id];
        fp_buf += '\0';
        fp_buf += std::to_string((int) a);
        fp_buf += '\n';
    }
    fingerprint = research_sha256(fp_buf);
}

json research_vocab::token_json(llama_token id) const {
    const std::string & p = pieces[id];
    return json {
        {"id",            id},
        {"piece",         research_valid_utf8(p) ? p : research_escape_piece(p)},
        {"piece_escaped", research_escape_piece(p)},
        {"piece_b64",     research_base64(p)},
        {"type",          types[id]},
        {"special",       (bool) special[id]},
    };
}

research_plan research_plan::from_json(const json & plan) {
    research_plan p;
    p.fingerprint           = plan.value("fingerprint", std::string());
    p.tokenizer_fingerprint = plan.value("tokenizer_fingerprint", std::string());
    for (const auto & id : plan.at("watch_ids")) {
        p.watch_ids.push_back(id.get<int>());
    }
    std::map<llama_token, uint32_t> idx;
    for (size_t i = 0; i < p.watch_ids.size(); i++) {
        idx[p.watch_ids[i]] = (uint32_t) i;
    }
    for (const auto & g : plan.at("groups")) {
        research_plan_group pg;
        pg.id   = g.value("id", std::string());
        pg.name = g.value("name", pg.id);
        for (const auto & m : g.at("members")) {
            const llama_token id = m.get<int>();
            pg.members.push_back(id);
            pg.watch_idx.push_back(idx.at(id));
        }
        p.groups.push_back(std::move(pg));
    }
    return p;
}

// resolve one text by exact piece equality; fills candidate fields, returns accepted ids
static std::vector<llama_token> resolve_piece(const research_vocab & rv, const std::string & text, json & cand, bool allow_tokenize) {
    std::vector<llama_token> accepted;
    json ids = json::array();
    json pieces = json::array();

    auto it = rv.by_piece.find(text);
    if (it != rv.by_piece.end()) {
        std::vector<llama_token> normal;
        for (llama_token id : it->second) {
            ids.push_back(id);
            pieces.push_back(research_escape_piece(rv.pieces[id]));
            if (!rv.special[id]) {
                normal.push_back(id);
            }
        }
        if (normal.empty()) {
            cand["status"] = "special_token";
        } else if (normal.size() > 1) {
            cand["status"] = "ambiguous_duplicate_piece";
            accepted = normal;
        } else {
            cand["status"] = "accepted_single_token";
            accepted = normal;
        }
    } else if (allow_tokenize) {
        const auto toks = common_tokenize(rv.vocab, text, false, false);
        for (llama_token id : toks) {
            ids.push_back(id);
            pieces.push_back(research_escape_piece(rv.pieces[id]));
        }
        if (toks.empty()) {
            cand["status"] = "not_found";
        } else if (toks.size() > 1) {
            cand["status"] = "multi_token";
        } else if (rv.special[toks[0]]) {
            cand["status"] = "special_token";
        } else {
            cand["status"] = "accepted_single_token";
            accepted = toks;
        }
    } else {
        cand["status"] = "not_found";
    }
    cand["token_ids"] = ids;
    cand["pieces"]    = pieces;
    return accepted;
}

json research_compile(const research_vocab & rv, const json & spec) {
    json warnings   = json::array();
    json candidates = json::array();
    json groups_out = json::array();
    bool ok = true;

    if (!spec.is_object() || !spec.contains("groups") || !spec.at("groups").is_array()) {
        throw std::invalid_argument("spec.groups must be an array");
    }

    std::map<llama_token, std::vector<std::string>> owners;
    std::set<llama_token> watch;
    std::set<std::string> group_ids;

    for (const auto & g : spec.at("groups")) {
        const std::string gid  = g.value("id", std::string());
        const std::string name = g.value("name", gid);
        const std::string role = g.value("role", std::string("target"));
        if (gid.empty()) {
            throw std::invalid_argument("every group needs an id");
        }
        if (!group_ids.insert(gid).second) {
            throw std::invalid_argument("duplicate group id: " + gid);
        }
        std::set<llama_token> members;
        json gwarn = json::array();

        const json selectors = g.value("selectors", json::array());
        for (const auto & sel : selectors) {
            const std::string kind = sel.value("kind", std::string());
            if (kind == "token_ids") {
                for (const auto & v : sel.value("ids", json::array())) {
                    const int64_t id = v.get<long long>();
                    json cand = {
                        {"group_id", gid}, {"selector_kind", kind}, {"value", std::to_string(id)},
                    };
                    json ids = json::array();
                    json pcs = json::array();
                    if (!rv.valid(id)) {
                        cand["status"] = "not_found";
                    } else {
                        ids.push_back((int) id);
                        pcs.push_back(research_escape_piece(rv.pieces[id]));
                        if (rv.special[id]) {
                            cand["status"] = "special_token";
                        } else {
                            cand["status"] = "accepted_single_token";
                            members.insert((llama_token) id);
                        }
                    }
                    cand["token_ids"] = ids;
                    cand["pieces"]    = pcs;
                    candidates.push_back(cand);
                }
            } else if (kind == "exact_token_piece" || kind == "variant_list") {
                std::vector<std::string> texts;
                if (kind == "exact_token_piece") {
                    texts.push_back(sel.value("text", std::string()));
                } else {
                    for (const auto & t : sel.value("texts", json::array())) {
                        texts.push_back(t.get<std::string>());
                    }
                }
                for (const auto & text : texts) {
                    json cand = {
                        {"group_id", gid}, {"selector_kind", kind}, {"value", text},
                    };
                    for (llama_token id : resolve_piece(rv, text, cand, kind == "variant_list")) {
                        members.insert(id);
                    }
                    candidates.push_back(cand);
                }
            } else {
                gwarn.push_back("unsupported selector kind: " + kind);
            }
        }

        if (members.empty()) {
            gwarn.push_back("group " + gid + " has no members");
            warnings.push_back("group " + gid + " has no members");
            ok = false;
        }

        std::vector<llama_token> mem(members.begin(), members.end());
        for (llama_token id : mem) {
            owners[id].push_back(gid);
            watch.insert(id);
        }
        groups_out.push_back(json {
            {"id", gid}, {"name", name}, {"role", role}, {"members", mem}, {"warnings", gwarn},
        });
    }

    for (const auto & v : spec.value("watch_extra_token_ids", json::array())) {
        const int64_t id = v.get<long long>();
        if (rv.valid(id)) {
            watch.insert((llama_token) id);
        } else {
            warnings.push_back("watch_extra_token_ids: invalid id " + std::to_string(id));
        }
    }

    json overlaps = json::array();
    for (const auto & [id, gs] : owners) {
        if (gs.size() > 1) {
            overlaps.push_back(json {{"token_id", id}, {"group_ids", gs}});
        }
    }
    if (!overlaps.empty()) {
        warnings.push_back("some tokens belong to more than one group; group masses may sum to more than 1");
    }
    if (groups_out.empty()) {
        warnings.push_back("design has no groups");
        ok = false;
    }

    std::vector<llama_token> watch_ids(watch.begin(), watch.end());

    std::string fp_src = spec.dump() + "\n" + rv.fingerprint + "\n" + RESEARCH_COMPILER_VERSION + "\n";
    for (llama_token id : watch_ids) {
        fp_src += std::to_string(id) + ",";
    }

    return json {
        {"compiled_id",           nullptr},
        {"tokenizer_fingerprint", rv.fingerprint},
        {"model_id",              rv.model_id},
        {"compiler_version",      RESEARCH_COMPILER_VERSION},
        {"watch_ids",             watch_ids},
        {"groups",                groups_out},
        {"candidates",            candidates},
        {"overlaps",              overlaps},
        {"warnings",              warnings},
        {"fingerprint",           research_sha256(fp_src)},
        {"ok",                    ok},
    };
}
