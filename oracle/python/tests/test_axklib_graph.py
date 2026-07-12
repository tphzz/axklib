import json
from pathlib import Path

import pytest

from axklib.graph import iter_volume_smpl_rows, preferred_smpl_display_name


def test_preferred_smpl_display_name_uses_first_alias() -> None:
    assert (
        preferred_smpl_display_name(
            {
                "display_name": "JP6 FatBs1b036 *",
                "wav_path": "SMPL/JP6 FatBs1b036 (2).wav",
                "user_facing_aliases": [
                    {"display_name": "J FatBs1b036"},
                    {"display_name": "JP6 FatBs1b036 *"},
                ],
            }
        )
        == "J FatBs1b036"
    )


def test_preferred_smpl_display_name_falls_back_to_physical_display_name() -> None:
    assert (
        preferred_smpl_display_name(
            {
                "display_name": "sine wave      *",
                "wav_path": "SMPL/sine wave (2).wav",
                "user_facing_aliases": [],
            }
        )
        == "sine wave      *"
    )


def test_preferred_smpl_display_name_does_not_use_wav_path_as_label() -> None:
    assert preferred_smpl_display_name({"wav_path": "SMPL/sine wave (2).wav"}) == ""


def test_iter_volume_smpl_rows_reads_parsed_graph() -> None:
    first = {"display_name": "One", "wav_path": "SMPL/One.wav"}
    second = {"display_name": "Two", "wav_path": "SMPL/Two.wav"}
    graph = {"objects": {"smpl": [first, "bad row", second]}}

    assert list(iter_volume_smpl_rows(graph)) == [first, second]


def test_iter_volume_smpl_rows_reads_graph_path(tmp_path: Path) -> None:
    graph_path = tmp_path / "volume.axklib.json"
    graph_path.write_text(
        json.dumps({"objects": {"smpl": [{"display_name": "From File"}]}}),
        encoding="utf-8",
    )

    assert list(iter_volume_smpl_rows(graph_path)) == [{"display_name": "From File"}]


def test_iter_volume_smpl_rows_reads_selection_graph_array(tmp_path: Path) -> None:
    graph_path = tmp_path / "selection.axklib.json"
    graph_path.write_text(
        json.dumps(
            [
                {"objects": {"smpl": [{"display_name": "One"}]}},
                {"objects": {"smpl": [{"display_name": "Two"}]}},
            ]
        ),
        encoding="utf-8",
    )

    assert list(iter_volume_smpl_rows(graph_path)) == [
        {"display_name": "One"},
        {"display_name": "Two"},
    ]


def test_iter_volume_smpl_rows_rejects_non_object_graph(tmp_path: Path) -> None:
    graph_path = tmp_path / "volume.axklib.json"
    graph_path.write_text("[1]", encoding="utf-8")

    with pytest.raises(ValueError, match="graph must be a JSON object"):
        list(iter_volume_smpl_rows(graph_path))
