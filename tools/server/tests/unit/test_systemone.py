# POST /v1/systemone and GET /v1/models against TypeSafe's System One contract.
# fixtures/systemone-openapi.json: https://api.typesafe.ai/openapi.json, retrieved 2026-09-27.

import json
import math
import os
import re

import pytest
from utils import *

server = ServerPreset.tinyllama2()

SPEC_PATH = os.path.join(os.path.dirname(__file__), "..", "fixtures", "systemone-openapi.json")
with open(SPEC_PATH, encoding="utf-8") as f:
    SPEC = json.load(f)
SCHEMAS = SPEC["components"]["schemas"]


def check(schema: dict, value, path="$"):
    """Minimal OpenAPI 3.1 checker for the constructs the System One schemas use."""
    if "$ref" in schema:
        return check(SCHEMAS[schema["$ref"].split("/")[-1]], value, path)
    if "oneOf" in schema:
        disc = schema["discriminator"]
        tag = value.get(disc["propertyName"]) if isinstance(value, dict) else None
        assert tag in disc["mapping"], f"{path}: bad tag {tag!r}"
        return check({"$ref": disc["mapping"][tag]}, value, path)
    if "anyOf" in schema:
        errors = []
        for sub in schema["anyOf"]:
            try:
                return check(sub, value, path)
            except AssertionError as e:
                errors.append(str(e))
        raise AssertionError(f"{path}: no anyOf branch matched: {errors}")
    types = {"string": str, "object": dict, "array": list, "null": type(None), "boolean": bool}
    t = schema.get("type")
    if t == "number":
        assert isinstance(value, (int, float)) and not isinstance(value, bool), f"{path}: not a number"
    elif t == "integer":
        assert isinstance(value, int) and not isinstance(value, bool), f"{path}: not an integer"
    elif t is not None:
        assert isinstance(value, types[t]), f"{path}: not {t}"
    if "const" in schema:
        assert value == schema["const"], f"{path}: expected {schema['const']!r}"
    if t == "object":
        for key in schema.get("required", []):
            assert key in value, f"{path}: missing {key}"
        props = schema.get("properties", {})
        extra = schema.get("additionalProperties")
        for key, v in value.items():
            if key in props:
                check(props[key], v, f"{path}.{key}")
            elif isinstance(extra, dict):
                check(extra, v, f"{path}.{key}")
        assert len(value) >= schema.get("minProperties", 0), f"{path}: too few properties"
    if t == "array":
        assert len(value) >= schema.get("minItems", 0), f"{path}: too few items"
        for i, v in enumerate(value):
            check(schema.get("items", {}), v, f"{path}[{i}]")


EXAMPLE = {
    "model": "tinyllama-2",
    "state": "Customer reports a duplicate card charge and asks for a refund.",
    "questions": {
        "refund_requested": {"type": "noul", "instructions": "Does the customer request a refund?"},
        "department": {
            "type": "choice",
            "instructions": "Which team should handle this?",
            "criteria": {"billing": "Payments, invoices, and refunds", "technical": "Product bugs", "other": None},
        },
        "urgency": {
            "type": "score",
            "instructions": "How urgent is the issue?",
            "criteria": ["Can wait", "Needs attention this week", "Needs attention today"],
        },
    },
}


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.decision_seqs = 8
    server.n_ctx = 2048


