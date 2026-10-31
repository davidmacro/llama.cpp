# llama-server research module (POC)

Branch `logprobintrospection`. Proof of concept of the design package in `../../../../*.md`
(`D:/repos/forkedrepos/llamacpp-logprobs/`): Type A hypertokens (sets of single next-token IDs),
static logit interventions, sparse observation of watched logits, SQLite persistence and a Svelte
research workspace. Not production grade.

## Build and run

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DLLAMA_SERVER_RESEARCH=ON -DLLAMA_BUILD_BORINGSSL=ON
cmake --build build --config Release -j --target llama-server
./build/bin/Release/llama-server -m Qwen3-4B-Instruct-2507-Q4_K_M.gguf --research-db research.sqlite --port 8080
```

- `LLAMA_SERVER_RESEARCH` (CMake, default OFF): compiles the module; SQLite amalgamation is fetched with FetchContent.
- `--research-db PATH` (runtime): enables the routes. Without it every route returns 404 `RESEARCH_DISABLED`.
- Research runs force speculative decoding off for that request; grammar/json_schema are not allowed in research runs.

## Semantics (summary, see design docs 02-04)

- Hypertoken group = set of unique token IDs. `P(H|c) = sum_{i in H} P(i|c)`.
- Watch vocabulary `Vw` = sorted union of all group members + extra IDs. Arrays in observations follow `watch_ids` order.
- Effective bias per token = sum of `add_logit_bias` deltas over enabled rules whose group contains it; `exclude` wins (`-inf`).
- Intervention is applied through the existing sampler `logit_bias` stage (merged into the request's `logit_bias`).
- Stages: `raw` = model logits row at the sampling index (before any sampler). `post_intervention` = raw + effective bias, computed (not read back from the sampler). Later samplers (penalties, temperature, top-k/p) are not reflected.
- `raw_log_z` = full-vocab logsumexp (only in `watch_normalized` mode). `adjusted_log_z = raw_log_z + log(1 + sum_{changed i} P_raw(i) (exp(delta_i) - 1))`.
- Observation at `generation_index = t` is the distribution before token t was sampled. Captured when `t % every_n == 0`. Every accepted token is recorded.

## REST API, base `/api/research/v1`

All bodies JSON. Errors: `{"error":{"code":"STRING_CODE","message":"..."}}` with 400/404/409/422.
IDs are strings with a prefix (`prj_`, `q_`, `dsg_`, `cmp_`, `int_`, `bat_`, `run_`, `prop_`) + random hex.

| Method | Route | Body / query | Response |
|---|---|---|---|
| GET | `/status` | | `{enabled, model_id, n_vocab, tokenizer_fingerprint, schema_version}` |
| GET POST | `/projects` | POST `{title, description?}` | list `{projects:[Project]}` / `Project` |
| GET | `/projects/{id}` | | `Project` + `questions:[Question]` |
| POST | `/projects/{id}/questions` | `{question_text, hypothesis?, notes?}` | `Question` |
| GET PATCH | `/questions/{id}` | PATCH any of `{question_text, hypothesis, notes, status}` | `Question` |
| GET POST | `/questions/{id}/designs` | POST `{spec}` (DesignSpec) | `{designs:[Design]}` / `Design` (state `draft`, next revision) |
| GET | `/designs/{id}` | | `Design` (+ `compiled` if compiled) |
| POST | `/compile-preview` | `{spec}` | `CompiledPlan` (not stored) |
| POST | `/designs/{id}/compile` | | `CompiledPlan` (stored, `compiled_id`) |
| POST | `/designs/{id}/approve` | | `Design` state `approved` (requires successful compile, no empty group). Approved designs are immutable (409 `IMMUTABLE`). |
| GET POST | `/questions/{id}/interventions` | POST `{design_id, rules:[Rule]}` | `{interventions:[Intervention]}` / `Intervention` |
| POST | `/interventions/{id}/approve` | | `Intervention` |
| GET | `/interventions/{id}/effective-bias` | | `{entries:[{token_id, piece, delta, excluded, rules:[rule_id]}], warnings:[]}` |
| GET | `/vocab` | `query`, `mode=substring|exact|prefix`, `limit` (<=200), `offset`, `include_special=0` | `{n_vocab, tokens:[TokenInfo]}` |
| GET | `/tokens/{id}` | | `TokenInfo` |
| POST | `/tokenize-preview` | `{text, add_special=false, parse_special=false}` | `{tokens:[TokenInfo]}` |
| POST | `/proposals` | `{question_id, model, prompt, raw_output, proposal}` | `Proposal` (stored, untrusted) |
| GET | `/questions/{id}/proposals` | | `{proposals:[Proposal]}` |
| POST | `/batches` | BatchRequest | 202 `{batch_id, run_ids, status}` |
| GET | `/batches/{id}` | | `Batch` + `runs:[RunSummary]` |
| GET | `/runs` | `question_id?`, `batch_id?` | `{runs:[RunSummary]}` |
| GET | `/runs/{id}` | | `RunManifest` |
| GET | `/runs/{id}/tokens` | | `{tokens:[{generation_index, token_id, piece, piece_b64, context_position}]}` |
| GET | `/runs/{id}/observations` | `from?`, `to?` | `{watch_ids:[...], groups:[{id,name}], observations:[Observation]}` |
| GET | `/runs/{id}/events` | `after=N` | SSE stream (below); replays from DB/memory then follows live until `research.run_finished` |
| POST | `/runs/{id}/cancel` | | `RunSummary` |
| GET | `/runs/{id}/export` | `format=jsonl|csv` | file download (`jsonl`: manifest + tokens + observations; `csv`: long form `run_id,generation_index,stage,token_id,logit`) |
| POST | `/counterfactual` | `{run_id, generation_index, intervention_id}` | same-prefix mechanical effect: `{raw:{groups}, adjusted:{groups}}` recomputed from stored raw logits |

### Types

```jsonc
// DesignSpec (authored, design doc 02)
{"schema_version":1,
 "groups":[{"id":"paris","name":"PARIS","role":"target|competitor|control",
            "selectors":[{"kind":"exact_token_piece","text":" Paris"},
                         {"kind":"variant_list","texts":["paris"," Parijs"]},
                         {"kind":"token_ids","ids":[1234]}]}],
 "watch_extra_token_ids":[],
 "unknown_multi_token_policy":"flag_and_exclude"}

