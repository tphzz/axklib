import unittest

from axklib.containers import sfs_extents


class MemoryReader:
    def __init__(self, data: dict[int, bytes]) -> None:
        self.data = data

    def read_at(self, offset: int, size: int) -> bytes:
        return self.data.get(offset, b"\x00" * size)[:size]


def put_u16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def put_u32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def cluster_offset(cluster: int) -> int:
    return sfs_extents.cluster_absolute_offset(
        partition_start_sector=3,
        sector_size=512,
        sectors_per_cluster=2,
        cluster_offset=cluster,
    )


class SfsExtentsTests(unittest.TestCase):
    def test_reads_direct_extents_as_one_logical_stream(self) -> None:
        record = bytearray(72)
        put_u16(record, 0x00, 2)
        put_u16(record, 0x04, 2)
        put_u32(record, 0x06, 7)
        put_u32(record, 0x0A, 10)
        put_u32(record, 0x0E, 1)
        put_u32(record, 0x12, 3)
        put_u32(record, 0x16, 20)
        put_u32(record, 0x1A, 1)
        put_u32(record, 0x1E, 4)
        reader = MemoryReader(
            {
                cluster_offset(10): b"abc",
                cluster_offset(20): b"defg",
            }
        )

        result = sfs_extents.read_index_record_data(
            reader,
            bytes(record),
            partition_start_sector=3,
            sector_size=512,
            sectors_per_cluster=2,
            cluster_count_limit=100,
        )

        self.assertEqual(result.data, b"abcdefg")
        self.assertEqual([extent.cluster_offset for extent in result.extents], [10, 20])
        self.assertEqual(result.allocated_bytes, 2048)
        self.assertEqual(result.storage_padding_bytes, 2041)
        self.assertEqual(result.warnings, [])

    def test_reads_continuation_extents(self) -> None:
        record = bytearray(72)
        put_u16(record, 0x00, 5)
        put_u16(record, 0x04, 5)
        put_u32(record, 0x06, 10)
        put_u32(record, 0x0A, 50)
        continuation = bytearray(1024)
        put_u32(continuation, 0x00, 5)
        for index, cluster in enumerate([60, 61, 62, 63, 64]):
            offset = 0x0C + index * 12
            put_u32(continuation, offset, cluster)
            put_u32(continuation, offset + 4, 1)
            put_u32(continuation, offset + 8, 2)
        reader = MemoryReader(
            {
                cluster_offset(50): bytes(continuation),
                cluster_offset(60): b"aa",
                cluster_offset(61): b"bb",
                cluster_offset(62): b"cc",
                cluster_offset(63): b"dd",
                cluster_offset(64): b"ee",
            }
        )

        result = sfs_extents.read_index_record_data(
            reader,
            bytes(record),
            partition_start_sector=3,
            sector_size=512,
            sectors_per_cluster=2,
            cluster_count_limit=100,
        )

        self.assertEqual(result.data, b"aabbccddee")
        self.assertEqual(result.continuation_clusters, [50])
        self.assertEqual([extent.cluster_offset for extent in result.extents], [60, 61, 62, 63, 64])
        self.assertEqual(result.warnings, [])


if __name__ == "__main__":
    unittest.main()

