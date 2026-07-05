import tempfile
import unittest
import wave
from pathlib import Path

from axklib.containers import fat as report_floppy_objects
from axklib.containers import load_objects

SECTOR_SIZE = 512
TOTAL_SECTORS = 2880
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
ROOT_OFFSET = (1 + 2 * SECTORS_PER_FAT) * SECTOR_SIZE
DATA_OFFSET = ROOT_OFFSET + ((ROOT_ENTRIES * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE


def put_le16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "little")


def put_le32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "little")


def put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def set_fat12_entry(buf: bytearray, cluster: int, value: int) -> None:
    for fat_index in range(2):
        fat_offset = (1 + fat_index * SECTORS_PER_FAT) * SECTOR_SIZE
        byte_index = fat_offset + cluster + cluster // 2
        pair = buf[byte_index] | (buf[byte_index + 1] << 8)
        if cluster & 1:
            pair = (pair & 0x000F) | ((value & 0x0FFF) << 4)
        else:
            pair = (pair & 0xF000) | (value & 0x0FFF)
        buf[byte_index] = pair & 0xFF
        buf[byte_index + 1] = pair >> 8


def build_smpl_payload() -> bytes:
    header = bytearray(128)
    header[0:12] = b"FSFSDEV3SPLX"
    header[0x0C:0x10] = b"SMPL"
    put_be32(header, 0x10, 128)
    put_be32(header, 0x1C, 4)
    put_be32(header, 0x20, 4)
    put_be16(header, 0x28, 32000)
    put_be16(header, 0x2A, 2)
    header[0x32 : 0x32 + 4] = b"TEST"
    return bytes(header) + b"\x12\x34\x56\x78"


def build_floppy_image() -> bytes:
    buf = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    buf[0:3] = b"\xeb\x3c\x90"
    buf[3:11] = b"YAMAHA  "
    put_le16(buf, 0x0B, SECTOR_SIZE)
    buf[0x0D] = 1
    put_le16(buf, 0x0E, 1)
    buf[0x10] = 2
    put_le16(buf, 0x11, ROOT_ENTRIES)
    put_le16(buf, 0x13, TOTAL_SECTORS)
    buf[0x15] = 0xF0
    put_le16(buf, 0x16, SECTORS_PER_FAT)
    put_le16(buf, 0x18, 18)
    put_le16(buf, 0x1A, 2)
    buf[0x36:0x3E] = b"FAT12   "
    buf[510:512] = b"\x55\xaa"

    for fat_index in range(2):
        fat_offset = (1 + fat_index * SECTORS_PER_FAT) * SECTOR_SIZE
        buf[fat_offset : fat_offset + 3] = b"\xf0\xff\xff"
    set_fat12_entry(buf, 2, 0xFFF)

    payload = build_smpl_payload()
    root = ROOT_OFFSET
    buf[root : root + 8] = b"SMPTEST "
    buf[root + 8 : root + 11] = b"004"
    buf[root + 0x0B] = 0x20
    put_le16(buf, root + 0x1A, 2)
    put_le32(buf, root + 0x1C, len(payload))
    buf[DATA_OFFSET : DATA_OFFSET + len(payload)] = payload
    return bytes(buf)


class ReportFloppyObjectsTests(unittest.TestCase):
    def test_reports_and_extracts_current_smpl_from_fat_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            image_path = tmp_path / "sample.ima"
            image_path.write_bytes(build_floppy_image())

            rows = report_floppy_objects.scan_floppy(image_path)
            extracted = report_floppy_objects.extract_smpl_rows(rows, tmp_path / "wav")

            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["container_kind"], "fat12_floppy")
            self.assertEqual(rows[0]["fat_file"], "SMPTEST.004")
            self.assertEqual(rows[0]["type"], "SMPL")
            self.assertEqual(rows[0]["header_size"], 128)
            self.assertEqual(rows[0]["payload_bytes_0x1c"], 4)
            self.assertEqual(rows[0]["object_offset"], DATA_OFFSET)
            self.assertEqual(rows[0]["stored_payload_offset"], DATA_OFFSET + 128)

            loaded = load_objects(image_path, "fat12_floppy")
            self.assertEqual(len(loaded), 1)
            self.assertEqual(loaded[0].payload_offset, DATA_OFFSET)
            self.assertEqual(loaded[0].metadata["fat_directory_offset"], ROOT_OFFSET)
            self.assertEqual(loaded[0].metadata["fat_first_cluster"], 2)
            self.assertEqual(loaded[0].metadata["fat_stored_payload_offset"], DATA_OFFSET + 128)

            self.assertEqual(len(extracted), 1)
            self.assertIn("extraction_quality", extracted[0])
            self.assertIn("extraction_basis", extracted[0])
            self.assertIn("extraction_notes", extracted[0])
            self.assertIn("field_quality", extracted[0])
            for field in (
                "header_size",
                "stored_payload_size",
                "stored_payload_transform",
                "sample_rate",
                "sample_width_bytes",
                "channels",
                "name_guess",
            ):
                self.assertIn(field, extracted[0]["field_quality"])
                self.assertIn("quality", extracted[0]["field_quality"][field])
                self.assertIn("basis", extracted[0]["field_quality"][field])
                self.assertIn("notes", extracted[0]["field_quality"][field])
            self.assertEqual(extracted[0]["header_size"], 128)
            self.assertEqual(extracted[0]["stored_payload_size"], 4)
            self.assertEqual(extracted[0]["stored_payload_offset"], DATA_OFFSET + 128)
            self.assertEqual(extracted[0]["stored_payload_transform"], "byteswap16")
            self.assertEqual(extracted[0]["sample_rate"], 32000)
            self.assertEqual(extracted[0]["sample_width_bytes"], 2)
            self.assertEqual(extracted[0]["channels"], 1)
            self.assertEqual(extracted[0]["name_guess"], "TEST")
            with wave.open(extracted[0]["wav_path"], "rb") as wav:
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.getframerate(), 32000)
                self.assertEqual(wav.readframes(wav.getnframes()), b"\x34\x12\x78\x56")


if __name__ == "__main__":
    unittest.main()
