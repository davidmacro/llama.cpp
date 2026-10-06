# llama.cpp - System One

A fork of [llama.cpp](https://github.com/ggml-org/llama.cpp) whose `llama-server` serves TypeSafe AI's
[System One](https://api.typesafe.ai/docs) API itself: `POST /v1/systemone` and `GET /v1/models`, following the
official [OpenAPI](https://api.typesafe.ai/openapi.json). One process handles HTTP, validation, prompt caching and
scoring. The official SDKs talk to a local GGUF model; only the base URL changes.

> The wire format matches; the answers come from your local model. Its classifications, probabilities and
> confidence differ from TypeSafe's hosted Jev models and have their own calibration.

Branch `systemone`. The upstream llama.cpp README is kept as [README-llama.cpp.md](README-llama.cpp.md).

## Native: System One inside the inference server

Other open-source System One implementations for llama.cpp, such as
[llamacpp-jev](https://github.com/NakliTechie/llamacpp-jev) and [jev-bridge](https://github.com/TOSUKUi/jev-bridge),
run as adapters in front of an inference server and read answer probabilities through its HTTP API. Here the endpoint
is part of `llama-server`, and scoring runs inside the decode loop with direct access to the logits:

- **One process, one hop.** A request goes from the client straight to the model and back. Validation, prompt caching
  and scoring share the server's memory and the model's KV cache.
- **Complete distributions.** Every allowed value is scored from the raw logits, including options that span several
  tokens, so `probabilities` covers all options exactly. Adapters read label probabilities from the server's logprobs
  output, which lists the top-k tokens at one position.
- **All questions in one forward pass.** Questions run as parallel branches of one cached prompt, and concurrent
  requests with the same questions are batched together.
- **One deployment.** The same server, API keys, router and model presets also serve chat completions and embeddings:
  one binary to deploy, configure and upgrade.

## Built on parallel-decision by thecodacus

The core of this fork is the **parallel decision engine** by [thecodacus](https://github.com/thecodacus), from the
[`parallel-decision`](https://github.com/thecodacus/llama.cpp/tree/parallel-decision) branch of
[thecodacus/llama.cpp](https://github.com/thecodacus/llama.cpp). This branch starts from that branch unchanged
(commit `ad129b08d`) and adds System One on top.

thecodacus designed and built:

- the engine that answers a finite schema by scoring every allowed value as token paths forked from one cached
  context, all in one batched `llama_decode`, with exact constrained probabilities in tree mode
  ([decision-engine.cpp](tools/parallel-decision/decision-engine.cpp));
- `POST /v1/decision` in `llama-server` and the `--decision-seqs` option (`b9244f893`);
- the `llama-parallel-decision` CLI and the [engine README](tools/parallel-decision/README.md) (`14d04e755`);
- single-pass scoring for hybrid and recurrent models (`ad129b08d`);
- [decision-playground](https://github.com/thecodacus/decision-playground), a browser UI for the endpoint.

This fork adds the System One wire format (`/v1/systemone`, validation, `/v1/models` fields, router support),
request coalescing, the `x_labels` extension, two small engine changes (a per-field terminator and `make_input()`), and
the tests and measurements below. The speed and exact probabilities described here come from thecodacus's engine.

## How it works

Every question has a finite set of allowed answers. The server scores all allowed answers of all questions in one
batched forward pass, forked from one cached prompt, using thecodacus's parallel decision engine in
[tools/parallel-decision](tools/parallel-decision). Each answer carries the exact probability distribution over its
allowed values, normalised over those values only.

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
| `confidence` | `--systemone-confidence entropy` (default: 1 - normalised entropy) or `max` (highest probability). A local, uncalibrated statistic. |
| `usage` | `input_tokens` = the whole rendered prompt, cached tokens included; `output_tokens` = the number of questions. |
| Validation | 422 `{"detail": [{"type", "loc", "msg", "input", "ctx"}]}` in FastAPI / pydantic form. Types, `loc` and messages were checked against pydantic run on the official SDK's generated schemas. |
| Other errors | `{"detail": "..."}`: 404 unknown model, 503 when `--decision-seqs` is missing, 401 with `--api-key` and a bad or missing key. |
| Limits (local policy) | At most 32 questions and 255 options or levels per question (422 `value_error`). |
| Unknown fields | Ignored, like pydantic's default. |

### Extensions

These extend the official schema and are opt-in.

- **`x_labels`** on a `choice` question: `"names"` (default: the model writes the option name), `"letters"` (`A`..`Z`,
  at most 26 options) or `"numbers"` (`1`..`N`). With codes the prompt lists `- "A" = "billing": ...` and the model
  answers with the code; answers stay keyed by the option names. Each question picks its own mode:

  ```json
  "language": {"type": "choice", "criteria": {"NL": "Dutch", "EN": "English"}},
  "topic":    {"type": "choice", "x_labels": "letters", "criteria": {"q7mz": "Billing", "t1x9": "Technical"}}
  ```

  Use `letters` for ids, long codes or awkward keys.
- **`x_layout`** on the request (server default: `--systemone-layout`), one of four prompt layouts. All of them score
  every question in one batched pass; they differ in what the model sees before each answer.

  | Layout | What each question sees |
  |---|---|
  | `questions-first` (default) | all questions in one cached system prompt; answered as the first key of one JSON object |
  | `catalog` | the same, answered as `{"question": name, "answer": value}`; option keys shared between questions written as `"language.other"` |
  | `state-first` | the state, then only its own question (as llamacpp-jev lays it out) |
  | `state-first-context` | as `state-first`, plus one line naming the other questions |

- **`x_images`** and **`x_shared_images`** on the request: lists (at most 16 each) of images as `data:image/<type>;base64,...`
  URIs or raw base64. Needs a vision model started with `--mmproj` (otherwise 400). The images open the user turn:
  `x_shared_images` first, as part of the cached prefix (reference images, logos, examples; requests with the same
  shared images share the cache and are batched together), then the request's own `x_images`, then the state. This
  holds for every layout. A bad reference gets a 422 at `["body", "x_images", i]`. http(s) and `file://` references
  are refused unless the server runs with `--systemone-media-urls`. `usage.input_tokens` includes the image tokens.
- **`?debug=1`** adds `x_debug` to the response: timings, batch size, cache hit, scored rows, image tokens (`shared_image_tokens`, `context_image_tokens`), the system prompt, the full rendered prompt (`prompt_prefix` + `prompt_state`, images shown as media markers) and per question the text it is scored after and its candidates (`fields`).

### `GET /v1/models`

Returns `models[]` with `name`, `description` and `release_date` (`YYYY-MM-DD`), one entry per model name and alias.
The values come from `--model-description` and `--model-release-date`, or else from the GGUF `general.description`
and the model file's date. The OpenAI-style `data[]` list stays alongside for OpenAI clients.

### Router mode

With `--models-preset` / `--models-dir`, the router validates `/v1/systemone` requests itself (same 422 bodies) and
forwards them to the model named in `model`. `GET /v1/models` lists every model and alias in `models[]`. Options such
as `decision-seqs`, `model-description` and `model-release-date` can be set per preset.

## Server options

| Option | Default | Meaning |
|---|---|---|
| `--decision-seqs N` | 0 (off) | Sequences reserved for decisions; required, at least 3. Size it to about `(1 + questions) x concurrent requests`. Too few splits a batch into sequential groups. |
| `--systemone-confidence` | `entropy` | `entropy` or `max`. |
| `--systemone-layout` | `questions-first` | Default prompt layout: `questions-first` or `state-first` (see `x_layout`). |
| `--systemone-media-urls` | off | Also accept http(s) image URLs (fetched by the server) and `file://` references (with `--media-path`) in `x_images` / `x_shared_images`. Lets clients make the server fetch URLs (SSRF): enable only for trusted clients. |
| `--model-description` | GGUF `general.description` | `description` in `/v1/models`. |
| `--model-release-date` | model file date | `release_date` in `/v1/models`. |
| `-a, --alias` | | Extra model names, e.g. `jev-latest`. |
| `--api-key` | | Bearer token required on every request except `/health`. |
| `-sm none` | | On a multi-GPU machine, keeps a model that fits on one card on that card (about 12 ms faster per request). |

All other `llama-server` options work as usual; the same server also serves chat completions.

## Evaluation: this fork vs llamacpp-jev

Qwen3.8-27B (UD-Q5_K_XL) on one RTX PRO 6000, served by this fork; llamacpp-jev (`38e5d4c`) connects to the same
server. The test set has 441 hair-salon customer comments: 63 comments in each of Dutch, Afrikaans, English, Spanish,
Portuguese, Romanian and Italian, in a balanced design (L9 orthogonal array x 7 variants). Each comment is rated on
four aspects, price, quality, speed and customer service, as good, bad or not mentioned: 1764 judgements per method.

| Method | Overall accuracy | Latency per comment (p50) |
|---|---|---|
| This fork, `questions-first` (default) | **0.985** | **150 ms** |
| This fork, `catalog` | **0.985** | 167 ms |
| This fork, `state-first` | 0.967 | 335 ms |
| llamacpp-jev | 0.972 | 1370 ms |

## Tests

```bash
cd tools/server/tests
pip install -r requirements.txt typesafe-sdk
LLAMA_SERVER_BIN_PATH=../../../build/bin/llama-server pytest unit/test_systemone.py
```

[test_systemone.py](tools/server/tests/unit/test_systemone.py) checks every response against a vendored copy of the
official OpenAPI ([fixtures/systemone-openapi.json](tools/server/tests/fixtures/systemone-openapi.json)) and covers
validation errors, `/v1/models`, auth, concurrency, router mode, `x_labels` and the official SDK. The test harness
downloads a tiny model, so the build needs HTTPS: build with OpenSSL, or with `-DLLAMA_BUILD_BORINGSSL=ON` to fetch
BoringSSL.

## Limitations

- A multimodal `state` (images, message parts) is read as plain JSON.
- Each question has one `noul`, `choice` or `score` answer. For structured output, ask one question per field.
- Responses arrive in one piece, as in the official API.
- Errors and answers are checked against the OpenAPI and the official SDK; a comparison with the hosted API is still
  open.

## Code

| Path | Content |
|---|---|
| [tools/parallel-decision/systemone.cpp](tools/parallel-decision/systemone.cpp) | Validation, prompt and answer serialisation for System One |
| [tools/parallel-decision/decision-engine.cpp](tools/parallel-decision/decision-engine.cpp) | Parallel constrained scoring engine |
| [tools/server/server-context.cpp](tools/server/server-context.cpp) | `/v1/systemone` route, request coalescing, `/v1/models` fields |
| [tools/parallel-decision/README.md](tools/parallel-decision/README.md) | Engine and `/v1/decision` documentation |

## Credits

- **Parallel decision engine, `/v1/decision`, `llama-parallel-decision`:** [thecodacus](https://github.com/thecodacus),
  [thecodacus/llama.cpp @ parallel-decision](https://github.com/thecodacus/llama.cpp/tree/parallel-decision). See
  [Built on parallel-decision by thecodacus](#built-on-parallel-decision-by-thecodacus).
- **llama.cpp:** [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) and its contributors.
- **System One API:** [TypeSafe AI](https://typesafe.ai). This fork is independent of TypeSafe AI.

MIT license, see [LICENSE](LICENSE). Commit history keeps each author's work under their own name.
