import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace

from axklib.audio import exact_export


class ExportSbnkExactAudioTests(unittest.TestCase):
    def mono(
        self, *, rate: int = 48000, width: int = 2, frames: int = 100
    ) -> exact_export.MonoExport:
        return exact_export.MonoExport(
            object_offset=0,
            name="sample",
            sample_rate=rate,
            sample_width_bytes=width,
            frames=frames,
            source_wav_path="source.wav",
            exported_wav_path="export.wav",
            exported_json_path="export.json",
            partition_index=None,
            partition_name="",
            volume_name="",
            category_name="",
        )

    def test_exact_stereo_requires_equal_rate_width_and_frame_count(self) -> None:
        left = self.mono(rate=32000, width=2, frames=100)

        self.assertTrue(exact_export.is_exact_stereo_representable(left, self.mono(rate=32000)))
        self.assertFalse(exact_export.is_exact_stereo_representable(left, self.mono(rate=32145)))
        self.assertFalse(exact_export.is_exact_stereo_representable(left, self.mono(width=1)))
        self.assertFalse(exact_export.is_exact_stereo_representable(left, self.mono(frames=101)))
        self.assertFalse(exact_export.is_exact_stereo_representable(left, None))

    def test_category_output_dir_uses_yamaha_volume_path(self) -> None:
        location = exact_export.ObjectLocation(
            partition_index=1,
            partition_name="hd1            1",
            volume_name="Orch Full",
            category_code="SMPL",
            category_name="Samples",
            entry_name='Gong 18" Med',
            match_quality="Known",
        )

        path = exact_export.category_output_dir(Path("out"), location, "Samples")

        self.assertEqual(path, Path("out") / "partition_01_hd1_1" / "Orch_Full" / "Samples")

    def test_load_object_locations_skips_candidate_inventory_rows(self) -> None:
        with TemporaryDirectory() as tmp:
            inventory_dir = Path(tmp)
            (inventory_dir / "volume_objects.csv").write_text(
                "object_offset,partition_index,partition_name,volume_name,category_code,category_name,entry_name,match_quality\n"
                "4096,0,hd0,Vol,SMPL,Samples,Known,Known\n"
                "8192,0,hd0,Vol,SMPL,Samples,Candidate,Tentative\n",
                encoding="utf-8",
            )

            locations = exact_export.load_object_locations(inventory_dir)

        self.assertEqual(sorted(locations), [4096])
        self.assertEqual(locations[4096].match_quality, "Known")

    def test_direct_smpl_category_location_is_labeled_as_standalone_sample_visibility(self) -> None:
        with TemporaryDirectory() as tmp:
            inventory_dir = Path(tmp)
            (inventory_dir / "volume_objects.csv").write_text(
                "object_offset,partition_index,partition_name,volume_name,category_code,category_name,entry_name,match_quality\n"
                "4096,1,hd1,Orch Full,SMPL,Samples,BassTrombn 027,Known\n",
                encoding="utf-8",
            )

            locations = exact_export.load_object_locations(inventory_dir)

        self.assertEqual(locations[4096].location_source, "direct-smpl-category-visibility")
        self.assertEqual(locations[4096].relationship_path, "SMPL-category-entry")
        self.assertIsNone(locations[4096].owner_object_offset)

    def test_known_sbnk_pair_requires_known_left_and_right_matches(self) -> None:
        known = SimpleNamespace(
            bank_topology="two-member",
            left_match_quality="Known",
            right_match_quality="Known",
        )
        ambiguous = SimpleNamespace(
            bank_topology="two-member",
            left_match_quality="Known",
            right_match_quality="Tentative",
        )
        mono = SimpleNamespace(
            bank_topology="single-member",
            left_match_quality="Known",
            right_match_quality="NotApplicable",
        )

        self.assertTrue(exact_export.is_known_sbnk_pair(known))
        self.assertFalse(exact_export.is_known_sbnk_pair(ambiguous))
        self.assertFalse(exact_export.is_known_sbnk_pair(mono))

    def test_current_bank_relationships_derive_locations_without_overriding_direct_rows(
        self,
    ) -> None:
        with TemporaryDirectory() as tmp:
            relationships_dir = Path(tmp)
            (relationships_dir / "current_prog_bank_links.csv").write_text(
                "prog_payload_offset,prog_name,matched_target_payload_offset,matched_target_type,matched_target_name,match_quality\n"
                "100,Program A,200,SBAC,Bank Group,Likely\n",
                encoding="utf-8",
            )
            (relationships_dir / "current_sbac_sbnk_links.csv").write_text(
                "sbac_payload_offset,sbac_name,matched_sbnk_payload_offset,matched_sbnk_name,match_quality\n"
                "200,Bank Group,300,Member Bank,Known\n"
                "200,Bank Group,400,Ambiguous Bank,Tentative\n",
                encoding="utf-8",
            )
            locations = {
                100: exact_export.ObjectLocation(
                    partition_index=0,
                    partition_name="hd0",
                    volume_name="Vol",
                    category_code="PROG",
                    category_name="Programs",
                    entry_name="Program A",
                    match_quality="Known",
                ),
                400: exact_export.ObjectLocation(
                    partition_index=1,
                    partition_name="hd1",
                    volume_name="Direct",
                    category_code="SBNK",
                    category_name="Sample Banks",
                    entry_name="Ambiguous Bank",
                    match_quality="Known",
                ),
            }

            derived = exact_export.apply_bank_relationship_locations(locations, relationships_dir)

        self.assertIn(200, locations)
        self.assertIn(300, locations)
        self.assertEqual(locations[300].volume_name, "Vol")
        self.assertEqual(locations[400].volume_name, "Direct")
        self.assertEqual(sorted(row.object_offset for row in derived), [200, 300])

    def test_sbac_volume_disambiguation_derives_likely_locations_from_object_keys(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            relationships_dir = root / "relationships"
            volume_dir = root / "volume"
            inventory_dir = root / "inventory"
            relationships_dir.mkdir()
            volume_dir.mkdir()
            inventory_dir.mkdir()
            (relationships_dir / "current_prog_bank_links.csv").write_text("", encoding="utf-8")
            (relationships_dir / "current_sbac_sbnk_links.csv").write_text(
                """sbac_payload_offset,sbac_name,matched_sbnk_payload_offset,matched_sbnk_name,match_quality
""",
                encoding="utf-8",
            )
            (volume_dir / "current_sbac_volume_disambiguation.csv").write_text(
                """sbac_object_key,sbac_name,slot_sbnk_name,volume_selected_sbnk_object_key,volume_match_quality,volume_match_method
p0:sfs10,25 Saw1,Saw1 C2        *,p0:sfs30,Likely,active-sbac-slot-name+hidden-candidate+volume-handle-sfs-sequence
""",
                encoding="utf-8",
            )
            (inventory_dir / "ynode_records.csv").write_text(
                """partition_index,sfs_id,payload_offset
0,10,1000
0,30,3000
""",
                encoding="utf-8",
            )
            locations = {
                1000: exact_export.ObjectLocation(
                    partition_index=0,
                    partition_name="hd0",
                    volume_name="Megasynth 1",
                    category_code="SBAC",
                    category_name="Sample Bank Accessories",
                    entry_name="25 Saw1",
                    match_quality="Known",
                )
            }

            derived = exact_export.apply_bank_relationship_locations(
                locations,
                relationships_dir,
                inventory_dir,
                volume_dir,
            )

        self.assertIn(3000, locations)
        self.assertEqual(locations[3000].volume_name, "Megasynth 1")
        self.assertEqual(locations[3000].match_quality, "Likely")
        self.assertEqual(
            locations[3000].location_source, "current-sbac-volume-sequence-relationship"
        )
        self.assertEqual([row.object_offset for row in derived], [3000])
        self.assertEqual(derived[0].relationship_quality, "Likely")

    def test_sbnk_member_locations_require_known_member_matches(self) -> None:
        locations = {
            300: exact_export.ObjectLocation(
                partition_index=0,
                partition_name="hd0",
                volume_name="Vol",
                category_code="SBNK",
                category_name="Sample Banks",
                entry_name="Member Bank",
                match_quality="Known",
            )
        }
        rows = [
            SimpleNamespace(
                sbnk_offset=300,
                bank_name="Member Bank",
                left_smpl_offset=500,
                left_smpl_name="Left",
                left_match_quality="Known",
                right_smpl_offset=600,
                right_smpl_name="Right",
                right_match_quality="Tentative",
            )
        ]

        derived = exact_export.apply_sbnk_member_locations(locations, rows)

        self.assertIn(500, locations)
        self.assertNotIn(600, locations)
        self.assertEqual([row.object_offset for row in derived], [500])

    def test_exact_export_write_helpers_refuse_existing_targets_without_overwrite(self) -> None:
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "rows.json"
            path.write_text("existing", encoding="utf-8")

            with self.assertRaisesRegex(FileExistsError, "refusing to overwrite"):
                exact_export.write_json(path, [self.mono()])

            self.assertEqual(path.read_text(encoding="utf-8"), "existing")

    def test_exact_export_write_helpers_replace_existing_targets_with_overwrite(self) -> None:
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "rows.json"
            path.write_text("existing", encoding="utf-8")

            exact_export.write_json(path, [self.mono()], overwrite_policy="replace")

            self.assertIn('"object_offset"', path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
