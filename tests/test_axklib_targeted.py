from __future__ import annotations

from pathlib import Path

from axklib.audio import Waveform, WaveformRelationship
from axklib.audio.structured import SampleExportTarget, WaveExportPlan
from axklib.model import AxklibQuality, DataQuality
from axklib.targeted import TargetedExportRequest, _filter_plan_targets, export_targeted


def _waveform(object_key: str) -> Waveform:
    return Waveform(
        object_key=object_key,
        source_image="source.iso",
        partition_index=None,
        object_offset=None,
        container_kind="iso",
        object_type="SMPL",
        sample_name="sample",
        pcm=b"\x00\x00",
        channel_count=1,
        sample_width_bytes=2,
        stored_sample_width_bytes=2,
        sample_rate=44100,
        frame_count=1,
        stored_payload_size=2,
        stored_payload_transform="none",
        alternating_byte_payload_detected=False,
        root_key=None,
        fine_tune=None,
        loop_mode=None,
        loop_mode_label="",
        loop_start=None,
        loop_length=None,
        loop_end_a4000_ui=None,
        exactness_status="exact",
        quality=AxklibQuality(DataQuality.KNOWN, "test"),
    )


def test_program_target_filter_keeps_likely_sbac_members() -> None:
    plan = WaveExportPlan(
        output_dir=Path("out"),
        targets=(
            SampleExportTarget(
                stable_id="target",
                waveform=_waveform("smpl-1"),
                relative_wav_path=Path("SMPL/sample.wav"),
                relative_json_path=Path("SMPL/sample.json"),
                placement=None,
                placement_quality=DataQuality.LIKELY,
                placement_source="test",
                sampler_sample_key="sbnk-1",
                sampler_sample_name="Sample",
                sample_bank_key="sbac-1",
                sample_bank_name="B Bank",
            ),
        ),
        relationships=(
            WaveformRelationship(
                source_key="prog-1",
                target_key="sbac-1",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Likely",
                basis="test",
                active_assignment_state="confirmed-active",
            ),
            WaveformRelationship(
                source_key="sbac-1",
                target_key="sbnk-1",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Likely",
                basis="test",
            ),
        ),
    )

    targets = _filter_plan_targets(plan, "program", "Volume/Programs/001", "prog-1")

    assert [target.stable_id for target in targets] == ["target"]

def test_targeted_export_selection_graph_is_opt_in(tmp_path: Path) -> None:
    plan = WaveExportPlan(
        output_dir=tmp_path,
        targets=(
            SampleExportTarget(
                stable_id="target",
                waveform=_waveform("smpl-1"),
                relative_wav_path=Path("SMPL/sample.wav"),
                relative_json_path=Path("SMPL/sample.json"),
                placement=None,
                placement_quality=DataQuality.KNOWN,
                placement_source="test",
                sampler_sample_key="sbnk-1",
                sampler_sample_name="Sample",
            ),
        ),
    )

    default_out = tmp_path / "default"
    default_result = export_targeted(
        TargetedExportRequest(output_dir=default_out, plan=plan, scope="file")
    )
    opt_in_out = tmp_path / "opt-in"
    opt_in_result = export_targeted(
        TargetedExportRequest(output_dir=opt_in_out, plan=plan, scope="file", write_graphs=True)
    )

    assert default_result.selection_graphs
    assert not (default_out / "file" / "selection.axklib.json").exists()
    assert (opt_in_out / "file" / "selection.axklib.json").exists()
    assert opt_in_result.selection_graphs
