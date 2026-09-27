# parallel-decision

Answer a finite JSON schema in one batched forward pass, instead of generating the JSON token by token.

Every field of a schema has a fixed set of allowed values (enums, booleans, bounded integers, numbers on a grid).
After the context, each field's allowed values are scored as token paths that fork from the same KV cache, so all
fields are answered in one `llama_decode` and cannot see each other. Each answer comes back with a probability, and
the JSON object is assembled by code, so it always matches the schema.

This directory holds the engine (`decision-engine.*`), a CLI (`llama-parallel-decision`), and the engine is also
served by `llama-server` as `POST /v1/decision` and, in the TypeSafe System One format, as `POST /v1/systemone`.

## Build

Same as llama.cpp:

```bash
cmake -B build -DGGML_CUDA=ON        # or plain `cmake -B build` for CPU / Metal
cmake --build build --config Release -j
```

## Run the server

`--decision-seqs N` reserves the sequence slots the decisions need: one holds the cached instructions, one per context
in flight, the rest are the parallel questions. It also switches the KV cache to unified, which is what lets the
branches share the context's cells.

```bash
./build/bin/llama-server -m model.gguf -ngl 99 -fa on -c 32768 --decision-seqs 24 --port 8096
```

With a presets file, one loaded model serves chat and decisions:

```ini
[*]
ngl = 99
fa = on
jinja = 1
parallel = 1
cache-type-k = q8_0
cache-type-v = q8_0
decision-seqs = 24

[gemma-4-12b]
model = ./models/gemma-4-12b-it-UD-Q4_K_XL.gguf
ctx-size = 32768
ubatch-size = 512
decision-seqs = 12
```

```bash
./build/bin/llama-server --models-preset models.ini --models-max 1 --port 8096
```

How many sequences a model affords depends on its attention. A plain-attention model shares the context's cells, so
128 sequences cost almost nothing. A sliding-window model (Gemma) allocates its window per sequence, so keep it low
(12 on a 12 GB card). Hybrid models with recurrent layers (Qwen3.5, Nemotron-H) keep a recurrent state per
sequence (about 50 MB each for Qwen3.5 4B and 9B), and llama.cpp only batches their sequences together when they hold
the same number of tokens. The engine right-pads each group of branches to its longest one, so they still score in a
single pass; the padding comes after the token that is read, so it doesn't change the result.

## POST /v1/decision

`contexts` is a list of 1-256 strings. They share one schema, one set of instructions, and one cached prefix; results
come back in the same order.

```bash
curl http://localhost:8096/v1/decision -H "Content-Type: application/json" -d '{
  "model": "gemma-4-12b",
  "instructions": "Answer each question about this support request from its state.",
  "schema": {
    "category": {"type": "enum", "choices": ["billing","technical","cancellation","other"],
                 "description": "What type of support request is this?"},
    "urgent":   {"type": "boolean", "description": "Does this need urgent handling?"},
    "priority": {"type": "enum", "choices": ["low","medium","high","critical"],
                 "description": "Rate support priority."}
  },
  "contexts": ["I was charged twice and need this fixed today."]
}'
```

```json
{
  "object": "decision",
  "results": [
    {
      "decision": {"category": "billing", "urgent": true, "priority": "high"},
      "fields": {
        "category": {"value": "billing",  "probability": 1.0,  "scored_nodes": 1, "tree": true},
        "urgent":   {"value": true,       "probability": 1.0,  "scored_nodes": 1, "tree": true},
        "priority": {"value": "high",     "probability": 0.74, "scored_nodes": 1, "tree": true}
      },
      "usage": {"context_tokens": 21, "scored_rows": 14}
    }
  ],
  "usage": {"prompt_tokens": 137, "cached_tokens": 116, "context_tokens": 21, "scored_rows": 14},
  "timings": {"prefill_ms": 50.7, "scoring_ms": 50.0, "total_ms": 100.7, "rounds": 1, "per_decision_ms": 100.7}
}
```

(That response is a real one: Gemma 4 12B on an RTX 3060, warm cache.)

### Schema

Compact fields, or a JSON Schema object with `properties`:

| type | keys | notes |
|---|---|---|
| `enum` | `choices` (or `enum`) | 1-255 values |
| `boolean` | - | true / false |
| `integer` | `minimum`, `maximum` | 1-255 values |
| `number` | `minimum`, `maximum`, `step` (`multipleOf` in JSON Schema) | fixed-width decimals |

Numeric fields take `aggregate`: `mode` (default), `median` or `mean`.

