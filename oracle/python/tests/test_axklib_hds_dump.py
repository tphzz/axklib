import gzip
import tempfile
import unittest
from pathlib import Path

from axklib.containers import sfs_dump as dumper

SECTOR_SIZE = 512


def put_u16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def put_u32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def write_superblock(buf: bytearray, sector: int, total_sectors: int) -> None:
    base = sector * SECTOR_SIZE
    buf[base : base + 11] = b"YAMAHA_dev3"
    put_u32(buf, base + 0x09C, SECTOR_SIZE)
    put_u32(buf, base + 0x0A0, total_sectors)
    put_u32(buf, base + 0x0A8, 3)
    put_u32(buf, base + 0x0AC, total_sectors - 3)


def write_partition_header(buf: bytearray, sector: int) -> None:
    base = sector * SECTOR_SIZE
    buf[base : base + 11] = b"YAMAHA_dev3"
    buf[base + 0x040 : base + 0x050] = b"New Partition   "
    put_u32(buf, base + 0x080, 2)
    put_u32(buf, base + 0x084, 200)
    buf[base + 0x088 : base + 0x090] = b"\xff" * 8
    put_u32(buf, base + 0x090, 1022)
    put_u32(buf, base + 0x094, 2)
    put_u32(buf, base + 0x098, 2)
    put_u32(buf, base + 0x09C, 3)
    put_u32(buf, base + 0x0A0, 0x1394)
    put_u32(buf, base + 0x0A4, 4)
    put_u32(buf, base + 0x0A8, 0x0166)
    buf[base + 1024 : base + 2048] = buf[base : base + 1024]


def write_node(
    buf: bytearray, offset: int, node_size: int, cluster_offset: int, data_size: int
) -> None:
    put_u16(buf, offset + 0x00, 1)
    put_u16(buf, offset + 0x02, 0)
    put_u16(buf, offset + 0x04, 2)
    put_u16(buf, offset + 0x06, 0)
    put_u16(buf, offset + 0x08, node_size)
    put_u32(buf, offset + 0x0A, cluster_offset)
    put_u32(buf, offset + 0x0E, 2)
    put_u32(buf, offset + 0x12, data_size)
    put_u32(buf, offset + 0x16, 0)
    put_u16(buf, offset + 0x20, 1)


def write_dir_entry(buf: bytearray, offset: int, name: bytes, link: int) -> None:
    encoded_name = name + b"\x00"
    put_u16(buf, offset + 0x00, 0x20)
    put_u16(buf, offset + 0x02, len(encoded_name))
    put_u32(buf, offset + 0x04, link)
    buf[offset + 0x08 : offset + 0x08 + len(encoded_name)] = encoded_name


def build_minimal_sfs_image() -> bytes:
    total_sectors = 2048
    buf = bytearray(total_sectors * SECTOR_SIZE)
    write_superblock(buf, 0, total_sectors)
    write_superblock(buf, 1, total_sectors)
    write_partition_header(buf, 3)

    directory_index_offset = 11 * SECTOR_SIZE
    write_node(buf, directory_index_offset, 128, 362, 128)
    write_node(buf, directory_index_offset + 72, 128, 363, 128)

    directory_payload_offset = 729 * SECTOR_SIZE
    write_dir_entry(buf, directory_payload_offset + 0, b".", 1)
    write_dir_entry(buf, directory_payload_offset + 32, b"..", 1)
    write_dir_entry(buf, directory_payload_offset + 64, b"sfserrlog", 2)
    write_dir_entry(buf, directory_payload_offset + 96, b"sfserram", 0)
    return bytes(buf)


class AxklibHdsDumpTests(unittest.TestCase):
    def test_parse_minimal_sfs_geometry_and_nodes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.hds"
            path.write_bytes(build_minimal_sfs_image())

            parsed = dumper.parse_image(path)

        self.assertEqual(parsed["classification"]["kind"], "yamaha_sfs")
        self.assertEqual(parsed["sector_size_bytes"], SECTOR_SIZE)
        self.assertTrue(parsed["superblock_backup_matches_primary"])
        partition = parsed["partitions"][0]
        self.assertEqual(partition["name"], "New Partition")
        self.assertEqual(partition["derived"]["bitmap_absolute_sector"], 9)
        self.assertEqual(partition["derived"]["directory_index_absolute_sector"], 11)
        self.assertEqual(partition["nodes"][0]["payload_absolute_sector"], 727)
        self.assertEqual(partition["nodes"][1]["payload_absolute_sector"], 729)
        self.assertEqual(
            [entry["name"] for entry in partition["nodes"][1]["directory_entries_guess"]],
            [".", "..", "sfserrlog", "sfserram"],
        )

    def test_gzip_image_uses_same_parser(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.img.gz"
            with gzip.open(path, "wb") as fh:
                fh.write(build_minimal_sfs_image())

            parsed = dumper.parse_image(path, dumper.ReadOptions(max_nodes=4))

        self.assertEqual(parsed["container"], "gzip")
        self.assertEqual(parsed["classification"]["kind"], "yamaha_sfs")
        self.assertEqual(parsed["partitions"][0]["nodes"][1]["node_size"], 128)

    def test_non_sfs_ima_is_classified_not_parsed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.IMA"
            sector = bytearray(SECTOR_SIZE)
            sector[:11] = b"\xebD\x90YAMAHA  "
            sector[54:62] = b"FAT12   "
            sector[510:512] = b"\x55\xaa"
            path.write_bytes(bytes(sector) + b"\x00" * (1474560 - SECTOR_SIZE))

            parsed = dumper.parse_image(path)

        self.assertEqual(parsed["classification"]["kind"], "non_sfs")
        self.assertIn("DOS boot signature", " ".join(parsed["warnings"]))
        self.assertNotIn("partitions", parsed)


if __name__ == "__main__":
    unittest.main()
