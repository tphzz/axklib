import csv
import tempfile
import unittest
from pathlib import Path

from axklib.validation import volume as report_sfs_volume_validation


def write_rows(path: Path, fieldnames: list[str], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


class ReportSfsVolumeValidationTests(unittest.TestCase):
    def write_minimal_inventory(
        self,
        root: Path,
        *,
        malformed_entry: bool = False,
    ) -> None:
        write_rows(
            root / "volumes.csv",
            [
                "source_image",
                "partition_index",
                "partition_name",
                "volume_name",
                "volume_path",
                "directory_id",
                "category_count",
                "object_entry_count",
                "matched_object_count",
            ],
            [
                {
                    "source_image": "image.hda",
                    "partition_index": 1,
                    "partition_name": "hd1",
                    "volume_name": "Volume A",
                    "volume_path": "/Volume A",
                    "directory_id": 10,
                    "category_count": 1,
                    "object_entry_count": 1,
                    "matched_object_count": 0 if malformed_entry else 1,
                }
            ],
        )
        write_rows(
            root / "volume_categories.csv",
            [
                "source_image",
                "partition_index",
                "partition_name",
                "volume_name",
                "volume_path",
                "category_code",
                "category_name",
                "directory_id",
                "entry_count",
                "object_entry_count",
                "matched_object_count",
            ],
            [
                {
                    "source_image": "image.hda",
                    "partition_index": 1,
                    "partition_name": "hd1",
                    "volume_name": "Volume A",
                    "volume_path": "/Volume A",
                    "category_code": "SMPL",
                    "category_name": "Samples",
                    "directory_id": 11,
                    "entry_count": 3,
                    "object_entry_count": 1,
                    "matched_object_count": 0 if malformed_entry else 1,
                }
            ],
        )
        if malformed_entry:
            entry = {
                "entry_offset": 2048,
                "entry_flags": 0,
                "name_length_including_nul": 5,
                "link_id": 0,
                "name": "",
                "target_kind": "object",
                "target_directory_id": "",
                "target_object_type": "",
                "target_object_offset": "",
                "target_object_name": "",
                "match_method": "unmatched",
                "target_sfs_id": 0,
                "target_record_offset": 69120,
                "target_payload_kind": "unknown",
                "unmatched_reason": "link-id-target-unknown",
                "match_quality": "Unknown",
            }
        else:
            entry = {
                "entry_offset": 2048,
                "entry_flags": 32,
                "name_length_including_nul": 8,
                "link_id": 12,
                "name": "Sample1",
                "target_kind": "object",
                "target_directory_id": "",
                "target_object_type": "SMPL",
                "target_object_offset": 123456,
                "target_object_name": "Sample1",
                "match_method": "link-id+type",
                "target_sfs_id": 12,
                "target_record_offset": 70000,
                "target_payload_kind": "object",
                "unmatched_reason": "",
                "match_quality": "Known",
            }
        write_rows(
            root / "directory_entries.csv",
            [
                "source_image",
                "partition_index",
                "directory_id",
                "directory_path",
                "entry_offset",
                "entry_flags",
                "name_length_including_nul",
                "link_id",
                "name",
                "target_kind",
                "target_directory_id",
                "target_object_type",
                "target_object_offset",
                "target_object_name",
                "match_method",
                "target_sfs_id",
                "target_record_offset",
                "target_payload_kind",
                "unmatched_reason",
                "match_quality",
            ],
            [
                {
                    "source_image": "image.hda",
                    "partition_index": 1,
                    "directory_id": 11,
                    "directory_path": "/Volume A/SMPL",
                    **entry,
                }
            ],
        )

    def test_malformed_category_entry_fails_volume(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            inventory_dir = tmp / "inventory"
            output_dir = tmp / "out"
            self.write_minimal_inventory(inventory_dir, malformed_entry=True)

            rows, issues, summary = report_sfs_volume_validation.build_report(
                inventory_dir=inventory_dir,
                output_dir=output_dir,
            )

            self.assertEqual(summary.fail_count, 1)
            self.assertEqual(rows[0].validation_status, "Fail")
            self.assertTrue(any(issue.issue_type == "malformed-category-entry" for issue in issues))

    def test_clean_category_entry_passes_volume(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            inventory_dir = tmp / "inventory"
            output_dir = tmp / "out"
            self.write_minimal_inventory(inventory_dir, malformed_entry=False)

            rows, issues, summary = report_sfs_volume_validation.build_report(
                inventory_dir=inventory_dir,
                output_dir=output_dir,
            )

            self.assertEqual(summary.pass_count, 1)
            self.assertEqual(rows[0].validation_status, "Pass")
            self.assertEqual(issues, [])

    def test_visible_legacy_marker_lane_entries_warn_volume(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            inventory_dir = tmp / "inventory"
            output_dir = tmp / "out"
            self.write_minimal_inventory(inventory_dir, malformed_entry=False)
            write_rows(
                inventory_dir / "volume_objects.csv",
                [
                    "source_image",
                    "partition_index",
                    "volume_path",
                    "category_code",
                    "match_method",
                ],
                [
                    {
                        "source_image": "image.hda",
                        "partition_index": 1,
                        "volume_path": "/Volume A",
                        "category_code": "SMPL",
                        "match_method": "link-id+type",
                    },
                    {
                        "source_image": "image.hda",
                        "partition_index": 1,
                        "volume_path": "/Volume A",
                        "category_code": "SMPL",
                        "match_method": "link-id+legacy-type",
                    },
                    {
                        "source_image": "image.hda",
                        "partition_index": 1,
                        "volume_path": "/Volume A",
                        "category_code": "SBNK",
                        "match_method": "link-id+legacy-type",
                    },
                ],
            )

            rows, issues, summary = report_sfs_volume_validation.build_report(
                inventory_dir=inventory_dir,
                output_dir=output_dir,
            )

            self.assertEqual(summary.warn_count, 1)
            self.assertEqual(rows[0].validation_status, "Warn")
            self.assertEqual(rows[0].current_object_entry_count, 1)
            self.assertEqual(rows[0].legacy_marker_lane_object_entry_count, 2)
            self.assertEqual(rows[0].legacy_marker_lane_smpl_entry_count, 1)
            self.assertTrue(any(issue.issue_type == "visible-marker-lane-conversion-artifact-objects" for issue in issues))

    def test_allocation_summary_can_fail_volume(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            inventory_dir = tmp / "inventory"
            allocation_dir = tmp / "allocation"
            output_dir = tmp / "out"
            self.write_minimal_inventory(inventory_dir, malformed_entry=False)
            write_rows(
                allocation_dir / "allocation_summary.csv",
                [
                    "source_image",
                    "partition_index",
                    "stored_used_not_reconstructed_count",
                    "reconstructed_used_not_stored_count",
                    "extent_total_mismatch_count",
                    "warning_count",
                    "warnings",
                ],
                [
                    {
                        "source_image": "image.hda",
                        "partition_index": 1,
                        "stored_used_not_reconstructed_count": 1,
                        "reconstructed_used_not_stored_count": 0,
                        "extent_total_mismatch_count": 0,
                        "warning_count": 0,
                        "warnings": "",
                    }
                ],
            )

            rows, issues, summary = report_sfs_volume_validation.build_report(
                inventory_dir=inventory_dir,
                allocation_dir=allocation_dir,
                output_dir=output_dir,
            )

            self.assertEqual(summary.fail_count, 1)
            self.assertEqual(rows[0].allocation_status, "Fail")
            self.assertTrue(any(issue.issue_type == "partition-allocation-consistency" for issue in issues))


if __name__ == "__main__":
    unittest.main()


