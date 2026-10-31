#pragma once

#include "server-http.h"

#include <memory>
#include <string>
#include <vector>

struct common_params;
struct server_context;

// REST API of the research module (/api/research/v1) and the batch runner

struct research_route {
    std::string method; // GET | POST | PATCH
    std::string path;
    server_http_context::handler_t handler;
};

struct research_api {
    research_api(const common_params & params, server_context & ctx_server);
    ~research_api();

    // call after the model is loaded; opens the DB when --research-db is set
    // returns false on a fatal error
    bool init();

    // stop the batch runner (cancels the current run)
    void stop();

    std::vector<research_route> routes();

    struct impl;
    std::unique_ptr<impl> pimpl;
};