def check_answers(req: dict, body: dict):
    check({"$ref": "#/components/schemas/SystemOneResponse"}, body)
    assert set(body) == {"model", "answers", "usage"}
    assert list(body["answers"]) == list(req["questions"])
    for name, q in req["questions"].items():
        a = body["answers"][name]
        assert a["type"] == q["type"]
        if q["type"] == "noul":
            assert set(a) == {"type", "noul"}
            assert 0 <= a["noul"] <= 1
            continue
        probs = a["probabilities"]
        assert math.isclose(sum(probs.values()), 1.0, abs_tol=1e-6)
        assert all(0 <= p <= 1 for p in probs.values())
        assert 0 <= a["confidence"] <= 1
        if q["type"] == "choice":
            assert list(probs) == list(q["criteria"])
            assert a["choice"] == max(probs, key=probs.get)
        else:
            keys = [str(i) for i in range(len(q["criteria"]))]
            assert list(probs) == keys and list(a["legend"]) == keys
            assert list(a["legend"].values()) == q["criteria"]
            assert math.isclose(a["score"], sum(i * probs[k] for i, k in enumerate(keys)), abs_tol=1e-9)
    assert body["usage"]["output_tokens"] == len(req["questions"])
    assert body["usage"]["input_tokens"] > 0


def test_systemone_example():
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=EXAMPLE)
    assert res.status_code == 200, res.body
    assert res.body["model"] == "tinyllama-2"
    check_answers(EXAMPLE, res.body)


def test_systemone_structured_values():
    global server
    server.start()
    req = {
        "model": "tinyllama-2",
        "state": {"subject": "Duplicate charge", "message": "Please help.", "lines": [1, 2]},
        "questions": {
            "only": {"type": "choice", "criteria": {"yes": {"when": "always"}}},
            "spam": {"type": "noul", "instructions": {"task": "Is this spam?"}, "criteria": {"true": ["ads"], "false": None}},
            "wide": {"type": "choice", "instructions": None, "criteria": {"a": None, "ab": None, "abc": None, "b\"q": None, "üñí": None}},
            "levels": {"type": "score", "criteria": [str(i) for i in range(12)]},
            "bare": {"type": "noul"},
        },
    }
    res = server.make_request("POST", "/v1/systemone", data=req)
    assert res.status_code == 200, res.body
    check_answers(req, res.body)
    assert res.body["answers"]["only"]["probabilities"] == {"yes": 1.0}
    assert res.body["answers"]["only"]["confidence"] == 1.0


def test_systemone_debug_is_opt_in():
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone?debug=1", data=EXAMPLE)
    assert res.status_code == 200
    assert "x_debug" in res.body and res.body["x_debug"]["batch_size"] >= 1


def test_systemone_cached_prefix_gives_same_answers():
    global server
    server.start()
    a = server.make_request("POST", "/v1/systemone", data=EXAMPLE)
    b = server.make_request("POST", "/v1/systemone?debug=1", data=EXAMPLE)
    assert b.body["x_debug"]["cache_hit"]
    for name, ans in a.body["answers"].items():
        for key in ("noul", "score", "confidence"):
            if key in ans:
                assert math.isclose(ans[key], b.body["answers"][name][key], abs_tol=1e-3)


def test_systemone_concurrent_requests():
    global server
    server.n_slots = 4
    server.start()
    states = [f"Ticket {i}: I was charged {i} times, fix it today." for i in range(8)]
    reqs = [dict(EXAMPLE, state=s) for s in states]
    results = parallel_function_calls([
        (server.make_request, ("POST", "/v1/systemone?debug=1", r)) for r in reqs
    ])
    for req, res in zip(reqs, results):
        assert res.status_code == 200, res.body
        body = dict(res.body)
        body.pop("x_debug")
        check_answers(req, body)


def test_systemone_mixed_question_sets():
    global server
    server.start()
    other = {"model": "tinyllama-2", "state": "hello", "questions": {"greeting": {"type": "noul"}}}
    results = parallel_function_calls([
        (server.make_request, ("POST", "/v1/systemone", EXAMPLE if i % 2 else other)) for i in range(6)
    ])
    for i, res in enumerate(results):
        assert res.status_code == 200, res.body
        check_answers(EXAMPLE if i % 2 else other, res.body)


def test_systemone_confidence_max():
    global server
    server.systemone_confidence = "max"
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=EXAMPLE)
    a = res.body["answers"]["department"]
    assert math.isclose(a["confidence"], max(a["probabilities"].values()), abs_tol=1e-9)


