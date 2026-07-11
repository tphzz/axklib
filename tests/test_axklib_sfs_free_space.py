from __future__ import annotations

import pytest

from axklib import calculate_sfs_free_space


def test_sfs_free_space_matches_a4000_capped_partition_display() -> None:
    result = calculate_sfs_free_space(
        cluster_count=1_048_575,
        first_payload_cluster=616,
        allocated_cluster_count=65,
    )

    assert result.total_cluster_count == 1_048_575
    assert result.reserved_cluster_count == 616
    assert result.allocated_cluster_count == 65
    assert result.free_cluster_count == 1_047_894
    assert result.free_bytes == 1_073_043_456
    assert result.sampler_visible_free_kib == 1_047_894


def test_sfs_free_space_converts_nondefault_cluster_size_to_kib() -> None:
    result = calculate_sfs_free_space(
        cluster_count=100,
        first_payload_cluster=10,
        allocated_cluster_count=20,
        cluster_size_bytes=2048,
    )

    assert result.free_cluster_count == 70
    assert result.free_bytes == 143_360
    assert result.sampler_visible_free_kib == 140


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"cluster_count": -1, "first_payload_cluster": 0, "allocated_cluster_count": 0}, "cluster_count"),
        ({"cluster_count": 10, "first_payload_cluster": 11, "allocated_cluster_count": 0}, "first_payload_cluster"),
        ({"cluster_count": 10, "first_payload_cluster": 2, "allocated_cluster_count": 9}, "allocated_cluster_count"),
        ({"cluster_count": 10, "first_payload_cluster": 2, "allocated_cluster_count": 1, "cluster_size_bytes": 0}, "cluster_size_bytes"),
    ],
)
def test_sfs_free_space_rejects_invalid_inputs(
    kwargs: dict[str, int], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        calculate_sfs_free_space(**kwargs)