// TokenInfo
{"id":12366,"piece":" Paris","piece_escaped":"\u0120Paris-or-readable","piece_b64":"IFBhcmlz","type":"normal|control|user_defined|byte|unknown","special":false}

// CompiledPlan
{"compiled_id":"cmp_..|null","tokenizer_fingerprint":"sha256:..","model_id":"..","watch_ids":[..sorted..],
 "groups":[{"id":"paris","name":"PARIS","role":"target","members":[ids],"warnings":[]}],
 "candidates":[{"group_id":"paris","selector_kind":"exact_token_piece","value":" Paris","status":"accepted_single_token|multi_token|not_found|special_token|ambiguous_duplicate_piece","token_ids":[..],"pieces":[".."]}],
 "overlaps":[{"token_id":1,"group_ids":["a","b"]}],
 "warnings":["..."],"fingerprint":"sha256:..","ok":true}

// Rule (design doc 03)
{"id":"r1","target_group_id":"paris","operation":"add_logit_bias|exclude","delta":-3.0,"enabled":true}

// BatchRequest
{"question_id":"q_..","design_id":"dsg_..",           // design must be approved (POC: compiled is enough)
 "arms":[{"name":"control","intervention_id":null},{"name":"suppress_paris","intervention_id":"int_.."}],
 "prompt":"The capital of France is","chat":true,       // chat=true applies the chat template to a single user message
 "seeds":[17,29],"n_predict":64,
 "sampling":{"temperature":0.8,"top_k":40,"top_p":0.95},
 "collection":{"every_n":5,"mode":"raw_watch|watch_normalized","top_k":0}}