def detail(res) -> list:
    assert res.status_code == 422, res.body
    check({"$ref": "#/components/schemas/HTTPValidationError"}, res.body)
    return res.body["detail"]


@pytest.mark.parametrize("body,expected", [
    ({}, [("missing", ["body", "state"]), ("missing", ["body", "model"]), ("missing", ["body", "questions"])]),
    ([1], [("model_attributes_type", ["body"])]),
    ({"model": 1, "state": "x", "questions": {}}, [("string_type", ["body", "model"]), ("too_short", ["body", "questions"])]),
    ({"model": "m", "state": 3, "questions": {"q": {"type": "noul"}}}, [
        ("string_type", ["body", "state", "str"]),
        ("dict_type", ["body", "state", "dict[str,any]"]),
        ("list_type", ["body", "state", "list[any]"]),
    ]),
    ({"model": "m", "state": "x", "questions": {"q": {"type": "rating"}}}, [("union_tag_invalid", ["body", "questions", "q"])]),
    ({"model": "m", "state": "x", "questions": {"q": {}}}, [("union_tag_not_found", ["body", "questions", "q"])]),
    ({"model": "m", "state": "x", "questions": {"urgency": {"type": "score"}}}, [("missing", ["body", "questions", "urgency", "score", "criteria"])]),
    ({"model": "m", "state": "x", "questions": {"u": {"type": "score", "criteria": []}}}, [("too_short", ["body", "questions", "u", "score", "criteria"])]),
    ({"model": "m", "state": "x", "questions": {"c": {"type": "choice", "criteria": ["a"]}}}, [("dict_type", ["body", "questions", "c", "choice", "criteria"])]),
    ({"model": "m", "state": "x", "questions": {"c": {"type": "choice", "criteria": {}}}}, [("value_error", ["body", "questions", "c", "choice", "criteria"])]),
    ({"model": "m", "state": "x", "questions": {"n": {"type": "noul", "criteria": "yes"}}}, [("model_type", ["body", "questions", "n", "noul", "criteria"])]),
    ({"model": "m", "state": "x", "questions": {f"q{i}": {"type": "noul"} for i in range(33)}}, [("value_error", ["body", "questions"])]),
])
def test_systemone_validation(body, expected):
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=body)
    assert [(e["type"], e["loc"]) for e in detail(res)] == expected


def test_systemone_invalid_json():
    global server
    server.start()
    for raw, kind in [("{\"model\": ", "json_invalid"), ("", "missing")]:
        res = requests.post(f"http://{server.server_host}:{server.server_port}/v1/systemone", data=raw,
                            headers={"Content-Type": "application/json"})
        res.body = res.json()
        assert detail(res)[0]["type"] == kind


def test_systemone_unknown_model():
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=dict(EXAMPLE, model="nope"))
    assert res.status_code == 404
    assert isinstance(res.body["detail"], str)


def test_systemone_disabled():
    global server
    server.decision_seqs = None
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=EXAMPLE)
    assert res.status_code == 503
    assert "--decision-seqs" in res.body["detail"]


def test_systemone_api_key():
    global server
    server.api_key = "secret"
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=EXAMPLE)
    assert res.status_code == 401
    res = server.make_request("POST", "/v1/systemone", data=EXAMPLE, headers={"Authorization": "Bearer secret"})
    assert res.status_code == 200
    res = server.make_request("GET", "/v1/models", headers={"Authorization": "Bearer secret"})
    assert res.status_code == 200


