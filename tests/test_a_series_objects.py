import tempfile
import unittest
import wave
from pathlib import Path

from axklib.objects import current as a_series_objects


def put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def build_current_smpl_header() -> bytearray:
    header = bytearray(0x200)
    header[0:12] = b"FSFSDEV3SPLX"
    header[0x0C:0x10] = b"SMPL"
    put_be32(header, 0x10, 0x200)
    put_be32(header, 0x18, 0x7C)
    put_be32(header, 0x1C, 8)
    put_be32(header, 0x20, 8)
    put_be16(header, 0x28, 44100)
    put_be16(header, 0x2A, 2)
    header[0x32 : 0x32 + 4] = b"TEST"
    header[0x54 : 0x54 + 10] = b"Wave Name "
    put_be32(header, 0x6C, 0x016B1E02)
    put_be32(header, 0x78, 0x016B1EC4)
    put_be16(header, 0x7C, 44100)
    header[0x7E] = 60
    header[0x7F] = 0xFB
    header[0x85] = 4
    put_be32(header, 0x92, 1000)
    put_be32(header, 0x96, 100)
    put_be32(header, 0x9A, 400)
    return header


class ASeriesObjectsTests(unittest.TestCase):
    def test_decodes_typed_current_smpl_metadata(self) -> None:
        metadata = a_series_objects.decode_current_smpl_metadata(bytes(build_current_smpl_header()))

        self.assertEqual(metadata.sample_rate_duplicate_0x07c, 44100)
        self.assertEqual(metadata.root_key_midi_note_guess, 60)
        self.assertEqual(metadata.fine_tune_cents_guess, -5)
        self.assertEqual(metadata.loop_end_frame_a4000_ui_guess, 500)
        self.assertEqual(metadata.loop_mode_a4000_ui_label_guess, "One->")

    def test_summarizes_current_smpl_compact_playback_metadata(self) -> None:
        row = a_series_objects.summarize_object_header(bytes(build_current_smpl_header()))

        self.assertEqual(row["sample_rate_duplicate_0x07c"], 44100)
        self.assertEqual(row["root_key_midi_note_guess"], 60)
        self.assertEqual(row["fine_tune_cents_guess"], -5)
        self.assertEqual(row["wave_length_frames_0x092"], 1000)
        self.assertEqual(row["loop_start_frame_0x096"], 100)
        self.assertEqual(row["loop_length_frames_0x09a"], 400)
        self.assertEqual(row["loop_end_frame_inclusive_guess"], 499)
        self.assertEqual(row["loop_end_frame_exclusive_guess"], 500)
        self.assertEqual(row["loop_end_frame_a4000_ui_guess"], 500)
        self.assertEqual(row["loop_mode_candidate_0x085"], 4)
        self.assertEqual(row["loop_mode_a4000_ui_label_guess"], "One->")
        self.assertEqual(row["smpl_group_id_0x06c"], 0x016B1E02)
        self.assertEqual(row["smpl_link_id_0x078"], 0x016B1EC4)
        self.assertEqual(row["source_wave_name_guess"], "Wave Name")
        self.assertNotIn("smpl_sbnk_mirror_field_0x098", row)
        self.assertNotIn("smpl_sbnk_mirror_field_0x09c", row)

    def test_wav_sidecar_includes_current_smpl_metadata_and_quality(self) -> None:
        row = a_series_objects.summarize_object_header(bytes(build_current_smpl_header()))
        payload = b"\x00\x01\x00\x02\x00\x03\x00\x04"

        with tempfile.TemporaryDirectory() as tmp:
            metadata = a_series_objects.write_current_smpl_wav(
                row=row,
                output_dir=Path(tmp),
                stem_prefix="fixture",
                read_stored_payload=lambda rel_offset, size: payload[:size],
                source_container="fixture.sfs",
                container_kind="test",
            )

            self.assertEqual(metadata["root_key_midi_note_guess"], 60)
            self.assertEqual(metadata["fine_tune_cents_guess"], -5)
            self.assertEqual(metadata["loop_start_frame_0x096"], 100)
            self.assertEqual(metadata["loop_end_frame_inclusive_guess"], 499)
            self.assertEqual(metadata["loop_end_frame_a4000_ui_guess"], 500)
            self.assertEqual(metadata["loop_mode_a4000_ui_label_guess"], "One->")
            for field in (
                "root_key_midi_note_guess",
                "fine_tune_cents_guess",
                "loop_start_frame_0x096",
                "loop_length_frames_0x09a",
                "smpl_link_id_0x078",
            ):
                self.assertIn(field, metadata["field_quality"])
                self.assertIn("quality", metadata["field_quality"][field])
            self.assertNotIn("smpl_sbnk_mirror_field_0x098", metadata)
            self.assertNotIn("smpl_sbnk_mirror_field_0x09c", metadata)
            with wave.open(metadata["wav_path"], "rb") as wav:
                self.assertEqual(wav.getframerate(), 44100)


    def test_current_smpl_marker_lane_payload_writes_8_bit_useful_lane(self) -> None:
        row = a_series_objects.summarize_object_header(bytes(build_current_smpl_header()))
        payload = b"\x00\x55\x80\xaa\xff\x55\x7f\xaa"

        with tempfile.TemporaryDirectory() as tmp:
            metadata = a_series_objects.write_current_smpl_wav(
                row=row,
                output_dir=Path(tmp),
                stem_prefix="marker",
                read_stored_payload=lambda rel_offset, size: payload[:size],
                source_container="fixture.sfs",
                container_kind="test",
            )

            self.assertEqual(metadata["extraction_quality"], "Likely")
            self.assertEqual(metadata["stored_payload_transform"], "current-marker-lane-signed-high-byte")
            self.assertTrue(metadata["marker_lane_payload_detected"])
            self.assertEqual(metadata["marker_lane_useful_bytes"], 4)
            self.assertEqual(metadata["stored_payload_size"], 8)
            self.assertEqual(metadata["decoded_pcm_size"], 4)
            self.assertEqual(metadata["stored_sample_width_bytes"], 2)
            self.assertEqual(metadata["sample_width_bytes"], 1)
            self.assertIn("conversion_artifact_label", metadata)
            self.assertTrue(metadata["conversion_artifact_label"])
            self.assertIn("conversion_artifact_quality", metadata)
            self.assertIn("stored_payload_transform", metadata["field_quality"])
            with wave.open(metadata["wav_path"], "rb") as wav:
                self.assertEqual(wav.getsampwidth(), 1)
                self.assertEqual(wav.readframes(wav.getnframes()), b"\x80\x00\x7f\xff")
if __name__ == "__main__":
    unittest.main()

