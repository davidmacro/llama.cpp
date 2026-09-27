# llama.cpp - System One

A fork of [llama.cpp](https://github.com/ggml-org/llama.cpp) whose `llama-server` natively serves TypeSafe AI's
[System One](https://api.typesafe.ai/docs) API: `POST /v1/systemone` and `GET /v1/models`, wire-compatible with the
official [OpenAPI](https://api.typesafe.ai/openapi.json). The official SDKs work against a local GGUF model by changing
only the base URL. No adapter or proxy runs in front of the server.

> Wire-compatible does not mean behaviour-compatible. A local model gives System One-*style* answers: its
> classifications, probabilities and confidence differ from TypeSafe's hosted Jev models, and are not calibrated
> the same way.

Branch `systemone`. The upstream llama.cpp README is kept as [README-llama.cpp.md](README-llama.cpp.md).

## How it works

Every question has a finite set of allowed answers. Instead of generating JSON token by token, the server scores all
allowed answers of all questions in one batched forward pass, forked from one cached prompt, using the parallel
decision engine in [tools/parallel-decision](tools/parallel-decision). Each answer comes with the exact probability
distribution over its allowed values, normalised over those values only (no top-k truncation).

| Question | Scored values | Answer |
|---|---|---|
| `noul` | `true`, `false` | `noul` = p(true) |
| `choice` | the `criteria` keys (or codes, see `x_labels`) | `choice` = most likely key, `probabilities` for every key, `confidence` |
| `score` | levels `0` .. `n-1` | `score` = sum of `i * p_i`, `probabilities` and `legend` per level, `confidence` |

- The questions, with their instructions and criteria descriptions, form the system prompt, which is cached across
  requests. The `state` is the user message; objects and arrays are passed as indented JSON.
- Requests that arrive while the server is busy are coalesced: requests with the same questions are scored together in
  one batch.

## Quick start

Build (CUDA shown; see [docs/build.md](docs/build.md) for other backends):

```bash
cmake -B build -DGGML_CUDA=ON
cmake --build build --config Release -j --target llama-server
```

Run with a local model. `--decision-seqs` enables the decision engine and is required:

```bash
./build/bin/llama-server -m gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf -a jev-latest \
    -ngl 99 -fa on -sm none -c 32768 --decision-seqs 128 --port 8096
```

Call it with the official Python SDK (`pip install typesafe-sdk`):

```python
from typesafe_sdk import Choice, Noul, Score, TypeSafeClient

with TypeSafeClient(api_key="local", base_url="http://localhost:8096") as client:
    r = client.system_one(
        state={"message": "I was charged twice. Please fix this ASAP."},
        questions={
            "department": Choice(instructions="Which team should handle this?",
                                 criteria={"billing": "Payments and refunds", "technical": "Bugs", "other": None}),
            "refund": Noul(instructions="Does the customer ask for money back?"),
            "urgency": Score(criteria=["Can wait", "This week", "Today"]),
        },
    )
print(r.choices["department"].choice, r.choices["department"].probabilities)
```

Or with plain HTTP:

```bash
curl http://localhost:8096/v1/systemone -H "Content-Type: application/json" -d '{
  "model": "jev-latest",
  "state": "Customer reports a duplicate card charge and asks for a refund.",
  "questions": {
    "refund_requested": {"type": "noul", "instructions": "Does the customer request a refund?"},
    "department": {"type": "choice", "instructions": "Which team should handle this?",
                   "criteria": {"billing": "Payments, invoices, and refunds", "technical": "Product bugs", "other": null}},
    "urgency": {"type": "score", "instructions": "How urgent is the issue?",
                "criteria": ["Can wait", "Needs attention this week", "Needs attention today"]}
  }
}'
```

```json
{
  "model": "gemma-4-e4b",
  "answers": {
    "refund_requested": {"type": "noul", "noul": 0.9927},
    "department": {"type": "choice", "choice": "billing", "confidence": 0.9995,
                   "probabilities": {"billing": 0.99995, "technical": 0.0000025, "other": 0.000047}},
    "urgency": {"type": "score", "score": 1.704, "confidence": 0.433,
                "legend": {"0": "Can wait", "1": "Needs attention this week", "2": "Needs attention today"},
                "probabilities": {"0": 0.0035, "1": 0.289, "2": 0.707}}
  },
  "usage": {"input_tokens": 204, "output_tokens": 3}
}
```

(A real response from Gemma 4 E4B on one RTX PRO 6000, numbers shortened.)

## API

### `POST /v1/systemone`

Request and response follow the official schema: `model`, `state` (string, object or array) and a map of named
`questions`; the response has `model`, `answers` (keyed by the question names, in request order) and `usage`.

| Topic | Behaviour |
|---|---|
| `model` | The model name or any `--alias` (e.g. `-a jev-latest`). The response names the model that answered. Unknown model: 404. |
| Nested JSON | Allowed wherever the schema allows objects or arrays: `state`, `instructions`, criteria descriptions. `legend` returns the criteria as sent. |
| `confidence` | `--systemone-confidence entropy` (default: 1 - normalised entropy) or `max` (highest probability). Not TypeSafe's calibrated confidence. |
| `usage` | `input_tokens` = the whole rendered prompt, cached or not; `output_tokens` = the number of questions. |
| Validation | 422 `{"detail": [{"type", "loc", "msg", "input", "ctx"}]}` in FastAPI / pydantic form. Types, `loc` and messages were checked against pydantic run on the official SDK's generated schemas. |
| Other errors | `{"detail": "..."}`: 404 unknown model, 503 when `--decision-seqs` is missing, 401 with `--api-key` and a bad or missing key. |
| Limits (local policy) | At most 32 questions and 255 options or levels per question (422 `value_error`). |
| Unknown fields | Ignored, like pydantic's default. |

### Extensions

These are not part of the official schema and are off unless requested.

- **`x_labels`** on a `choice` question: `"names"` (default: the model writes the option name), `"letters"` (`A`..`Z`,
  at most 26 options) or `"numbers"` (`1`..`N`). With codes the prompt lists `- "A" = "billing": ...` and the model
  answers with the code; answers stay keyed by the option names. Each question picks its own mode:

  ```json
  "language": {"type": "choice", "criteria": {"NL": "Dutch", "EN": "English"}},
  "topic":    {"type": "choice", "x_labels": "letters", "criteria": {"q7mz": "Billing", "t1x9": "Technical"}}
  ```

  Measured on Gemma 4 E4B, names were never clearly worse and were better when options have no description; codes only
  matched names for opaque keys such as ids. Use `letters` for ids, long codes or awkward keys.
- **`?debug=1`** adds `x_debug` to the response: timings, batch size, cache hit, scored rows and the system prompt.

### `GET /v1/models`

Returns `models[]` with `name`, `description` and `release_date` (`YYYY-MM-DD`), one entry per model name and alias.
The values come from `--model-description` and `--model-release-date`, or else from the GGUF `general.description`
and the model file's date. The OpenAI-style `data[]` list is kept alongside, so OpenAI clients still work.

### Router mode

With `--models-preset` / `--models-dir`, the router validates `/v1/systemone` requests itself (same 422 bodies) and
forwards them to the model named in `model`. `GET /v1/models` lists every model and alias in `models[]`. Options such
as `decision-seqs`, `model-description` and `model-release-date` can be set per preset.

## Server options

| Option | Default | Meaning |
|---|---|---|
| `--decision-seqs N` | 0 (off) | Sequences reserved for decisions; required, at least 3. Size it to about `(1 + questions) x concurrent requests`. Too few splits a batch into sequential groups. |
| `--systemone-confidence` | `entropy` | `entropy` or `max`. |
| `--model-description` | GGUF `general.description` | `description` in `/v1/models`. |
| `--model-release-date` | model file date | `release_date` in `/v1/models`. |
| `-a, --alias` | | Extra model names, e.g. `jev-latest`. |
| `--api-key` | | Bearer token required on every request except `/health`. |
| `-sm none` | | On a multi-GPU machine, keeps a model that fits on one card on that card (about 12 ms faster per request). |

All other `llama-server` options work as usual; the same server also serves chat completions.

## Performance

Gemma 4 E4B QAT (Q4_K_XL) on one RTX PRO 6000, 5 questions, 304 cached prompt tokens + 33 state tokens:

| Case | Result |
|---|---|
| Warm request | 30 ms |
| New question set (prompt not cached) | 72 ms |
| 16 concurrent clients | about 110 requests/s |
| 64 concurrent clients | about 120 requests/s |

## Quality

A small, hand-labelled smoke test (not a comparison with hosted Jev): on 40 support tickets, department (4-way choice)
is 95% correct and refund (noul) 97%; urgency (4-level score) is off by 0.48 levels on average.

## Tests

```bash
cd tools/server/tests
pip install -r requirements.txt typesafe-sdk
LLAMA_SERVER_BIN_PATH=../../../build/bin/llama-server pytest unit/test_systemone.py
```

[test_systemone.py](tools/server/tests/unit/test_systemone.py) checks every response against a vendored copy of the
official OpenAPI ([fixtures/systemone-openapi.json](tools/server/tests/fixtures/systemone-openapi.json)) and covers
validation errors, `/v1/models`, auth, concurrency, router mode, `x_labels` and the official SDK. The test harness
downloads a tiny model, so the build needs HTTPS: OpenSSL, or `-DLLAMA_BUILD_BORINGSSL=ON` if OpenSSL is not installed.

## Not supported

- Multimodal `state` (images or message parts): arrays are treated as plain JSON.
- Answers that are objects: each question has one `noul`, `choice` or `score` answer. Use one question per field.
- Streaming (the official API has none either).
- Hosted-model parity: errors and answers have not been compared with the hosted API.

## Code

| Path | Content |
|---|---|
| [tools/parallel-decision/systemone.cpp](tools/parallel-decision/systemone.cpp) | Validation, prompt and answer serialisation for System One |
| [tools/parallel-decision/decision-engine.cpp](tools/parallel-decision/decision-engine.cpp) | Parallel constrained scoring engine |
| [tools/server/server-context.cpp](tools/server/server-context.cpp) | `/v1/systemone` route, request coalescing, `/v1/models` fields |
| [tools/parallel-decision/README.md](tools/parallel-decision/README.md) | Engine and `/v1/decision` documentation |

## Credits

Built on [llama.cpp](https://github.com/ggml-org/llama.cpp) and on the parallel decision engine from
[thecodacus/llama.cpp](https://github.com/thecodacus/llama.cpp) (`parallel-decision` branch). System One and its API
are by [TypeSafe AI](https://typesafe.ai); this project is not affiliated with TypeSafe AI. MIT license, see
[LICENSE](LICENSE).