def test_models_systemone_fields():
    global server
    server.model_alias = "tinyllama-2,jev-latest"
    server.start()
    res = server.make_request("GET", "/v1/models")
    assert res.status_code == 200
    check({"$ref": "#/components/schemas/ModelMetadataList"}, res.body)
    names = [m["name"] for m in res.body["models"]]
    # the name is the first alias in sorted order, the rest are listed after it
    assert names == ["jev-latest", "tinyllama-2"]
    for m in res.body["models"]:
        assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", m["release_date"])
        assert m["description"]
    assert res.body["data"][0]["id"] == "jev-latest"  # OpenAI listing unchanged
    res = server.make_request("POST", "/v1/systemone", data=dict(EXAMPLE, model="tinyllama-2"))
    assert res.status_code == 200
    assert res.body["model"] == "jev-latest"


def test_router_systemone():
    router = ServerPreset.router()
    router.decision_seqs = 8
    router.start()
    model = "ggml-org/tinygemma3-GGUF:Q8_0"

    res = router.make_request("GET", "/v1/models")
    assert res.status_code == 200
    check({"$ref": "#/components/schemas/ModelMetadataList"}, res.body)
    assert model in [m["name"] for m in res.body["models"]]

    res = router.make_request("POST", "/v1/systemone", data={"model": model})
    assert [(e["type"], e["loc"]) for e in detail(res)] == [("missing", ["body", "state"]), ("missing", ["body", "questions"])]

    res = router.make_request("POST", "/v1/systemone", data=dict(EXAMPLE, model="non-existent/model"))
    assert 400 <= res.status_code < 500
    assert isinstance(res.body["detail"], str)

    req = dict(EXAMPLE, model=model)
    res = router.make_request("POST", "/v1/systemone", data=req, timeout=120)
    assert res.status_code == 200, res.body
    check_answers(req, res.body)


def test_official_sdk():
    ts = pytest.importorskip("typesafe_sdk")
    global server
    server.model_alias = "jev-latest"
    server.start()
    base = f"http://{server.server_host}:{server.server_port}"
    with ts.TypeSafeClient(api_key="local", base_url=base) as client:
        assert [m.name for m in client.models.list().models] == ["jev-latest"]
        r = client.system_one(
            state={"document": "I was charged twice. Please fix this ASAP."},
            questions={
                "category": ts.Choice(instructions="What is this about?", criteria={"billing": None, "technical": None}),
                "angry": ts.Noul(criteria=ts.NoulCriteria(true="Hostile", false="Calm")),
                "urgency": ts.Score(criteria=["Can wait", "This week", "Today"]),
            },
        )
        assert r.model == "jev-latest"
        assert r.usage.output_tokens == 3
        assert r.choices["category"].choice in ("billing", "technical")
        with pytest.raises(ts.TypeSafeNotFoundError):
            client.system_one(model="nope", state="x", questions={"q": ts.Noul()})


def test_systemone_labels_per_question():
    global server
    server.start()
    req = {
        "model": "tinyllama-2",
        "state": "Ik ben twee keer afgeschreven.",
        "questions": {
            "language": {"type": "choice", "criteria": {"NL": "Dutch", "EN": "English"}},
            "topic": {"type": "choice", "x_labels": "letters", "criteria": {"billing": None, "technical": None, "other": None}},
            "priority": {"type": "choice", "x_labels": "numbers", "criteria": {f"p{i}": None for i in range(12)}},
        },
    }
    res = server.make_request("POST", "/v1/systemone?debug=1", data=req)
    assert res.status_code == 200, res.body
    body = dict(res.body)
    prompt = body.pop("x_debug")["system_prompt"]
    check_answers(req, body)  # still keyed by the option names
    assert '- "NL": Dutch' in prompt
    assert '- "A" = "billing"' in prompt and '- "C" = "other"' in prompt
    assert '- 12 = "p11"' in prompt


