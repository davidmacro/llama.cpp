#include "research-store.h"

#include "sqlite3.h"

#include <chrono>
#include <ctime>
#include <random>
#include <stdexcept>

using json = common_json;

static const char * RESEARCH_SCHEMA = R"SQL(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS projects (
  id TEXT PRIMARY KEY, title TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS questions (
  id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES projects(id),
  question_text TEXT NOT NULL, hypothesis TEXT, notes TEXT,
  status TEXT NOT NULL DEFAULT 'draft' CHECK (status IN ('draft','active','archived')),
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_questions_project ON questions(project_id);
CREATE TABLE IF NOT EXISTS compiled_plans (
  id TEXT PRIMARY KEY, design_id TEXT REFERENCES designs(id),
  fingerprint TEXT NOT NULL, tokenizer_fingerprint TEXT NOT NULL,
  plan_json TEXT NOT NULL CHECK (json_valid(plan_json)), created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS designs (
  id TEXT PRIMARY KEY, question_id TEXT NOT NULL REFERENCES questions(id),
  revision INTEGER NOT NULL CHECK (revision > 0),
  state TEXT NOT NULL CHECK (state IN ('draft','approved')),
  spec_json TEXT NOT NULL CHECK (json_valid(spec_json)),
  compiled_id TEXT, content_hash TEXT,
  created_at TEXT NOT NULL, approved_at TEXT,
  UNIQUE (question_id, revision)
);
CREATE TABLE IF NOT EXISTS interventions (
  id TEXT PRIMARY KEY, question_id TEXT NOT NULL REFERENCES questions(id),
  design_id TEXT NOT NULL REFERENCES designs(id), compiled_id TEXT NOT NULL REFERENCES compiled_plans(id),
  revision INTEGER NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('draft','approved')),
  rules_json TEXT NOT NULL CHECK (json_valid(rules_json)),
  effective_json TEXT NOT NULL CHECK (json_valid(effective_json)),
  warnings_json TEXT NOT NULL DEFAULT '[]',
  created_at TEXT NOT NULL, approved_at TEXT
);
CREATE TABLE IF NOT EXISTS proposals (
  id TEXT PRIMARY KEY, question_id TEXT NOT NULL REFERENCES questions(id),
  model TEXT, prompt TEXT, raw_output TEXT,
  proposal_json TEXT, review_state TEXT NOT NULL DEFAULT 'untrusted', created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS batches (
  id TEXT PRIMARY KEY, question_id TEXT NOT NULL REFERENCES questions(id),
  design_id TEXT NOT NULL REFERENCES designs(id),
  request_json TEXT NOT NULL CHECK (json_valid(request_json)),
  status TEXT NOT NULL, created_at TEXT NOT NULL, ended_at TEXT
);
CREATE TABLE IF NOT EXISTS runs (
  id TEXT PRIMARY KEY, batch_id TEXT NOT NULL REFERENCES batches(id),
  question_id TEXT NOT NULL, arm TEXT NOT NULL, intervention_id TEXT, seed INTEGER,
  status TEXT NOT NULL, error TEXT,
  n_tokens INTEGER NOT NULL DEFAULT 0, captured INTEGER NOT NULL DEFAULT 0,
  skipped INTEGER NOT NULL DEFAULT 0, dropped INTEGER NOT NULL DEFAULT 0,
  text TEXT NOT NULL DEFAULT '',
  manifest_json TEXT NOT NULL CHECK (json_valid(manifest_json)),
  created_at TEXT NOT NULL, started_at TEXT, ended_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_runs_batch ON runs(batch_id);
CREATE INDEX IF NOT EXISTS idx_runs_question ON runs(question_id);
CREATE TABLE IF NOT EXISTS accepted_tokens (
  run_id TEXT NOT NULL REFERENCES runs(id),
  generation_index INTEGER NOT NULL CHECK (generation_index >= 0),
  token_id INTEGER NOT NULL, piece TEXT NOT NULL, piece_b64 TEXT NOT NULL, context_position INTEGER,
  PRIMARY KEY (run_id, generation_index)
);
CREATE TABLE IF NOT EXISTS observations (
  run_id TEXT NOT NULL REFERENCES runs(id),
  generation_index INTEGER NOT NULL,
  obs_json TEXT NOT NULL CHECK (json_valid(obs_json)),
  PRIMARY KEY (run_id, generation_index)
);
CREATE TABLE IF NOT EXISTS run_events (
  run_id TEXT NOT NULL REFERENCES runs(id),
  event_seq INTEGER NOT NULL, name TEXT NOT NULL,
  data_json TEXT NOT NULL CHECK (json_valid(data_json)),
  PRIMARY KEY (run_id, event_seq)
);
)SQL";

research_store::~research_store() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

void research_store::open(const std::string & path) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        throw std::runtime_error("research: cannot open " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db, 5000);
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA synchronous = NORMAL");

    char * err = nullptr;
    if (sqlite3_exec(db, RESEARCH_SCHEMA, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("research: schema error: " + msg);
    }
    exec("INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)", {std::to_string(RESEARCH_SCHEMA_VERSION)});
    exec("UPDATE runs SET status = 'interrupted', ended_at = COALESCE(ended_at, ?) WHERE status IN ('queued', 'running')", {research_now_iso()});
    exec("UPDATE batches SET status = 'interrupted', ended_at = COALESCE(ended_at, ?) WHERE status IN ('queued', 'running')", {research_now_iso()});
}

static bool ends_with(const std::string & s, const std::string & suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

json research_store::query(const std::string & sql, const std::vector<json> & params) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    sqlite3_stmt * st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("research: sql prepare: ") + sqlite3_errmsg(db));
    }
    for (size_t i = 0; i < params.size(); i++) {
        const json & p = params[i];
        const int k = (int) i + 1;
        if (p.is_null()) {
            sqlite3_bind_null(st, k);
        } else if (p.is_boolean()) {
            sqlite3_bind_int(st, k, p.get<bool>() ? 1 : 0);
        } else if (p.is_number_integer()) {
            sqlite3_bind_int64(st, k, p.get<long long>());
        } else if (p.is_number()) {
            sqlite3_bind_double(st, k, p.get<double>());
        } else if (p.is_string()) {
            const std::string s = p.get<std::string>();
            sqlite3_bind_text(st, k, s.c_str(), (int) s.size(), SQLITE_TRANSIENT);
        } else {
            const std::string s = p.dump_safe();
            sqlite3_bind_text(st, k, s.c_str(), (int) s.size(), SQLITE_TRANSIENT);
        }
    }

    json rows = json::array();
    while (true) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            const std::string msg = sqlite3_errmsg(db);
            sqlite3_finalize(st);
            throw std::runtime_error("research: sql step: " + msg);
        }
        json row = json::object();
        const int n = sqlite3_column_count(st);
        for (int c = 0; c < n; c++) {
            const std::string name = sqlite3_column_name(st, c);
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER: row[name] = (long long) sqlite3_column_int64(st, c); break;
                case SQLITE_FLOAT:   row[name] = sqlite3_column_double(st, c); break;
                case SQLITE_TEXT: {
                    const std::string s((const char *) sqlite3_column_text(st, c), sqlite3_column_bytes(st, c));
                    if (ends_with(name, "_json")) {
                        row[name.substr(0, name.size() - 5)] = json::parse(s);
                    } else {
                        row[name] = s;
                    }
                } break;
                default: row[ends_with(name, "_json") ? name.substr(0, name.size() - 5) : name] = nullptr; break;
            }
        }
        rows.push_back(row);
    }
    sqlite3_finalize(st);
    return rows;
}

json research_store::query_one(const std::string & sql, const std::vector<json> & params) {
    json rows = query(sql, params);
    if (rows.empty()) {
        return json(nullptr);
    }
    return rows.at(0);
}

void research_store::exec(const std::string & sql, const std::vector<json> & params) {
    query(sql, params);
}

void research_store::begin()    { mtx.lock(); exec("BEGIN"); }
void research_store::commit()   { exec("COMMIT");   mtx.unlock(); }
void research_store::rollback() { exec("ROLLBACK"); mtx.unlock(); }

std::string research_now_iso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = (int) (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    snprintf(out, sizeof(out), "%s.%03dZ", buf, ms);
    return out;
}

std::string research_new_id(const std::string & prefix) {
    static std::mutex m;
    static std::mt19937_64 rng(std::random_device{}() ^ (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count());
    std::lock_guard<std::mutex> lock(m);
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) rng());
    return prefix + buf;
}