// runs = arms x seeds, executed sequentially in a background thread

// Observation
{"generation_index":5,"context_position":31,"selected_token_id":12366,
 "raw":[..watch order..],"adjusted":[..],"raw_log_z":null,"adjusted_log_z":null,
 "groups":[{"id":"paris","raw_lse":..,"adj_lse":..,"raw_rel":..,"adj_rel":..,"raw_abs":null,"adj_abs":null}],
 "top_k":[{"id":..,"logit":..}],"capture_state":"ok"}

// RunSummary
{"id":"run_..","batch_id":"bat_..","question_id":"q_..","arm":"control","intervention_id":null,"seed":17,
 "status":"queued|running|completed|failed|cancelled|interrupted","n_tokens":64,"captured":13,"skipped":51,"dropped":0,
 "text":"...","started_at":"..","ended_at":".."}
// RunManifest = RunSummary + {prompt, rendered_prompt, sampling, collection, compiled: CompiledPlan,
//   effective_bias:[{token_id,delta,excluded}], model_id, tokenizer_fingerprint, llama_build, stage_semantics, plan_hash}
```

### SSE events (`GET /runs/{id}/events?after=N`)

`id:` is the per-run monotonic `event_seq`. Event names and `data` (JSON, all include `run_id`, `event_seq`):

- `research.run_started` `{status:"running"}`
- `research.token_accepted` `{generation_index, token_id, piece, piece_b64, context_position}`
- `research.observation` `Observation`
- `research.run_finished` `{status, n_tokens, captured, skipped, dropped, text}`

### Implementation notes (backend, additive to the contract above)

- Non-finite values are JSON `null`: in `Observation.adjusted` a `null` entry means `-inf` (excluded token); `raw_log_z`/`adjusted_log_z`/`*_abs` are `null` in `raw_watch` mode.
- `GET /vocab` also returns `total` (number of matches) and `offset`. Substring/prefix matching is ASCII case-insensitive; `exact` is byte-exact.
- Extra route: `GET /interventions/{id}`. `Intervention` = `{id, question_id, design_id, compiled_id, revision, state, rules, effective, warnings, created_at, approved_at}`.
- `Design` = `{id, question_id, revision, state, spec, compiled_id, content_hash, created_at, approved_at}`; approving twice returns the design unchanged.
- `research.run_finished` and `RunSummary` carry `error` (string or null). The SSE stream sends `: ping` comments every 10 s while idle; runs not in memory (finished, or from an earlier process) are replayed from the DB and the stream ends.
- `POST /counterfactual` returns `{run_id, generation_index, intervention_id, watch_ids, raw:{logits, log_z, groups}, adjusted:{logits, log_z, groups}, warnings}`; `groups` have the same fields as in `Observation`.
- Batch arms require interventions of the same design and compiled plan (422 `INTERVENTION_DESIGN_MISMATCH`, 409 `STALE_INTERVENTION`); `sampling` must not contain `grammar`, `json_schema`, `response_format` (422 `GRAMMAR_NOT_ALLOWED`) or `logit_bias`. Runs use `cache_prompt=false`, no speculative decoding, no backend sampling.
- `PATCH` was added to the default CORS methods (`common_params::cors_methods`).
- The math self-test (design doc 13 fixtures 1-3) runs at startup when `--research-db` is set; failure aborts startup.

## Files

| Path | Content |
|---|---|
| `research-math.{h,cpp}` | logsumexp, group masses, effective bias, adjusted log Z |
| `research-compile.{h,cpp}` | vocab introspection, selector resolution, compiled plan + fingerprint |
| `research-store.{h,cpp}` | SQLite schema, migrations, CRUD, run/token/observation persistence |
| `research-capture.{h,cpp}` | per-run collector used by the slot sampling path, event log for SSE |
| `research-api.{h,cpp}` | route handlers and batch runner |
