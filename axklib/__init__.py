"""Public axklib library API."""

from axklib.alteration import (
    AlterationManifest,
    AlterationResult,
    InsertSampleBankSpec,
    alter_hds,
    load_alteration_manifest,
    parse_alteration_manifest,
)
from axklib.containers import OpenOptions, open, open_many
from axklib.containers.sfs_allocation import SfsFreeSpace, calculate_sfs_free_space

__all__ = [
    "OpenOptions",
    "SfsFreeSpace",
    "AlterationManifest",
    "AlterationResult",
    "InsertSampleBankSpec",
    "alter_hds",
    "calculate_sfs_free_space",
    "load_alteration_manifest",
    "open",
    "open_many",
    "parse_alteration_manifest",
]
