"""Public axklib library API."""

from axklib.alteration import (
    AlterationManifest,
    AlterationResult,
    AudioImportSummary,
    InsertProgramAssignmentSpec,
    InsertProgramSpec,
    InsertSampleBankGroupSpec,
    InsertSampleBankSpec,
    InsertWaveformSpec,
    alter_hds,
    load_alteration_manifest,
    parse_alteration_manifest,
)
from axklib.containers import OpenOptions, open, open_many
from axklib.containers.sfs_allocation import SfsFreeSpace, calculate_sfs_free_space
from axklib.waveform_orphans import (
    WaveformOrphanReport,
    WaveformOrphanRow,
    WaveformOrphanSummary,
    analyze_hds_waveform_orphans,
)

__all__ = [
    "OpenOptions",
    "SfsFreeSpace",
    "AlterationManifest",
    "AlterationResult",
    "AudioImportSummary",
    "InsertProgramAssignmentSpec",
    "InsertProgramSpec",
    "InsertSampleBankGroupSpec",
    "InsertSampleBankSpec",
    "InsertWaveformSpec",
    "WaveformOrphanReport",
    "WaveformOrphanRow",
    "WaveformOrphanSummary",
    "alter_hds",
    "analyze_hds_waveform_orphans",
    "calculate_sfs_free_space",
    "load_alteration_manifest",
    "open",
    "open_many",
    "parse_alteration_manifest",
]
