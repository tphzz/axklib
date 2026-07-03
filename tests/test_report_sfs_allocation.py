import unittest

from axklib.containers import sfs_allocation


class ReportSfsAllocationTests(unittest.TestCase):
    def test_bitmap_bits_are_msb_first(self) -> None:
        data = bytearray(2)

        sfs_allocation.bitmap_set(data, 0)
        sfs_allocation.bitmap_set(data, 7)
        sfs_allocation.bitmap_set(data, 8)

        self.assertEqual(data, bytearray([0x81, 0x80]))
        self.assertTrue(sfs_allocation.bitmap_test(data, 0))
        self.assertTrue(sfs_allocation.bitmap_test(data, 7))
        self.assertTrue(sfs_allocation.bitmap_test(data, 8))
        self.assertFalse(sfs_allocation.bitmap_test(data, 1))

    def test_direct_extents_read_12_byte_triplets_from_record(self) -> None:
        record = bytearray(72)
        record[0x0A:0x0E] = (100).to_bytes(4, "big")
        record[0x0E:0x12] = (2).to_bytes(4, "big")
        record[0x12:0x16] = (2048).to_bytes(4, "big")
        record[0x16:0x1A] = (200).to_bytes(4, "big")
        record[0x1A:0x1E] = (3).to_bytes(4, "big")
        record[0x1E:0x22] = (123).to_bytes(4, "big")

        self.assertEqual(
            sfs_allocation.direct_extents(record, 2),
            [(100, 2, 2048), (200, 3, 123)],
        )

    def test_mismatch_ranges_are_inclusive(self) -> None:
        left = bytearray(2)
        right = bytearray(2)
        for cluster in [1, 2, 3, 6, 10]:
            sfs_allocation.bitmap_set(left, cluster)
        sfs_allocation.bitmap_set(right, 2)

        self.assertEqual(
            sfs_allocation.mismatch_ranges(left, right, 16),
            [(1, 1), (3, 3), (6, 6), (10, 10)],
        )

    def test_range_count_sums_inclusive_lengths(self) -> None:
        self.assertEqual(sfs_allocation.range_count([(1, 3), (10, 10)]), 4)


if __name__ == "__main__":
    unittest.main()

