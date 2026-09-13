import copy
import json
from pathlib import Path

import pytest

from generate_current_effects import render

DATA = Path(__file__).resolve().parents[3] / "library/data/current-effects.json"


def test_emits_checked_numeric_domains_and_complete_reset_vectors() -> None:
    data = json.loads(DATA.read_text())
    output = render(data)
    assert "effect_write_types" in output
    assert "35082" in output
    assert "1800" in output
    assert len(data["write_types"]) == 97


def test_unused_display_slots_are_not_exposed_as_writable_parameters() -> None:
    data = json.loads(DATA.read_text())
    for row in data["effect_parameters"]:
        if row["raw_type"] < 97 and row["value_label_source"] == "EMPTY_VALUE":
            domain = data["write_types"][row["raw_type"]]["parameters"][row["parameter_number"] - 1]
            assert domain["kind"] == "unused", (row["raw_type"], row["parameter_number"])


def test_emits_native_physical_reset_vectors_separately_from_current_defaults() -> None:
    data = json.loads(DATA.read_text())
    vectors = data["a3000_reset_words"]
    assert len(vectors) == 55
    assert all(len(words) == 16 for words in vectors)
    assert vectors[1][15] == 0
    assert vectors[54][15] == 35082
    assert "a3000_effect_reset_words" in render(data)


@pytest.mark.parametrize("mutation", ["missing", "short", "negative", "overflow", "boolean", "domain"])
def test_rejects_invalid_native_reset_vectors(mutation: str) -> None:
    data = json.loads(DATA.read_text())
    vectors = data["a3000_reset_words"]
    if mutation == "missing":
        vectors.pop()
    elif mutation == "short":
        vectors[1].pop()
    elif mutation == "negative":
        vectors[1][0] = -1
    elif mutation == "overflow":
        vectors[1][0] = 65536
    elif mutation == "boolean":
        vectors[1][0] = True
    else:
        vectors[1][1] = 0
    with pytest.raises(ValueError):
        render(data)


@pytest.mark.parametrize("mutation", ["missing_word", "negative", "overflow", "bad_kind", "unknown", "duplicate"])
def test_rejects_malformed_write_tables(mutation: str) -> None:
    data = json.loads(DATA.read_text())
    row = data["write_types"][1]
    if mutation == "missing_word":
        row["reset_words"].pop()
    elif mutation == "negative":
        row["parameters"][0]["minimum"] = -1
    elif mutation == "overflow":
        row["reset_words"][0] = 65536
    elif mutation == "bad_kind":
        row["parameters"][0]["kind"] = "runtime"
    elif mutation == "unknown":
        row["extra"] = 1
    else:
        data["write_types"][2] = copy.deepcopy(row)
    with pytest.raises(ValueError):
        render(data)