### Options

| field | default | meaning |
|---|---|---|
| `instructions` | `""` | prepended to the generated field catalogue; cached with it |
| `mode` | `auto` | `tree` scores every divergence node and returns exact probabilities; `greedy` walks the trie; `auto` picks tree up to `tree_max` values |
| `tree_max` | 128 | per-field switch between tree and greedy |
| `cache_prompt` | true | reuse the cached instructions + schema prefix |

## POST /v1/systemone

The same engine also speaks TypeSafe's [System One](https://api.typesafe.ai/openapi.json) wire format, so the official
SDKs work against llama-server by changing only the base URL. It needs `--decision-seqs` like `/v1/decision`.

```bash
./build/bin/llama-server -m model.gguf -a jev-latest -ngl 99 -fa on -sm none --decision-seqs 128 --port 8096
```

```python
from typesafe_sdk import Choice, TypeSafeClient

with TypeSafeClient(api_key="local", base_url="http://localhost:8096") as client:
    r = client.system_one(state="I was charged twice.",
                          questions={"team": Choice(criteria={"billing": None, "technical": None})})
```

How questions map to the engine; every question is scored in tree mode, so each answer has the full distribution:

| question | scored values | answer |
|---|---|---|
| `noul` | `true`, `false` | `noul` = p(true) |
| `choice` | the `criteria` keys as JSON strings | `choice` = most likely key, `probabilities` per key |
| `score` | `0` .. `n-1` | `score` = sum of `i * p_i`, `legend` = the criteria, `probabilities` per level |

- The questions (with their instructions and criteria descriptions) form the cached system prompt; the state is the
  user message (objects and arrays as indented JSON). Each question is scored as the first key of the answer object.
- Extension, per `choice` question: `"x_labels": "names"` (default: the model writes the option name), `"letters"`
  (`A`..`Z`, at most 26 options) or `"numbers"` (`1`..`N`). With codes, the prompt lists `- "A" = "billing": ...` and
  the model answers with the code; `choice` and `probabilities` stay keyed by the names. Different questions in one
  request can use different modes. On Gemma 4 E4B names were never worse than codes and clearly better when options
  have no description (codes: -4 to -7 points accuracy, answer changes with option order for 22-25% of items vs 0-17%);
  codes only matched names with opaque keys such as ids.
- `confidence`: `--systemone-confidence entropy` (default, 1 - normalised entropy) or `max` (highest probability).
  Neither is TypeSafe's calibrated confidence.
- `usage.input_tokens` is the whole rendered prompt, cached or not; `usage.output_tokens` is the number of questions.
- Invalid requests get 422 `{"detail": [...]}` in FastAPI / pydantic form (types, `loc` and messages checked against
  the SDK's pydantic schemas). Unknown model: 404, `--decision-seqs` missing: 503, both `{"detail": "..."}`.
- Local limits: 32 questions, 255 options or levels per question (422 `value_error` above that).
- `model` must be the model name or an `--alias`; the response names the model (the first alias, sorted).
- Requests that arrive while the server is busy are coalesced: those with the same questions share one batch and one
  cached prefix. Size `--decision-seqs` to about `(1 + questions) x concurrent requests`; a short budget splits a
  batch into sequential groups.
- `?debug=1` adds `x_debug` (timings, batch size, cache hit, the system prompt) to the response.
- `GET /v1/models` adds `models[]` (one entry per name and alias) with `description` and `release_date`, from
  `--model-description` / `--model-release-date` or the GGUF `general.description` and the model file's date. The
  OpenAI `data[]` list is unchanged. In router mode the same options work per preset.

Gemma 4 E4B (Q4_K_XL) on one RTX PRO 6000, 5 questions, 304 cached + 33 state tokens: 30 ms warm, 72 ms with a new
question set; about 110 requests/s at 16 concurrent clients and 120 at 64. On a multi-GPU machine `-sm none` saves
about 12 ms per request for a model that fits on one card.

## CLI

`llama-parallel-decision` runs the same engine from a worker process (stdin/stdout protocol, one JSON request per
line). Environment: `DECIDE_TREE`, `DECIDE_TREE_MAX`, `DECIDE_NSEQ`, `DECIDE_SPLIT_BOUNDARY`.

## A UI for it

[decision-playground](https://github.com/thecodacus/decision-playground) is a browser-only playground: it talks
straight to your llama-server, runs a decision and the same question as a chat completion side by side with live
timers, and has a small game whose agents decide through the endpoint.
