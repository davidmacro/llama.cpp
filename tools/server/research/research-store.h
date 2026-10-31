#pragma once

#include "json.h"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

// SQLite store for the research module; one connection guarded by one mutex (POC)

#define RESEARCH_SCHEMA_VERSION 1

struct research_store {
    sqlite3 * db = nullptr;
    std::recursive_mutex mtx;

    ~research_store();

    // opens the DB, applies the schema, marks queued/running runs as interrupted
    void open(const std::string & path);

    // run one statement with bound params (string, int, double, bool, null)
    // returns rows as objects keyed by column name; columns ending in "_json" are parsed
    common_json query(const std::string & sql, const std::vector<common_json> & params = {});
    common_json query_one(const std::string & sql, const std::vector<common_json> & params = {}); // null if no row
    void exec(const std::string & sql, const std::vector<common_json> & params = {});

    void begin();
    void commit();
    void rollback();
};

std::string research_now_iso();
std::string research_new_id(const std::string & prefix);
