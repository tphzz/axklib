import tempfile
import unittest
from pathlib import Path

from axklib.containers import sfs_inventory as report_sfs_inventory


class ReportSfsInventoryTests(unittest.TestCase):
    def test_parse_directory_entries_reads_32_byte_entries(self) -> None:
        payload = bytearray(64)
        payload[0:2] = (0x20).to_bytes(2, "big")
        payload[2:4] = (5).to_bytes(2, "big")
        payload[4:8] = (123).to_bytes(4, "big")
        payload[8:13] = b"SMPL\x00"

        entries = report_sfs_inventory.parse_directory_entries(payload)

        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["name"], "SMPL")
        self.assertEqual(entries[0]["link_id"], 123)

    def test_parse_directory_entries_keeps_entries_before_trailing_non_entry_data(self) -> None:
        payload = bytearray(96)
        payload[0:2] = (0x20).to_bytes(2, "big")
        payload[2:4] = (2).to_bytes(2, "big")
        payload[4:8] = (1).to_bytes(4, "big")
        payload[8:10] = b".\x00"
        payload[32:34] = (0x20).to_bytes(2, "big")
        payload[34:36] = (11).to_bytes(2, "big")
        payload[36:40] = (2).to_bytes(4, "big")
        payload[40:51] = b"Gong Bali\x00"
        payload[64:66] = (0x20).to_bytes(2, "big")
        payload[66:68] = (0x80).to_bytes(2, "big")

        entries = report_sfs_inventory.parse_directory_entries(payload)

        self.assertEqual([entry["name"] for entry in entries], [".", "Gong Bali"])

    def test_parse_index_record_accepts_multi_extent_object_records(self) -> None:
        record = bytearray(72)
        record[0:2] = (3).to_bytes(2, "big")
        record[4:6] = (600).to_bytes(2, "big")
        record[6:10] = (412430).to_bytes(4, "big")
        record[10:14] = (10400).to_bytes(4, "big")
        record[14:18] = (200).to_bytes(4, "big")
        record[18:22] = (204800).to_bytes(4, "big")

        parsed = report_sfs_inventory.parse_index_record(
            record,
            partition_index=0,
            record_offset=0,
            record_offset_in_index=0,
            partition_start_sector=3,
            sector_size=512,
            sectors_per_cluster=2,
        )

        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.extent_count, 3)
        self.assertEqual(parsed.cluster_count, 600)
        self.assertEqual(parsed.data_size, 412430)
        self.assertEqual(parsed.cluster_offset, 10400)
        self.assertEqual(parsed.payload_sector, 20803)
        self.assertEqual(parsed.payload_offset, 10651136)

    def test_continuation_extents_parse_counted_triplets(self) -> None:
        payload = bytearray(0x40)
        payload[0:4] = (2).to_bytes(4, "big")
        payload[0x0C:0x10] = (26800).to_bytes(4, "big")
        payload[0x10:0x14] = (200).to_bytes(4, "big")
        payload[0x14:0x18] = (204800).to_bytes(4, "big")
        payload[0x18:0x1C] = (27000).to_bytes(4, "big")
        payload[0x1C:0x20] = (200).to_bytes(4, "big")
        payload[0x20:0x24] = (12345).to_bytes(4, "big")

        self.assertEqual(
            report_sfs_inventory.continuation_extents(payload, 2),
            [(26800, 200, 204800), (27000, 200, 12345)],
        )

    def test_load_directory_entries_reassembles_noncontiguous_extents(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "image.hda"
            data = bytearray(24 * 1024)
            index_record = bytearray(72)
            index_record[0:2] = (2).to_bytes(2, "big")
            index_record[4:6] = (2).to_bytes(2, "big")
            index_record[6:10] = (64).to_bytes(4, "big")
            index_record[0x0A:0x0E] = (10).to_bytes(4, "big")
            index_record[0x0E:0x12] = (1).to_bytes(4, "big")
            index_record[0x12:0x16] = (32).to_bytes(4, "big")
            index_record[0x16:0x1A] = (20).to_bytes(4, "big")
            index_record[0x1A:0x1E] = (1).to_bytes(4, "big")
            index_record[0x1E:0x22] = (32).to_bytes(4, "big")
            data[0:72] = index_record

            first_entry = bytearray(32)
            first_entry[0:2] = (0x20).to_bytes(2, "big")
            first_entry[2:4] = (2).to_bytes(2, "big")
            first_entry[4:8] = (777).to_bytes(4, "big")
            first_entry[8:10] = b".\x00"
            second_entry = bytearray(32)
            second_entry[0:2] = (0x20).to_bytes(2, "big")
            second_entry[2:4] = (5).to_bytes(2, "big")
            second_entry[4:8] = (42).to_bytes(4, "big")
            second_entry[8:13] = b"SMPL\x00"

            data[10 * 1024 : 10 * 1024 + 32] = first_entry
            data[10 * 1024 + 32 : 10 * 1024 + 64] = b"not logical directory data here"
            data[20 * 1024 : 20 * 1024 + 32] = second_entry
            image.write_bytes(data)

            record = report_sfs_inventory.IndexRecord(
                0, 777, 0, 0, 2, 2, 64, 10, 10, 1, 32, None, 10 * 1024, 20, "directory", "", "", 777, None, 0
            )
            partition = {
                "index": 0,
                "start_sector": 0,
                "fields": {
                    "sectors_per_cluster": {"value": 2},
                    "number_of_clusters": {"value": 128},
                },
            }

            entries = report_sfs_inventory.load_directory_entries(image, record, partition)

        self.assertEqual([entry["name"] for entry in entries], [".", "SMPL"])

    def test_index_record_offsets_map_to_sfs_ids_with_block_overhead(self) -> None:
        self.assertEqual(report_sfs_inventory.index_record_offset_to_sfs_id(72), 1)
        self.assertEqual(report_sfs_inventory.index_record_offset_to_sfs_id(2552), 35)
        self.assertEqual(report_sfs_inventory.index_record_offset_to_sfs_id(1024 + 13 * 72), 27)
        self.assertIsNone(report_sfs_inventory.index_record_offset_to_sfs_id(1008))

    def test_yamaha_volume_reports_group_categories_and_objects(self) -> None:
        partitions = [
            report_sfs_inventory.PartitionReport(
                source_image="image.hda",
                partition_index=0,
                partition_name="hd1",
                start_sector=3,
                sector_count=100,
                directory_index_offset=0,
                first_object_offset=0,
                scanned_index_bytes=0,
                directory_record_count=0,
                object_record_count=0,
                top_level_volume_count=1,
            )
        ]
        directories = [
            report_sfs_inventory.DirectoryReport("image.hda", 0, 3, 1, "/A4K Disk 1", 0, 0, 1, 1, 0),
            report_sfs_inventory.DirectoryReport("image.hda", 0, 4, 3, "/A4K Disk 1/SMPL", 0, 0, 1, 0, 1),
        ]
        entries = [
            report_sfs_inventory.DirectoryEntry(
                "image.hda", 0, 3, "/A4K Disk 1", 0, 0x20, 5, 4, "SMPL", "directory", 4, "", None, "", "directory-id"
            ),
            report_sfs_inventory.DirectoryEntry(
                "image.hda", 0, 4, "/A4K Disk 1/SMPL", 0, 0x20, 8, 5, "Sample1", "object", None, "SMPL", 0x1000, "Sample1", "name+type"
            ),
        ]

        volumes, categories, objects = report_sfs_inventory.yamaha_volume_reports(partitions, directories, entries)

        self.assertEqual(volumes[0].volume_name, "A4K Disk 1")
        self.assertEqual(categories[0].category_name, "Samples")
        self.assertEqual(objects[0].object_offset, 0x1000)

    def test_update_ynode_visibility_marks_hidden_and_referenced_unknown(self) -> None:
        hidden = report_sfs_inventory.YNodeRecord(
            0, 0, 0, 0, 1, 32, 32768, 424, 424, 32, 32768, None, 0, 0, "unknown", "", "", None, None, 0
        )
        referenced_unknown = report_sfs_inventory.YNodeRecord(
            0, 12, 0, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0, 0, "unknown", "", "", None, None, 0
        )
        entry = report_sfs_inventory.DirectoryEntry(
            "image.hda", 0, 1, "/", 0, 0x20, 5, 12, "X", "object", None, "", None, "", "unmatched"
        )

        report_sfs_inventory.update_ynode_visibility([hidden, referenced_unknown], [entry])

        self.assertEqual(hidden.visibility, "hidden-system")
        self.assertEqual(referenced_unknown.visibility, "referenced-unknown")
        self.assertEqual(referenced_unknown.link_reference_count, 1)

    def test_choose_object_record_explains_unknown_link_target(self) -> None:
        target = report_sfs_inventory.YNodeRecord(
            0, 9, 1234, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0, 0, "unknown", "", "", None, None, 0
        )

        record, method, reason, ynode, candidates = report_sfs_inventory.choose_object_record(
            name="Missing",
            expected_type="SMPL",
            link_id=9,
            records_by_sfs_id={},
            ynodes_by_sfs_id={9: target},
            records=[],
            used_offsets=set(),
        )

        self.assertIsNone(record)
        self.assertEqual(method, "unmatched")
        self.assertEqual(reason, "link-id-target-unknown")
        self.assertIs(ynode, target)
        self.assertEqual(candidates, [])

    def test_classify_payload_accepts_legacy_marker_lane_objects(self) -> None:
        payload = bytearray(512)
        payload[:16] = bytes.fromhex("46 55 46 aa 44 55 56 aa 53 55 4c aa 53 55 50 aa")

        payload_kind, object_type, object_name, directory_id, parent_id, entry_count = (
            report_sfs_inventory.classify_payload(bytes(payload))
        )

        self.assertEqual(payload_kind, "legacy-object")
        self.assertEqual(object_type, "SMPL")
        self.assertEqual(object_name, "")
        self.assertIsNone(directory_id)
        self.assertIsNone(parent_id)
        self.assertEqual(entry_count, 0)

    def test_choose_object_record_resolves_exact_legacy_link_target(self) -> None:
        target = report_sfs_inventory.YNodeRecord(
            0, 9, 1234, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0, 0, "legacy-object", "SMPL", "", None, None, 0
        )
        record = report_sfs_inventory.ynode_to_index_record(target)

        resolved, method, reason, ynode, candidates = report_sfs_inventory.choose_object_record(
            name="Mark Tree Up 1",
            expected_type="SMPL",
            link_id=9,
            records_by_sfs_id={9: record},
            ynodes_by_sfs_id={9: target},
            records=[record],
            used_offsets=set(),
        )

        self.assertIs(resolved, record)
        self.assertEqual(method, "link-id+legacy-type")
        self.assertEqual(reason, "")
        self.assertIs(ynode, target)
        self.assertEqual(candidates, [])

    def test_choose_object_record_does_not_name_match_blank_legacy_records(self) -> None:
        target = report_sfs_inventory.YNodeRecord(
            0, 9, 1234, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0, 0, "legacy-object", "SMPL", "", None, None, 0
        )
        record = report_sfs_inventory.ynode_to_index_record(target)

        resolved, method, reason, ynode, candidates = report_sfs_inventory.choose_object_record(
            name="",
            expected_type="SMPL",
            link_id=0,
            records_by_sfs_id={9: record},
            ynodes_by_sfs_id={0: report_sfs_inventory.YNodeRecord(
                0, 0, 0, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0, 0, "unknown", "", "", None, None, 0
            )},
            records=[record],
            used_offsets=set(),
        )

        self.assertIsNone(resolved)
        self.assertEqual(method, "unmatched")
        self.assertEqual(reason, "link-id-target-unknown")
        self.assertIsNot(ynode, target)
        self.assertEqual(candidates, [])

    def test_choose_object_record_preserves_ambiguous_name_candidates_without_target(self) -> None:
        first = report_sfs_inventory.IndexRecord(
            0, 20, 0x2000, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0x10000, 0, "object", "SMPL", "Kick", None, None, 0
        )
        second = report_sfs_inventory.IndexRecord(
            0, 21, 0x2100, 0, 1, 1, 1024, 501, 501, 1, 1024, None, 0x11000, 0, "object", "SMPL", "Kick", None, None, 0
        )

        resolved, method, reason, ynode, candidates = report_sfs_inventory.choose_object_record(
            name="Kick",
            expected_type="SMPL",
            link_id=999,
            records_by_sfs_id={},
            ynodes_by_sfs_id={},
            records=[second, first],
            used_offsets=set(),
        )

        self.assertIsNone(resolved)
        self.assertEqual(method, "name+type-ambiguous")
        self.assertEqual(reason, "link-id-missing")
        self.assertIsNone(ynode)
        self.assertEqual([candidate.sfs_id for candidate in candidates], [20, 21])

    def test_choose_object_record_preserves_single_name_candidate_without_target(self) -> None:
        candidate = report_sfs_inventory.IndexRecord(
            0, 20, 0x2000, 0, 1, 1, 1024, 500, 500, 1, 1024, None, 0x10000, 0, "object", "SMPL", "Kick", None, None, 0
        )

        resolved, method, reason, _ynode, candidates = report_sfs_inventory.choose_object_record(
            name="Kick",
            expected_type="SMPL",
            link_id=999,
            records_by_sfs_id={},
            ynodes_by_sfs_id={},
            records=[candidate],
            used_offsets=set(),
        )

        self.assertIsNone(resolved)
        self.assertEqual(method, "name+type-candidate")
        self.assertEqual(reason, "link-id-missing")
        self.assertEqual(candidates, [candidate])


if __name__ == "__main__":
    unittest.main()

