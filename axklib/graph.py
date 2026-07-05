"""Helpers for consuming axklib structured export graphs."""

import json
from collections.abc import Iterator, Mapping
from pathlib import Path

GraphInput = Mapping[str, object] | str | Path

__all__ = ["GraphInput", "iter_volume_smpl_rows", "preferred_smpl_display_name"]


def _load_graph(graph_or_path: GraphInput) -> Mapping[str, object]:
    if isinstance(graph_or_path, str | Path):
        graph = json.loads(Path(graph_or_path).read_text(encoding="utf-8"))
    else:
        graph = graph_or_path
    if not isinstance(graph, Mapping):
        raise ValueError("volume graph must be a JSON object")
    return graph


def _text_value(value: object) -> str:
    if not isinstance(value, str):
        return ""
    return value.strip()


def preferred_smpl_display_name(smpl_row: Mapping[str, object]) -> str:
    """Return the best sampler-facing display name for a physical SMPL graph row.

    Structured exports keep ``SMPL/*.wav`` paths storage-facing. When a physical
    waveform is linked from sampler-visible ``SBNK`` rows, the graph row can
    include ``user_facing_aliases``. Display-oriented consumers should prefer
    the first non-empty alias display name and use the physical ``SMPL``
    ``display_name`` only when no alias is available.

    Args:
        smpl_row: One row from ``volume.axklib.json`` ``objects.smpl``.

    Returns:
        The preferred display label, or an empty string if the graph row has no
        sampler-facing or physical display name.
    """
    aliases = smpl_row.get("user_facing_aliases")
    if isinstance(aliases, list):
        for alias in aliases:
            if not isinstance(alias, Mapping):
                continue
            display_name = _text_value(alias.get("display_name"))
            if display_name:
                return display_name
    return _text_value(smpl_row.get("display_name"))


def iter_volume_smpl_rows(graph_or_path: GraphInput) -> Iterator[Mapping[str, object]]:
    """Yield physical SMPL rows from a structured export volume graph.

    Args:
        graph_or_path: A parsed ``volume.axklib.json`` mapping or a path to one.

    Yields:
        Mapping rows from ``objects.smpl``. Non-mapping entries are ignored so
        display/index consumers can iterate defensively over graph files.

    Raises:
        ValueError: The loaded graph is not a JSON object.
        OSError: The graph path cannot be read.
        json.JSONDecodeError: The graph path does not contain valid JSON.
    """
    graph = _load_graph(graph_or_path)
    objects = graph.get("objects")
    if not isinstance(objects, Mapping):
        return
    smpl_rows = objects.get("smpl")
    if not isinstance(smpl_rows, list):
        return
    for row in smpl_rows:
        if isinstance(row, Mapping):
            yield row
