import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from axklib.parameters import sbnk_contract, sbnk_links


class ReportSbnkLinksTests(unittest.TestCase):
    def make_ref(self, name: str, offset: int, link_id: int) -> sbnk_links.SmplRef:
        return sbnk_links.SmplRef(
            name=name,
            object_offset=offset,
            sample_rate=48000,
            payload_bytes=200,
            frames=100,
            root_key_midi_note_guess=60,
            fine_tune_cents_guess=0,
            link_id_0x078=link_id,
            group_id_0x06c=0,
            wave_length_frames_0x092=96,
            loop_start_frame_0x096=0,
            loop_length_frames_0x09a=0,
            wav_path="",
        )

    def test_load_smpl_refs_accepts_canonical_source_container_sidecars(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "source.hda"
            header = bytearray(0x200)
            header[0x078:0x07C] = (0x12345678).to_bytes(4, "big")
            header[0x06C:0x070] = (0x00000009).to_bytes(4, "big")
            header[0x07E] = 64
            header[0x07F] = 0
            header[0x092:0x096] = (16).to_bytes(4, "big")
            header[0x096:0x09A] = (0).to_bytes(4, "big")
            header[0x09A:0x09E] = (16).to_bytes(4, "big")
            image.write_bytes(header)
            sidecar = {
                "source_container": str(image),
                "object_offset": 0,
                "stored_payload_size": 32,
                "sample_width_bytes": 2,
                "sample_rate": 44100,
                "name_guess": "S01",
                "wav_path": "S01.wav",
            }
            (root / "S01.json").write_text(json.dumps(sidecar), encoding="utf-8")

            by_link, by_name = sbnk_links.load_smpl_refs(root)

        self.assertEqual(by_link[0x12345678][0].name, "S01")
        self.assertEqual(by_link[0x12345678][0].group_id_0x06c, 9)
        self.assertEqual(by_name["S01"][0].sample_rate, 44100)

    def test_choose_smpl_ref_uses_name_to_disambiguate_duplicate_link_ids(self) -> None:
        wanted = self.make_ref("K-Off 8'*029  -L", 0x11216800, 0x016B2AF4)
        other = self.make_ref("WiolinSolo 085-R", 0x164D8000, 0x016B2AF4)

        match = sbnk_links.choose_smpl_ref(
            "K-Off 8'*029  -L",
            0x016B2AF4,
            {0x016B2AF4: [other, wanted]},
            {wanted.name: [wanted], other.name: [other]},
        )

        self.assertIs(match.ref, wanted)
        self.assertEqual(match.method, "link+name")
        self.assertEqual(match.candidate_count, 2)
        self.assertEqual(match.candidate_refs, [wanted])

    def test_choose_smpl_ref_preserves_ambiguous_link_name_candidates_without_selection(
        self,
    ) -> None:
        first = self.make_ref("Snare", 0x1000, 0x42)
        second = self.make_ref("Snare", 0x2000, 0x42)

        match = sbnk_links.choose_smpl_ref(
            "Snare",
            0x42,
            {0x42: [first, second]},
            {"Snare": [first, second]},
        )

        self.assertIsNone(match.ref)
        self.assertEqual(match.method, "link+name-ambiguous")
        self.assertEqual(match.candidate_count, 2)
        self.assertEqual(match.candidate_refs, [first, second])

    def test_choose_smpl_ref_preserves_ambiguous_name_candidates_without_selection(self) -> None:
        first = self.make_ref("Snare", 0x1000, 0x41)
        second = self.make_ref("Snare", 0x2000, 0x42)

        match = sbnk_links.choose_smpl_ref(
            "Snare",
            0x99,
            {},
            {"Snare": [first, second]},
        )

        self.assertIsNone(match.ref)
        self.assertEqual(match.method, "name-ambiguous")
        self.assertEqual(match.candidate_count, 0)
        self.assertEqual(match.candidate_refs, [first, second])

    def test_smpl_unaligned_u32_at_0x092_reads_wave_length_field(self) -> None:
        header = bytearray(0x98)
        header[0x092:0x096] = (0x00011724).to_bytes(4, "big")

        self.assertEqual(sbnk_contract.smpl_unaligned_u32_at_0x092(header), 71460)

    def test_unused_empty_name_match_is_not_a_failed_link(self) -> None:
        self.assertEqual(sbnk_links.match_quality_for("unused-empty-name"), "NotApplicable")

    def test_estimated_pitch_base_word_matches_formula_representatives(self) -> None:
        self.assertEqual(sbnk_contract.estimated_pitch_base_word(60, 44100, 0), 0x13AB)
        self.assertEqual(sbnk_contract.estimated_pitch_base_word(48, 48000, -4), 0x0F32)
        self.assertEqual(sbnk_contract.estimated_pitch_base_word(60, 22050, 0), 0x17AA)
        self.assertEqual(sbnk_contract.estimated_pitch_base_word(60, 12345, 0), 0x1B03)
        self.assertIsNone(sbnk_contract.estimated_pitch_base_word(60, 0, 0))

    def test_root_key_pitch_word_handles_zero_root_key(self) -> None:
        self.assertEqual(sbnk_contract.root_key_pitch_word(0), 0x03AB)

    def test_pitch_base_word_status_distinguishes_clean_write_value_from_stored_exception(
        self,
    ) -> None:
        self.assertEqual(
            sbnk_contract.pitch_base_word_status(0x1600, 0x1600),
            "matches-pitch-formula",
        )
        self.assertEqual(
            sbnk_contract.pitch_base_word_status(0x1601, 0x1600),
            "stored-image-exception",
        )
        self.assertEqual(
            sbnk_contract.pitch_base_word_status(0x15BF, None, active=False),
            "inactive-secondary-single-member",
        )

    def test_current_sbnk_contract_round_trips_single_member_with_clean_pitch_word(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Talk02",
            smpl_link_id_0x078=0x10203040,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=9696,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=9696,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Talk Bank",
            instrument_name="Voice",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[:12], b"FSFSDEV3SPLX")
        self.assertEqual(payload[0x0C:0x10], b"SBNK")
        self.assertEqual(parsed.bank_topology, "single-member")
        self.assertFalse(parsed.right_slot_present)
        self.assertIsNone(parsed.right)
        self.assertEqual(parsed.left.sample_name, left.sample_name)
        self.assertEqual(parsed.left.smpl_link_id, left.smpl_link_id_0x078)
        self.assertEqual(parsed.left.pitch_base_word, parsed.left.clean_pitch_base_word_for_write)
        self.assertEqual(
            parsed.left.clean_pitch_base_word_for_write,
            sbnk_contract.estimated_pitch_base_word(
                left.root_key_0x0d6,
                left.sample_rate_0x0d8,
                left.fine_tune_cents_0x0dc,
            ),
        )
        self.assertEqual(
            parsed.secondary_pitch_base_word_status,
            sbnk_contract.PITCH_BASE_STATUS_INACTIVE,
        )

    def test_current_sbnk_contract_round_trips_two_member_with_clean_pitch_words(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=48,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-4,
            wave_length_frames_0x0f0=24000,
            loop_start_frame_0x0f8=1024,
            loop_length_frames_0x100=12000,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad R",
            smpl_link_id_0x078=0x22222222,
            root_key_0x0d6=60,
            sample_rate_0x0d8=22050,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=24000,
            loop_start_frame_0x0f8=1024,
            loop_length_frames_0x100=12000,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            right=right,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertTrue(parsed.right_slot_present)
        self.assertIsNotNone(parsed.right)
        self.assertEqual(parsed.left.pitch_base_word, parsed.left.clean_pitch_base_word_for_write)
        self.assertEqual(parsed.right.pitch_base_word, parsed.right.clean_pitch_base_word_for_write)
        self.assertEqual(
            parsed.left.clean_pitch_base_word_for_write,
            sbnk_contract.estimated_pitch_base_word(
                left.root_key_0x0d6,
                left.sample_rate_0x0d8,
                left.fine_tune_cents_0x0dc,
            ),
        )
        self.assertEqual(
            parsed.right.clean_pitch_base_word_for_write,
            sbnk_contract.estimated_pitch_base_word(
                right.root_key_0x0d6,
                right.sample_rate_0x0d8,
                right.fine_tune_cents_0x0dc,
            ),
        )

    def test_generated_two_member_wrapper_writes_template_free_stereo_defaults(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Tone-L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=1000,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=1000,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Tone-R",
            smpl_link_id_0x078=0x22222222,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=1000,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=1000,
        )

        payload = sbnk_contract.serialize_current_two_member_sbnk_payload(
            bank_name="Stereo Bank",
            left=left,
            right=right,
            key_range_low_0x0e3=48,
            key_range_high_0x0e2=72,
            sample_level_0x116=96,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(parsed.left.sample_name, "Tone-L")
        self.assertIsNotNone(parsed.right)
        assert parsed.right is not None
        self.assertEqual(parsed.right.sample_name, "Tone-R")
        self.assertEqual(payload[0x0E5], 1)
        self.assertEqual(payload[0x0EA:0x0EC], b"\x00\x00")
        self.assertEqual(payload[0x0EE:0x0F0], b"\x00\x00")
        self.assertEqual(parsed.key_range_low_0x0e3, 48)
        self.assertEqual(parsed.key_range_high_0x0e2, 72)
        self.assertEqual(parsed.sample_level_0x116, 96)
        self.assertNotEqual(parsed.left.clean_pitch_base_word_for_write, 0x1601)
        self.assertNotEqual(parsed.right.clean_pitch_base_word_for_write, 0x1601)

    def test_current_sbnk_contract_reports_wave_and_loop_end_cache_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        payload = bytearray(
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
            )
        )
        payload[0x0E8:0x0EC] = (1234).to_bytes(4, "big")
        payload[0x15C:0x160] = (117710).to_bytes(4, "big")
        payload[0x160:0x164] = (116476).to_bytes(4, "big")

        parsed = sbnk_contract.parse_current_sbnk_contract_payload(bytes(payload))

        self.assertEqual(parsed.wave_end_address_0x15c, 117710)
        self.assertEqual(parsed.expected_wave_end_address_from_start_length, 117710)
        self.assertEqual(parsed.wave_end_address_delta_from_expected, 0)
        self.assertEqual(parsed.loop_end_address_0x160, 116476)
        self.assertEqual(parsed.expected_loop_end_address_from_start_length, 116476)
        self.assertEqual(parsed.loop_end_address_delta_from_expected, 0)

    def test_current_sbnk_contract_reports_loop_end_cache_with_u32_wraparound(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Kick",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=4096,
            loop_start_frame_0x0f8=9341,
            loop_length_frames_0x100=0xFFFFFFFF,
        )
        payload = bytearray(
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Kick Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
            )
        )
        payload[0x160:0x164] = (9340).to_bytes(4, "big")

        parsed = sbnk_contract.parse_current_sbnk_contract_payload(bytes(payload))

        self.assertEqual(parsed.expected_loop_end_address_from_start_length, 9340)
        self.assertEqual(parsed.loop_end_address_delta_from_expected, 0)

    def test_current_sbnk_contract_parses_raw_sample_control_records(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        payload = bytearray(
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
            )
        )
        payload[0x164:0x168] = bytes([1, 2, 3, 0xE5])
        payload[0x178:0x17C] = bytes([66, 36, 1, 9])

        parsed = sbnk_contract.parse_current_sbnk_contract_payload(bytes(payload))

        self.assertEqual(parsed.sample_control1_device_0x164, 1)
        self.assertEqual(parsed.sample_control1_device_ui_label, "001/ModWhel")
        self.assertEqual(parsed.sample_control1_function_0x165, 2)
        self.assertEqual(parsed.sample_control1_function_ui_label, "AmpModDepth")
        self.assertEqual(parsed.sample_control1_type_0x166, 3)
        self.assertEqual(parsed.sample_control1_type_ui_label, "+ofst(+exp)")
        self.assertEqual(parsed.sample_control1_range_0x167, -27)
        self.assertEqual(parsed.sample_control6_device_0x178, 66)
        self.assertEqual(parsed.sample_control6_device_ui_label, "066/Sostenuto")
        self.assertEqual(parsed.sample_control6_function_0x179, 36)
        self.assertEqual(parsed.sample_control6_function_ui_label, "Control6 Range")
        self.assertEqual(parsed.sample_control6_type_0x17a, 1)
        self.assertEqual(parsed.sample_control6_type_ui_label, "-/+offset")
        self.assertEqual(parsed.sample_control6_range_0x17b, 9)
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(65), "---")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(121), "AfterTouch")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(122), "PitchBend")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(123), "NoteNumber")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(124), "Velocity")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(125), "ProgramLFO")
        self.assertEqual(sbnk_contract.sample_control_device_ui_label(126), "KeyOnRandom")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(3), "CutoffMdDpth")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(25), "FEG Dcy Rate")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(4), "Cutoff Bias")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(8), "Level")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(31), "Control1 Range")
        self.assertEqual(sbnk_contract.sample_control_function_ui_label(37), "")

    def test_current_sbnk_contract_writes_explicit_key_range_high_low(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad R",
            smpl_link_id_0x078=0x22222222,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            right=right,
            key_range_low_0x0e3=60,
            key_range_high_0x0e2=72,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x0E2], 72)
        self.assertEqual(payload[0x0E3], 60)
        self.assertEqual(parsed.key_range_high_0x0e2, 72)
        self.assertEqual(parsed.key_range_low_0x0e3, 60)

    def test_current_sbnk_contract_rejects_incomplete_or_reversed_key_range(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        with self.assertRaisesRegex(ValueError, "requires both"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                key_range_low_0x0e3=60,
            )
        with self.assertRaisesRegex(ValueError, "below low"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                key_range_low_0x0e3=72,
                key_range_high_0x0e2=60,
            )

    def test_current_sbnk_contract_writes_explicit_velocity_range_high_low(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad R",
            smpl_link_id_0x078=0x22222222,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            right=right,
            velocity_range_low_0x11b=40,
            velocity_range_high_0x11a=100,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x11A], 100)
        self.assertEqual(payload[0x11B], 40)
        self.assertEqual(parsed.velocity_range_high_0x11a, 100)
        self.assertEqual(parsed.velocity_range_low_0x11b, 40)

    def test_current_sbnk_sample_parameter_base_aligns_known_fields(self) -> None:
        base = sbnk_contract.CURRENT_SBNK_SAMPLE_PARAMETER_BASE

        self.assertEqual(base, 0x0A8)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x2E), 0x0D6)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x30), 0x0D8)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x34), 0x0DC)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x3A), 0x0E2)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x3B), 0x0E3)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x3D), 0x0E5)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x2A), 0x0D2)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x2B), 0x0D3)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x2C), 0x0D4)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x2D), 0x0D5)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x72), 0x11A)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x73), 0x11B)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x6A), 0x112)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x6B), 0x113)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x6C), 0x114)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x6D), 0x115)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x74), 0x11C)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x75), 0x11D)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x76), 0x11E)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x77), 0x11F)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x79), 0x121)
        self.assertEqual(sbnk_contract.sbnk_sample_parameter_offset(0x7A), 0x122)

    def test_current_sbnk_contract_reports_manual_defined_raw_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        payload = bytearray(
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
            )
        )
        payload[0x0C0:0x0C4] = (0x01020304).to_bytes(4, "big")
        payload[0x0C4:0x0C8] = (0x11223344).to_bytes(4, "big")
        payload[0x0C8:0x0CC] = (0x55667788).to_bytes(4, "big")
        payload[0x0CC:0x0D0] = (0x99AABBCC).to_bytes(4, "big")
        payload[0x0D0] = 0x07
        payload[0x0E6:0x0E8] = (12345).to_bytes(2, "big")
        payload[0x0E8:0x0EC] = (0x00001234).to_bytes(4, "big")
        payload[0x0EC:0x0F0] = (0x00005678).to_bytes(4, "big")

        parsed = sbnk_contract.parse_current_sbnk_contract_payload(bytes(payload))

        self.assertEqual(parsed.linked_programs_001_032_bitmap_0x0c0, 0x01020304)
        self.assertEqual(parsed.linked_programs_033_064_bitmap_0x0c4, 0x11223344)
        self.assertEqual(parsed.linked_programs_065_096_bitmap_0x0c8, 0x55667788)
        self.assertEqual(parsed.linked_programs_097_128_bitmap_0x0cc, 0x99AABBCC)
        self.assertEqual(parsed.sample_flags_0x0d0, 0x07)
        self.assertTrue(parsed.sample_bank_member_0x0d0_bit0)
        self.assertTrue(parsed.mono_sample_0x0d0_bit1)
        self.assertTrue(parsed.expanded_0x0d0_bit2)
        self.assertEqual(parsed.loop_tempo_0x0e6, 12345)
        self.assertEqual(parsed.left_wave_start_address_0x0e8, 0x00001234)
        self.assertEqual(parsed.left_wave_start_low16_0x0ea, 0x1234)
        self.assertEqual(parsed.right_wave_start_address_0x0ec, 0x00005678)
        self.assertEqual(parsed.right_wave_start_low16_0x0ee, 0x5678)

    def test_current_sbnk_contract_parses_sysex_aligned_exp_velocity_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        payload = bytearray(
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
            )
        )
        offset = sbnk_contract.sbnk_sample_parameter_offset
        payload[0x0D1] = 0x14
        payload[0x0D2] = 3
        payload[0x0D3] = 5
        payload[0x0D4] = 7
        payload[0x0D5] = 0xF4
        payload[offset(0x6E)] = 91
        payload[offset(0x6F)] = 0xC0
        payload[offset(0x70)] = 12
        payload[offset(0x71)] = 0xF6
        payload[offset(0x78)] = 0x81
        payload[0x121] = 9
        payload[offset(0xD4)] = 100
        payload[offset(0xD5)] = 24

        parsed = sbnk_contract.parse_current_sbnk_contract_payload(bytes(payload))

        self.assertEqual(parsed.mapout_flags_0x0d1, 0x14)
        self.assertEqual(parsed.sample_eq_type_0x0d1_b7_6, 0)
        self.assertEqual(parsed.sample_eq_type_ui_label, "PeakDip")
        self.assertTrue(parsed.fixed_pitch_on_0x0d1_bit4)
        self.assertTrue(parsed.key_xfade_on_0x0d1_bit2)
        self.assertEqual(parsed.midi_receive_channel_0x0d2, 3)
        self.assertEqual(parsed.pitch_bend_type_0x0d3, 5)
        self.assertEqual(parsed.pitch_bend_type_ui_label, "Up2Dwn3")
        self.assertEqual(parsed.pitch_bend_range_0x0d4, 7)
        self.assertEqual(parsed.coarse_tune_0x0d5, -12)
        self.assertEqual(parsed.sample_level_0x116, 91)
        self.assertEqual(parsed.pan_0x117, -64)
        self.assertEqual(parsed.velocity_low_limit_0x118, 12)
        self.assertEqual(parsed.velocity_offset_0x119, -10)
        self.assertEqual(parsed.velocity_sensitivity_0x120, -127)
        self.assertEqual(parsed.alternate_group_0x121, 9)
        self.assertEqual(parsed.velocity_xfade_high_0x17c, 100)
        self.assertEqual(parsed.velocity_xfade_low_0x17d, 24)

    def test_current_sbnk_contract_writes_midiset_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            midi_receive_channel_0x0d2=2,
            pitch_bend_type_0x0d3=1,
            pitch_bend_range_0x0d4=7,
            coarse_tune_0x0d5=-12,
            alternate_group_0x121=5,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.midi_receive_channel_0x0d2, 2)
        self.assertEqual(parsed.pitch_bend_type_0x0d3, 1)
        self.assertEqual(parsed.pitch_bend_type_ui_label, "Slow")
        self.assertEqual(parsed.pitch_bend_range_0x0d4, 7)
        self.assertEqual(parsed.coarse_tune_0x0d5, -12)
        self.assertEqual(parsed.alternate_group_0x121, 5)

    def test_current_sbnk_contract_writes_loop_tempo(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            loop_tempo_0x0e6=12340,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x0E6:0x0E8], bytes.fromhex("3034"))
        self.assertEqual(parsed.loop_tempo_0x0e6, 12340)

    def test_current_sbnk_contract_rejects_out_of_range_loop_tempo(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        with self.assertRaisesRegex(ValueError, "loop tempo out of range"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                loop_tempo_0x0e6=7999,
            )

    def test_sample_eq_type_ui_label_table(self) -> None:
        expected = {
            0: "PeakDip",
            1: "LoShelv",
            2: "HiShelv",
        }

        for raw_value, label in expected.items():
            with self.subTest(raw_value=raw_value):
                self.assertEqual(sbnk_contract.sample_eq_type_ui_label(raw_value), label)
        self.assertEqual(sbnk_contract.sample_eq_type_ui_label(3), "")

    def test_pitch_bend_type_ui_label_table(self) -> None:
        expected = {
            0: "Normal",
            1: "Slow",
            2: "Slow&Rev",
            3: "Stop",
            4: "Stop&Rev",
            5: "Up2Dwn3",
            6: "Up2Dwn4",
            7: "Up2Dwn5",
            8: "Up2Dwn12",
            9: "Up3Dwn2",
            10: "Up3Dwn4",
            11: "Up3Dwn5",
            12: "Up3Dwn12",
        }

        for raw_value, label in expected.items():
            with self.subTest(raw_value=raw_value):
                self.assertEqual(sbnk_contract.pitch_bend_type_ui_label(raw_value), label)
        self.assertEqual(sbnk_contract.pitch_bend_type_ui_label(13), "")

    def test_current_sbnk_contract_writes_sample_level(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            sample_level_0x116=80,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x116], 80)
        self.assertEqual(parsed.sample_level_0x116, 80)

    def test_current_sbnk_contract_writes_batched_sysex_aligned_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            pan_0x117=-32,
            velocity_low_limit_0x118=24,
            velocity_offset_0x119=-12,
            velocity_xfade_high_0x17c=96,
            velocity_xfade_low_0x17d=32,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x117], 0xE0)
        self.assertEqual(payload[0x118], 24)
        self.assertEqual(payload[0x119], 0xF4)
        self.assertEqual(payload[0x17C], 96)
        self.assertEqual(payload[0x17D], 32)
        self.assertEqual(parsed.pan_0x117, -32)
        self.assertEqual(parsed.velocity_low_limit_0x118, 24)
        self.assertEqual(parsed.velocity_offset_0x119, -12)
        self.assertEqual(parsed.velocity_xfade_high_0x17c, 96)
        self.assertEqual(parsed.velocity_xfade_low_0x17d, 32)

    def test_current_sbnk_contract_writes_pitch_expand_and_level_scaling_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            expand_detune_0x112=-5,
            expand_dephase_0x113=27,
            expand_width_0x114=-41,
            random_pitch_0x115=23,
            level_scaling_break1_0x11c=36,
            level_scaling_break2_0x11d=96,
            level_scaling_level1_0x11e=44,
            level_scaling_level2_0x11f=102,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x112], 0xFB)
        self.assertEqual(payload[0x113], 27)
        self.assertEqual(payload[0x114], 0xD7)
        self.assertEqual(payload[0x115], 23)
        self.assertEqual(payload[0x11C], 36)
        self.assertEqual(payload[0x11D], 96)
        self.assertEqual(payload[0x11E], 44)
        self.assertEqual(payload[0x11F], 102)
        self.assertEqual(parsed.expand_detune_0x112, -5)
        self.assertEqual(parsed.expand_dephase_0x113, 27)
        self.assertEqual(parsed.expand_width_0x114, -41)
        self.assertEqual(parsed.random_pitch_0x115, 23)
        self.assertEqual(parsed.level_scaling_break1_0x11c, 36)
        self.assertEqual(parsed.level_scaling_break2_0x11d, 96)
        self.assertEqual(parsed.level_scaling_level1_0x11e, 44)
        self.assertEqual(parsed.level_scaling_level2_0x11f, 102)

    def test_current_sbnk_contract_rejects_out_of_range_batched_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"midi_receive_channel_0x0d2": 17},
            {"pitch_bend_type_0x0d3": 13},
            {"pitch_bend_range_0x0d4": 13},
            {"coarse_tune_0x0d5": -65},
            {"coarse_tune_0x0d5": 64},
            {"alternate_group_0x121": 17},
            {"expand_detune_0x112": -8},
            {"expand_detune_0x112": 8},
            {"expand_dephase_0x113": -64},
            {"expand_dephase_0x113": 64},
            {"expand_width_0x114": -64},
            {"expand_width_0x114": 64},
            {"random_pitch_0x115": 64},
            {"level_scaling_break1_0x11c": 97, "level_scaling_break2_0x11d": 36},
            {"level_scaling_level1_0x11e": 128},
            {"level_scaling_level2_0x11f": 128},
            {"pan_0x117": 64},
            {"velocity_low_limit_0x118": 128},
            {"velocity_offset_0x119": -128},
            {"velocity_xfade_high_0x17c": 128},
            {"velocity_xfade_low_0x17d": 128},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range|exceeds"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_feg_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            feg_attack_rate_0x126=31,
            feg_decay_rate_0x127=53,
            feg_release_rate_0x128=97,
            feg_init_level_0x129=-40,
            feg_attack_level_0x12a=55,
            feg_sustain_level_0x12b=-22,
            feg_release_level_0x12c=33,
            feg_rate_key_scaling_0x12d=4,
            feg_rate_velocity_sensitivity_0x12e=-29,
            feg_attack_level_velocity_sensitivity_0x12f=41,
            feg_level_velocity_sensitivity_0x130=-17,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x126], 31)
        self.assertEqual(payload[0x127], 53)
        self.assertEqual(payload[0x128], 97)
        self.assertEqual(payload[0x129], 0xD8)
        self.assertEqual(payload[0x12A], 55)
        self.assertEqual(payload[0x12B], 0xEA)
        self.assertEqual(payload[0x12C], 33)
        self.assertEqual(payload[0x12D], 4)
        self.assertEqual(payload[0x12E], 0xE3)
        self.assertEqual(payload[0x12F], 41)
        self.assertEqual(payload[0x130], 0xEF)
        self.assertEqual(parsed.feg_attack_rate_0x126, 31)
        self.assertEqual(parsed.feg_decay_rate_0x127, 53)
        self.assertEqual(parsed.feg_release_rate_0x128, 97)
        self.assertEqual(parsed.feg_init_level_0x129, -40)
        self.assertEqual(parsed.feg_attack_level_0x12a, 55)
        self.assertEqual(parsed.feg_sustain_level_0x12b, -22)
        self.assertEqual(parsed.feg_release_level_0x12c, 33)
        self.assertEqual(parsed.feg_rate_key_scaling_0x12d, 4)
        self.assertEqual(parsed.feg_rate_velocity_sensitivity_0x12e, -29)
        self.assertEqual(parsed.feg_attack_level_velocity_sensitivity_0x12f, 41)
        self.assertEqual(parsed.feg_level_velocity_sensitivity_0x130, -17)

    def test_current_sbnk_contract_rejects_out_of_range_feg_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"feg_attack_rate_0x126": 128},
            {"feg_decay_rate_0x127": 128},
            {"feg_release_rate_0x128": 128},
            {"feg_init_level_0x129": -128},
            {"feg_attack_level_0x12a": 128},
            {"feg_sustain_level_0x12b": -128},
            {"feg_release_level_0x12c": 128},
            {"feg_rate_key_scaling_0x12d": 8},
            {"feg_rate_velocity_sensitivity_0x12e": -64},
            {"feg_attack_level_velocity_sensitivity_0x12f": 64},
            {"feg_level_velocity_sensitivity_0x130": -64},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_peg_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            peg_attack_rate_0x131=17,
            peg_decay_rate_0x132=39,
            peg_release_rate_0x133=91,
            peg_init_level_0x134=-36,
            peg_attack_level_0x135=48,
            peg_sustain_level_0x136=-14,
            peg_release_level_0x137=27,
            peg_rate_key_scaling_0x138=-5,
            peg_rate_velocity_sensitivity_0x139=33,
            peg_level_velocity_sensitivity_0x13a=-21,
            peg_range_0x13b=12,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x131], 17)
        self.assertEqual(payload[0x132], 39)
        self.assertEqual(payload[0x133], 91)
        self.assertEqual(payload[0x134], 0xDC)
        self.assertEqual(payload[0x135], 48)
        self.assertEqual(payload[0x136], 0xF2)
        self.assertEqual(payload[0x137], 27)
        self.assertEqual(payload[0x138], 0xFB)
        self.assertEqual(payload[0x139], 33)
        self.assertEqual(payload[0x13A], 0xEB)
        self.assertEqual(payload[0x13B], 12)
        self.assertEqual(parsed.peg_attack_rate_0x131, 17)
        self.assertEqual(parsed.peg_decay_rate_0x132, 39)
        self.assertEqual(parsed.peg_release_rate_0x133, 91)
        self.assertEqual(parsed.peg_init_level_0x134, -36)
        self.assertEqual(parsed.peg_attack_level_0x135, 48)
        self.assertEqual(parsed.peg_sustain_level_0x136, -14)
        self.assertEqual(parsed.peg_release_level_0x137, 27)
        self.assertEqual(parsed.peg_rate_key_scaling_0x138, -5)
        self.assertEqual(parsed.peg_rate_velocity_sensitivity_0x139, 33)
        self.assertEqual(parsed.peg_level_velocity_sensitivity_0x13a, -21)
        self.assertEqual(parsed.peg_range_0x13b, 12)

    def test_current_sbnk_contract_rejects_out_of_range_peg_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"peg_attack_rate_0x131": 128},
            {"peg_decay_rate_0x132": 128},
            {"peg_release_rate_0x133": 128},
            {"peg_init_level_0x134": -128},
            {"peg_attack_level_0x135": 128},
            {"peg_sustain_level_0x136": -128},
            {"peg_release_level_0x137": 128},
            {"peg_rate_key_scaling_0x138": -8},
            {"peg_rate_velocity_sensitivity_0x139": 64},
            {"peg_level_velocity_sensitivity_0x13a": -64},
            {"peg_range_0x13b": 64},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_aeg_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            aeg_attack_rate_0x13c=23,
            aeg_decay_rate_0x13d=45,
            aeg_release_rate_0x13e=89,
            aeg_sustain_level_0x141=67,
            aeg_attack_mode_0x143=1,
            aeg_rate_key_scaling_0x144=-3,
            aeg_rate_velocity_sensitivity_0x145=37,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x13C], 23)
        self.assertEqual(payload[0x13D], 45)
        self.assertEqual(payload[0x13E], 89)
        self.assertEqual(payload[0x141], 67)
        self.assertEqual(payload[0x143], 1)
        self.assertEqual(payload[0x144], 0xFD)
        self.assertEqual(payload[0x145], 37)
        self.assertEqual(parsed.aeg_attack_rate_0x13c, 23)
        self.assertEqual(parsed.aeg_decay_rate_0x13d, 45)
        self.assertEqual(parsed.aeg_release_rate_0x13e, 89)
        self.assertEqual(parsed.aeg_sustain_level_0x141, 67)
        self.assertEqual(parsed.aeg_attack_mode_0x143, 1)
        self.assertEqual(parsed.aeg_rate_key_scaling_0x144, -3)
        self.assertEqual(parsed.aeg_rate_velocity_sensitivity_0x145, 37)

    def test_current_sbnk_contract_rejects_out_of_range_aeg_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"aeg_attack_rate_0x13c": 128},
            {"aeg_decay_rate_0x13d": 128},
            {"aeg_release_rate_0x13e": 128},
            {"aeg_sustain_level_0x141": 128},
            {"aeg_attack_mode_0x143": 3},
            {"aeg_rate_key_scaling_0x144": -8},
            {"aeg_rate_velocity_sensitivity_0x145": 64},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_lfo_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            lfo_wave_0x146=2,
            lfo_speed_0x147=72,
            lfo_delay_time_0x148=45,
            lfo_flags_0x149=0x07,
            lfo_cutoff_mod_depth_0x14a=83,
            lfo_pitch_mod_depth_0x14b=47,
            lfo_amp_mod_depth_0x14c=29,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x146], 2)
        self.assertEqual(payload[0x147], 72)
        self.assertEqual(payload[0x148], 45)
        self.assertEqual(payload[0x149], 0x07)
        self.assertEqual(payload[0x14A], 83)
        self.assertEqual(payload[0x14B], 47)
        self.assertEqual(payload[0x14C], 29)
        self.assertEqual(parsed.lfo_wave_0x146, 2)
        self.assertEqual(parsed.lfo_speed_0x147, 72)
        self.assertEqual(parsed.lfo_speed_ui_value, 73)
        self.assertEqual(parsed.lfo_delay_time_0x148, 45)
        self.assertEqual(parsed.lfo_delay_ui_value, 45)
        self.assertEqual(parsed.lfo_flags_0x149, 0x07)
        self.assertTrue(parsed.lfo_key_on_sync_0x149_bit0)
        self.assertTrue(parsed.lfo_cutoff_mod_phase_invert_0x149_bit1)
        self.assertTrue(parsed.lfo_pitch_mod_phase_invert_0x149_bit2)
        self.assertEqual(parsed.lfo_cutoff_mod_depth_0x14a, 83)
        self.assertEqual(parsed.lfo_pitch_mod_depth_0x14b, 47)
        self.assertEqual(parsed.lfo_amp_mod_depth_0x14c, 29)

    def test_current_sbnk_contract_rejects_out_of_range_lfo_known_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"lfo_wave_0x146": 4},
            {"lfo_speed_0x147": 128},
            {"lfo_delay_time_0x148": 128},
            {"lfo_flags_0x149": 8},
            {"lfo_cutoff_mod_depth_0x14a": 128},
            {"lfo_pitch_mod_depth_0x14b": 128},
            {"lfo_amp_mod_depth_0x14c": 128},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_known_filter_eq_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            start_address_velocity_sensitivity_0x108=-22,
            filter_type_0x109=15,
            filter_cutoff_0x10a=91,
            filter_q_width_0x10b=23,
            sample_eq_frequency_0x122=37,
            sample_eq_gain_0x123=69,
            sample_eq_width_0x124=80,
            filter_cutoff_distance_0x125=-31,
            filter_gain_0x151=-13,
            output1_0x17e=2,
            output1_level_0x17f=90,
            output2_0x180=7,
            output2_level_0x181=45,
            sample_portamento_type_0x182=4,
            sample_portamento_rate_0x183=37,
            sample_portamento_time_0x184=91,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x108], 0xEA)
        self.assertEqual(payload[0x109], 15)
        self.assertEqual(payload[0x10A], 91)
        self.assertEqual(payload[0x10B], 23)
        self.assertEqual(payload[0x122], 37)
        self.assertEqual(payload[0x123], 69)
        self.assertEqual(payload[0x124], 80)
        self.assertEqual(payload[0x151], 0xF3)
        self.assertEqual(parsed.start_address_velocity_sensitivity_0x108, -22)
        self.assertEqual(parsed.filter_type_0x109, 15)
        self.assertEqual(parsed.filter_type_ui_label, "HPF+Peak")
        self.assertEqual(parsed.filter_cutoff_0x10a, 91)
        self.assertEqual(parsed.filter_q_width_0x10b, 23)
        self.assertEqual(parsed.sample_eq_frequency_0x122, 37)
        self.assertEqual(parsed.sample_eq_frequency_ui_label, "1.4kHz")
        self.assertEqual(parsed.sample_eq_gain_0x123, 69)
        self.assertEqual(parsed.sample_eq_gain_db, 5)
        self.assertEqual(parsed.sample_eq_width_0x124, 80)
        self.assertEqual(parsed.sample_eq_width_ui_value, 8.0)
        self.assertEqual(parsed.filter_cutoff_distance_0x125, -31)
        self.assertEqual(parsed.filter_gain_0x151, -13)
        self.assertEqual(parsed.output1_0x17e, 2)
        self.assertEqual(parsed.output1_ui_label, "E1-Through")
        self.assertEqual(parsed.output1_level_0x17f, 90)
        self.assertEqual(parsed.output2_0x180, 7)
        self.assertEqual(parsed.output2_ui_label, "E1-Through")
        self.assertEqual(parsed.output2_level_0x181, 45)
        self.assertEqual(parsed.sample_portamento_type_0x182, 4)
        self.assertEqual(parsed.sample_portamento_type_ui_label, "time(fingered)")
        self.assertEqual(parsed.sample_portamento_rate_0x183, 37)
        self.assertEqual(parsed.sample_portamento_time_0x184, 91)

        lane_sample_check = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            output1_0x17e=7,
            output2_0x180=0,
        )
        parsed_lane_sample_check = sbnk_contract.parse_current_sbnk_contract_payload(
            lane_sample_check
        )
        self.assertEqual(parsed_lane_sample_check.output1_ui_label, "AssnOut3&4")
        self.assertEqual(parsed_lane_sample_check.output2_ui_label, "off")

        lane_specific_sample_check = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            output1_0x17e=5,
            output2_0x180=5,
        )
        parsed_lane_specific_sample_check = sbnk_contract.parse_current_sbnk_contract_payload(
            lane_specific_sample_check
        )
        self.assertEqual(parsed_lane_specific_sample_check.output1_ui_label, "AssnOutL&R")
        self.assertEqual(parsed_lane_specific_sample_check.output2_ui_label, "DIG&OPT")

        output2_raw2_sample_check = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            output1_0x17e=2,
            output2_0x180=2,
        )
        parsed_output2_raw2_sample_check = sbnk_contract.parse_current_sbnk_contract_payload(
            output2_raw2_sample_check
        )
        self.assertEqual(parsed_output2_raw2_sample_check.output1_ui_label, "E1-Through")
        self.assertEqual(parsed_output2_raw2_sample_check.output2_ui_label, "AssnOut1&2")

    def test_sample_eq_frequency_ui_label_uses_ordered_sampler_steps_with_raw_offset(self) -> None:
        expected = {
            4: "32Hz",
            5: "36Hz",
            28: "500Hz",
            34: "1.0kHz",
            37: "1.4kHz",
            58: "16.0kHz",
        }

        for raw_value, label in expected.items():
            with self.subTest(raw_value=raw_value):
                self.assertEqual(sbnk_contract.sample_eq_frequency_ui_label(raw_value), label)
        self.assertEqual(sbnk_contract.sample_eq_frequency_ui_label(3), "")
        self.assertEqual(sbnk_contract.sample_eq_frequency_ui_label(59), "")

    def test_current_sbnk_contract_rejects_out_of_range_filter_eq_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        out_of_range_values = (
            {"filter_type_0x109": 128},
            {"filter_cutoff_0x10a": 128},
            {"filter_q_width_0x10b": 32},
            {"sample_eq_frequency_0x122": 3},
            {"sample_eq_frequency_0x122": 59},
            {"sample_eq_gain_0x123": 51},
            {"sample_eq_gain_0x123": 77},
            {"sample_eq_width_0x124": 9},
            {"sample_eq_width_0x124": 121},
            {"filter_cutoff_distance_0x125": -64},
            {"filter_cutoff_distance_0x125": 64},
            {"start_address_velocity_sensitivity_0x108": -64},
            {"start_address_velocity_sensitivity_0x108": 64},
            {"filter_gain_0x151": -32},
            {"filter_gain_0x151": 32},
            {"output1_0x17e": 13},
            {"output1_level_0x17f": 128},
            {"output2_0x180": 13},
            {"output2_level_0x181": 128},
            {"sample_portamento_type_0x182": 6},
            {"sample_portamento_rate_0x183": 128},
            {"sample_portamento_time_0x184": 128},
        )
        for kwargs in out_of_range_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "out of range"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_writes_filter_scaling_candidate_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            filter_cutoff_key_scaling_break1_0x10c=36,
            filter_cutoff_key_scaling_break2_0x10d=96,
            filter_cutoff_key_scaling_level1_0x10e=-45,
            filter_cutoff_key_scaling_level2_0x10f=52,
            filter_cutoff_velocity_sensitivity_0x110=35,
            filter_q_width_velocity_sensitivity_0x111=-27,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x10C], 36)
        self.assertEqual(payload[0x10D], 96)
        self.assertEqual(payload[0x10E], 0xD3)
        self.assertEqual(payload[0x10F], 52)
        self.assertEqual(payload[0x110], 35)
        self.assertEqual(payload[0x111], 0xE5)
        self.assertEqual(parsed.filter_cutoff_key_scaling_break1_0x10c, 36)
        self.assertEqual(parsed.filter_cutoff_key_scaling_break2_0x10d, 96)
        self.assertEqual(parsed.filter_cutoff_key_scaling_level1_0x10e, -45)
        self.assertEqual(parsed.filter_cutoff_key_scaling_level2_0x10f, 52)
        self.assertEqual(parsed.filter_cutoff_velocity_sensitivity_0x110, 35)
        self.assertEqual(parsed.filter_q_width_velocity_sensitivity_0x111, -27)
        self.assertEqual(parsed.filter_scaling_bp1_0x10c, 36)
        self.assertEqual(parsed.filter_scaling_bp2_0x10d, 96)
        self.assertEqual(parsed.filter_scaling_cutoff1_0x10e, -45)
        self.assertEqual(parsed.filter_scaling_cutoff2_0x10f, 52)
        self.assertEqual(parsed.filter_velocity_to_cutoff_0x110, 35)
        self.assertEqual(parsed.filter_velocity_to_q_width_0x111, -27)

    def test_current_sbnk_contract_rejects_invalid_filter_scaling_candidate_fields(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        invalid_values = (
            {"filter_cutoff_key_scaling_break1_0x10c": 36},
            {
                "filter_cutoff_key_scaling_break1_0x10c": 96,
                "filter_cutoff_key_scaling_break2_0x10d": 36,
            },
            {
                "filter_cutoff_key_scaling_break1_0x10c": 36,
                "filter_cutoff_key_scaling_break2_0x10d": 128,
            },
            {"filter_cutoff_key_scaling_level1_0x10e": -128},
            {"filter_cutoff_key_scaling_level2_0x10f": 128},
            {"filter_cutoff_velocity_sensitivity_0x110": 64},
            {"filter_q_width_velocity_sensitivity_0x111": -64},
        )
        for kwargs in invalid_values:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "range|require both|exceeds"):
                    sbnk_contract.serialize_current_sbnk_contract_payload(
                        bank_name="Pad Bank",
                        left=left,
                        allow_zero_inactive_right_slot_without_template=True,
                        **kwargs,
                    )

    def test_current_sbnk_contract_rejects_out_of_range_sample_level(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        with self.assertRaisesRegex(ValueError, "sample level"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                sample_level_0x116=128,
            )

    def test_current_sbnk_contract_writes_velocity_sensitivity_as_signed_byte(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Pad Bank",
            left=left,
            allow_zero_inactive_right_slot_without_template=True,
            velocity_sensitivity_0x120=-12,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x120], 0xF4)
        self.assertEqual(parsed.velocity_sensitivity_0x120, -12)

    def test_current_sbnk_contract_rejects_out_of_range_velocity_sensitivity(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        with self.assertRaisesRegex(ValueError, "velocity sensitivity"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                velocity_sensitivity_0x120=128,
            )

    def test_current_sbnk_contract_rejects_incomplete_or_reversed_velocity_range(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Pad L",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )

        with self.assertRaisesRegex(ValueError, "requires both"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                velocity_range_low_0x11b=40,
            )
        with self.assertRaisesRegex(ValueError, "below low"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Pad Bank",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                velocity_range_low_0x11b=100,
                velocity_range_high_0x11a=40,
            )

    def test_current_sbnk_contract_rejects_stored_pitch_cache_as_write_input(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="WHISPER1",
            smpl_link_id_0x078=0x33333333,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=1000,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=1000,
        )

        with self.assertRaisesRegex(ValueError, "not write inputs"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Stale Source",
                left=left,
                allow_zero_inactive_right_slot_without_template=True,
                stored_pitch_base_word_0x0de=0x1601,
            )

    def test_current_sbnk_contract_requires_template_for_single_member_defaults(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Talk02",
            smpl_link_id_0x078=0x10203040,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=9696,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=9696,
        )

        with self.assertRaisesRegex(ValueError, "requires a template"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Talk Bank",
                left=left,
            )

    def new_sample_single_member_template(self) -> bytes:
        template = bytearray(sbnk_contract.CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE)
        template[:12] = b"FSFSDEV3SPLX"
        template[0x0C:0x10] = b"SBNK"
        template[0x10:0x14] = b"\x00" * 4
        template[0x32:0x42] = b"_NewSample".ljust(16, b" ")
        template[0x78:0x88] = b"SMP 252511".ljust(16, b" ")
        template[0x088:0x098] = b" " * 16
        template[0x0A4:0x0A8] = (0xDEADBEEF).to_bytes(4, "big")
        template[0x0D7] = 64
        template[0x0DA:0x0DC] = (44100).to_bytes(2, "big")
        template[0x0DD] = 0
        template[0x0E5] = 0
        template[0x0EA:0x0EC] = (0x33B0).to_bytes(2, "big")
        template[0x0EE:0x0F0] = (0x33B0).to_bytes(2, "big")
        template[0x0E0:0x0E2] = (0x15BF).to_bytes(2, "big")
        template[0x0F4:0x0F8] = (103244).to_bytes(4, "big")
        template[0x0FC:0x100] = (13232).to_bytes(4, "big")
        template[0x104:0x108] = (103244).to_bytes(4, "big")
        return bytes(template)

    def test_single_member_policy_uses_template_but_forces_empty_right_topology(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Generated",
            smpl_link_id_0x078=0x12345678,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=9696,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=9696,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="Generated Bank",
            left=left,
            template=self.new_sample_single_member_template(),
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(payload[0x088:0x098], b"\x00" * 16)
        self.assertEqual(payload[0x0A4:0x0A8], b"\x00" * 4)
        self.assertEqual(payload[0x10:0x14], b"\x00" * 4)
        self.assertEqual(payload[0x0D7], 64)
        self.assertEqual(payload[0x0DA:0x0DC], (44100).to_bytes(2, "big"))
        self.assertEqual(payload[0x0DD], 0)
        self.assertEqual(payload[0x0E5], 0)
        self.assertEqual(payload[0x0EA:0x0EC], (0x33B0).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], (0x33B0).to_bytes(2, "big"))
        self.assertEqual(payload[0x0E0:0x0E2], (0x15BF).to_bytes(2, "big"))
        self.assertEqual(payload[0x0F4:0x0F8], (103244).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], (13232).to_bytes(4, "big"))
        self.assertEqual(payload[0x104:0x108], (103244).to_bytes(4, "big"))
        self.assertEqual(parsed.bank_topology, "single-member")
        self.assertEqual(parsed.left.sample_name, "Generated")
        self.assertEqual(parsed.left.smpl_link_id, 0x12345678)
        self.assertEqual(parsed.left.pitch_base_word, parsed.left.clean_pitch_base_word_for_write)
        self.assertEqual(parsed.secondary_pitch_base_word_0x0e0, 0x15BF)
        self.assertEqual(
            parsed.secondary_pitch_base_word_status,
            sbnk_contract.PITCH_BASE_STATUS_INACTIVE,
        )

    def test_minimal_generated_single_member_sbnk_object_fixture_uses_newsample_template(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=62,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=-3,
            wave_length_frames_0x0f0=2048,
            loop_start_frame_0x0f8=128,
            loop_length_frames_0x100=1024,
        )

        template = self.new_sample_single_member_template()
        fixture = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(fixture)

        self.assertEqual(len(fixture), sbnk_contract.CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE)
        self.assertEqual(fixture[0x10:0x14], template[0x10:0x14])
        self.assertEqual(parsed.bank_name, "GEN BANK")
        self.assertEqual(parsed.bank_topology, "single-member")
        self.assertFalse(parsed.right_slot_present)
        self.assertIsNone(parsed.right)
        self.assertEqual(parsed.left.sample_name, "GEN 000001")
        self.assertEqual(parsed.left.smpl_link_id, 0x016BCAFE)
        self.assertEqual(parsed.left.root_key, 62)
        self.assertEqual(parsed.left.sample_rate, 44100)
        self.assertEqual(parsed.left.fine_tune_cents, -3)
        self.assertEqual(parsed.left.wave_length_frames, 2048)
        self.assertEqual(parsed.left.loop_start_frame, 128)
        self.assertEqual(parsed.left.loop_length_frames, 1024)
        self.assertEqual(parsed.left.pitch_base_word, parsed.left.clean_pitch_base_word_for_write)
        self.assertEqual(parsed.secondary_pitch_base_word_0x0e0, 0x15BF)
        self.assertEqual(fixture[0x0E5], template[0x0E5])
        self.assertEqual(fixture[0x0EA:0x0EC], template[0x0EA:0x0EC])
        self.assertEqual(fixture[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(fixture[0x0F4:0x0F8], template[0x0F4:0x0F8])
        self.assertEqual(fixture[0x0FC:0x100], template[0x0FC:0x100])
        self.assertEqual(fixture[0x104:0x108], template[0x104:0x108])

    def test_single_member_one_shot_loop_cache_policy_updates_proven_cache_lanes(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=116476,
        )

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=self.new_sample_single_member_template(),
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT,
        )

        self.assertEqual(payload[0x0E5], 0x04)
        self.assertEqual(payload[0x0EA:0x0EC], b"\x00\x00")
        self.assertEqual(payload[0x0EE:0x0F0], b"\x00\x00")
        self.assertEqual(payload[0x0F8:0x0FC], b"\x00\x00\x00\x00")
        self.assertEqual(payload[0x100:0x104], (116476).to_bytes(4, "big"))

    def test_single_member_one_shot_loop_cache_policy_rejects_two_member_sbnk(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Left",
            smpl_link_id_0x078=0x11111111,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=100,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=100,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Right",
            smpl_link_id_0x078=0x22222222,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=100,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=100,
        )

        with self.assertRaisesRegex(ValueError, "single-member wave-start"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="GEN BANK",
                left=left,
                right=right,
                loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT,
            )

    def test_single_member_forward_loop_cache_policy_updates_only_proven_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD,
        )

        self.assertEqual(payload[0x0E5], template[0x0E5])
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x100:0x104], (114428).to_bytes(4, "big"))

    def test_single_member_forward_to_zero_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO,
        )

        self.assertEqual(payload[0x0E5], 0x01)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))

    def test_single_member_forward_to_zero_forward_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO_FORWARD,
        )

        self.assertEqual(payload[0x0E5], 0x02)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))

    def test_single_member_reverse_loop_cache_policy_updates_mode_and_authority_lane(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_REVERSE,
        )

        self.assertEqual(payload[0x0E5], 0x03)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))

    def test_single_member_one_shot_reverse_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT_REVERSE,
        )

        self.assertEqual(payload[0x0E5], 0x05)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))

    def test_two_member_forward_loop_cache_policy_updates_left_authority_lane(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], template[0x0E5])
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_two_member_forward_to_zero_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], 0x01)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_two_member_forward_to_zero_forward_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO_FORWARD,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], 0x02)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_two_member_reverse_loop_cache_policy_updates_mode_and_authority_lane(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_REVERSE,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], 0x03)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_two_member_one_shot_loop_cache_policy_updates_mode_and_authority_lane(self) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], 0x04)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_two_member_one_shot_reverse_loop_cache_policy_updates_mode_and_authority_lane(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=116476,
            loop_start_frame_0x0f8=2048,
            loop_length_frames_0x100=114428,
        )
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="sine wave",
            smpl_link_id_0x078=0x016B1DBC,
            root_key_0x0d6=66,
            sample_rate_0x0d8=48000,
            fine_tune_cents_0x0dc=-20,
            wave_length_frames_0x0f0=128,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=128,
        )
        template = self.new_sample_single_member_template()

        payload = sbnk_contract.serialize_current_sbnk_contract_payload(
            bank_name="GEN BANK",
            left=left,
            right=right,
            template=template,
            loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT_REVERSE,
        )
        parsed = sbnk_contract.parse_current_sbnk_contract_payload(payload)

        self.assertEqual(parsed.bank_topology, "two-member")
        self.assertEqual(payload[0x0E5], 0x05)
        self.assertEqual(payload[0x0EA:0x0EC], (2048).to_bytes(2, "big"))
        self.assertEqual(payload[0x0EE:0x0F0], template[0x0EE:0x0F0])
        self.assertEqual(payload[0x0F8:0x0FC], (2048).to_bytes(4, "big"))
        self.assertEqual(payload[0x0FC:0x100], b"\x00\x00\x00\x00")

    def test_single_member_one_shot_loop_cache_policy_rejects_unproven_wide_start_cache(
        self,
    ) -> None:
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="GEN 000001",
            smpl_link_id_0x078=0x016BCAFE,
            root_key_0x0d6=64,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=200000,
            loop_start_frame_0x0f8=0x10000,
            loop_length_frames_0x100=1000,
        )

        with self.assertRaisesRegex(ValueError, "wave-start low16"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="GEN BANK",
                left=left,
                template=self.new_sample_single_member_template(),
                loop_cache_policy=sbnk_contract.CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT,
            )

    def test_single_member_serializer_rejects_two_member_template(self) -> None:
        template = bytearray(self.new_sample_single_member_template())
        template[0x088:0x098] = b"Right".ljust(16, b" ")
        left = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name="Generated",
            smpl_link_id_0x078=0x12345678,
            root_key_0x0d6=60,
            sample_rate_0x0d8=44100,
            fine_tune_cents_0x0dc=0,
            wave_length_frames_0x0f0=9696,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=9696,
        )

        with self.assertRaisesRegex(ValueError, "empty right sample-name"):
            sbnk_contract.serialize_current_sbnk_contract_payload(
                bank_name="Generated Bank",
                left=left,
                template=bytes(template),
            )


if __name__ == "__main__":
    unittest.main()