@pytest.mark.parametrize("question,expected", [
    ({"type": "choice", "x_labels": "roman", "criteria": {"a": None}}, "literal_error"),
    ({"type": "choice", "x_labels": 1, "criteria": {"a": None}}, "literal_error"),
    ({"type": "noul", "x_labels": "letters"}, "value_error"),
    ({"type": "score", "x_labels": "numbers", "criteria": ["a"]}, "value_error"),
    ({"type": "choice", "x_labels": "letters", "criteria": {f"k{i}": None for i in range(27)}}, "value_error"),
])
def test_systemone_labels_validation(question, expected):
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone", data={"model": "tinyllama-2", "state": "x", "questions": {"q": question}})
    errs = detail(res)
    assert [e["type"] for e in errs] == [expected]
    assert errs[0]["loc"] == ["body", "questions", "q", question["type"], "x_labels"]


def test_systemone_state_first_layout():
    global server
    server.n_batch = 512  # a state-first branch carries its whole question block
    server.start()
    req = dict(EXAMPLE, x_layout="state-first")
    res = server.make_request("POST", "/v1/systemone?debug=1", data=req)
    assert res.status_code == 200, res.body
    body = dict(res.body)
    d = body.pop("x_debug")
    check_answers(EXAMPLE, body)
    assert d["layout"] == "state-first"
    assert EXAMPLE["state"] in d["prompt_state"]
    # each branch carries only its own question and opens a one-key object
    for f in d["fields"]:
        others = [q for q in EXAMPLE["questions"] if q != f["question"]]
        assert f'"{f["question"]}" (' in f["suffix"]
        assert all(f'"{o}" (' not in f["suffix"] for o in others)
        assert f["terminator"] == "}"


def test_systemone_layout_server_default():
    global server
    server.systemone_layout = "state-first"
    server.n_batch = 512
    server.start()
    res = server.make_request("POST", "/v1/systemone?debug=1", data=EXAMPLE)
    assert res.status_code == 200, res.body
    assert res.body["x_debug"]["layout"] == "state-first"
    res = server.make_request("POST", "/v1/systemone?debug=1", data=dict(EXAMPLE, x_layout="questions-first"))
    assert res.body["x_debug"]["layout"] == "questions-first"


def test_systemone_layout_validation():
    global server
    server.start()
    res = server.make_request("POST", "/v1/systemone", data=dict(EXAMPLE, x_layout="sideways"))
    assert [(e["type"], e["loc"]) for e in detail(res)] == [("literal_error", ["body", "x_layout"])]


def test_systemone_catalog_layout():
    global server
    server.start()
    req = {
        "model": "tinyllama-2", "state": "hello", "x_layout": "catalog",
        "questions": {
            "topic": {"type": "choice", "criteria": {"billing": None, "other": None}},
            "language": {"type": "choice", "criteria": {"EN": None, "other": None}},
            "refund": {"type": "noul"},
        },
    }
    res = server.make_request("POST", "/v1/systemone?debug=1", data=req)
    assert res.status_code == 200, res.body
    body = dict(res.body)
    d = body.pop("x_debug")
    check_answers({k: v for k, v in req.items() if k != "x_layout"}, body)
    # a key shared by two questions is scoped in the prompt; answers keep the original keys
    assert '"topic.other"' in d["system_prompt"] and '"language.other"' in d["system_prompt"]
    assert list(body["answers"]["topic"]["probabilities"]) == ["billing", "other"]
    f = {x["question"]: x for x in d["fields"]}
    assert f["language"]["suffix"].startswith('  "question": "language",\n  "answer": ')
    assert 'language.other"' in f["language"]["candidates"]


def test_systemone_state_first_context_layout():
    global server
    server.n_batch = 512
    server.start()
    res = server.make_request("POST", "/v1/systemone?debug=1", data=dict(EXAMPLE, x_layout="state-first-context"))
    assert res.status_code == 200, res.body
    body = dict(res.body)
    d = body.pop("x_debug")
    check_answers(EXAMPLE, body)
    f = {x["question"]: x for x in d["fields"]}
    assert 'Asked separately about the same content: "department"' in f["refund_requested"]["suffix"]
    assert "Allowed answers:\n- \"billing\"" not in f["refund_requested"]["suffix"]  # the other questions' options stay out
