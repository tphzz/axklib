import tempfile
import unittest
from io import BytesIO
from pathlib import Path

from axklib.containers import OpenOptions, iso_lowlevel, load_objects, open_many

SECTOR_SIZE = 2048


def put_le32_both(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "little")
    buf[offset + 4 : offset + 8] = value.to_bytes(4, "big")


def put_le16_both(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "little")
    buf[offset + 2 : offset + 4] = value.to_bytes(2, "big")


def directory_record(name: bytes, extent: int, size: int, flags: int) -> bytes:
    length = 33 + len(name)
    if length % 2:
        length += 1
    rec = bytearray(length)
    rec[0] = length
    put_le32_both(rec, 2, extent)
    put_le32_both(rec, 10, size)
    rec[18:25] = b"\x7d\x01\x01\x00\x00\x00\x00"
    rec[25] = flags
    put_le16_both(rec, 28, 1)
    rec[32] = len(name)
    rec[33 : 33 + len(name)] = name
    return bytes(rec)


def build_smpl_object() -> bytes:
    header = bytearray(0x200)
    header[0:12] = b"FSFSDEV3SPLX"
    header[0x0C:0x10] = b"SMPL"
    header[0x10:0x14] = (0x200).to_bytes(4, "big")
    header[0x18:0x1C] = (0x7C).to_bytes(4, "big")
    header[0x1C:0x20] = (4).to_bytes(4, "big")
    header[0x20:0x24] = (4).to_bytes(4, "big")
    header[0x28:0x2A] = (44100).to_bytes(2, "big")
    header[0x2A:0x2C] = (2).to_bytes(2, "big")
    header[0x32:0x36] = b"TEST"
    return bytes(header) + b"\x12\x34\x56\x78"


def build_raw_sbac_impossible_capacity_object() -> bytes:
    payload = bytearray(0x200)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SBAC"
    payload[0x32:0x3A] = b"P5 RCDR "
    payload[0x144] = 80
    return bytes(payload)


def write_iso(path: Path) -> None:
    import pycdlib

    smpl = build_smpl_object()
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, vol_ident="TESTVOL")
    iso.add_fp(BytesIO(smpl), len(smpl), iso_path="/F001.;1")
    iso.write(str(path))
    iso.close()


class ReportIsoObjectsTests(unittest.TestCase):
    def test_walks_iso_and_hashes_smpl_payload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.iso"
            write_iso(path)

            volume_id, rows = iso_lowlevel.walk_iso_files(path)
            hashes, strings = iso_lowlevel.build_hash_rows(path, volume_id, rows)

            self.assertEqual(volume_id, "TESTVOL")
            objects = [row for row in rows if row.object_type]
            self.assertEqual(len(objects), 1)
            self.assertEqual(objects[0].path, "F001")
            self.assertEqual(objects[0].object_type, "SMPL")
            self.assertEqual(objects[0].inventory_method, "iso9660")
            self.assertEqual(len(hashes), 1)
            self.assertEqual(hashes[0].stored_payload_size, 4)
            self.assertEqual(hashes[0].high_byte_lane_size, 2)
            self.assertEqual(hashes[0].low_byte_lane_size, 2)
            self.assertEqual(hashes[0].stored_payload_transform, "byteswap16")
            self.assertEqual(strings[0].inventory_method, "iso9660")
            self.assertEqual(objects[0].inventory_status, "iso9660")
            self.assertEqual(objects[0].iso_recovery_quality, "clean-iso9660-object")
            self.assertEqual(hashes[0].inventory_status, "iso9660")
            self.assertEqual(hashes[0].iso_recovery_quality, "clean-iso9660-object")
            self.assertEqual(strings[0].iso_recovery_quality, "clean-iso9660-object")

    def test_shared_object_source_loads_iso_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.iso"
            write_iso(path)

            items = load_objects(path, "iso")

            self.assertEqual(len(items), 1)
            self.assertEqual(items[0].container_kind, "iso")
            self.assertEqual(items[0].type, "SMPL")
            self.assertEqual(items[0].name, "TEST")
            self.assertEqual(items[0].fat_file, "F001")
            self.assertIsInstance(items[0].payload_offset, int)
            self.assertEqual(items[0].metadata["iso_data_offset"], items[0].payload_offset)
            self.assertEqual(
                items[0].metadata["iso_extent_sector"], items[0].payload_offset // SECTOR_SIZE
            )
            self.assertEqual(items[0].payload_size, len(build_smpl_object()))
            self.assertIn("TESTVOL", items[0].scope_key)
            self.assertIn("iso9660", items[0].object_key)
            self.assertEqual(items[0].metadata["iso_inventory_method"], "iso9660")
            self.assertEqual(items[0].metadata["iso_inventory_status"], "iso9660")
            self.assertEqual(items[0].metadata["iso_recovery_quality"], "clean-iso9660-object")

    def test_shared_object_source_rejects_raw_only_iso_without_recovery(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "raw-only.iso"
            path.write_bytes(build_raw_sbac_impossible_capacity_object())

            result = open_many([path], options=OpenOptions(include_payloads=True))[0]

            self.assertIsNone(result.container)
            self.assertIsNotNone(result.error)
            assert result.error is not None
            self.assertEqual(result.error.error_code, "CONTAINER_UNSUPPORTED_ISO9660")
            self.assertIn("unsupported ISO9660 image", result.error.message)

    def test_decodes_yamaha_cdrom_menu_labels(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "labels.bin"
            image = bytearray(SECTOR_SIZE * 4)
            table = bytearray(64)
            table[0] = 0xDD
            table[1:15] = b"Ep31 CP80     "
            table[15:18] = b"5me"
            table[18:22] = b"F001"
            table[32] = 0xAA
            table[33:47] = b"Ignored       "
            table[50:54] = b"F999"
            image[SECTOR_SIZE : SECTOR_SIZE + len(table)] = table
            image[SECTOR_SIZE * 2 : SECTOR_SIZE * 2 + 16] = b"PIANO/KEYS/SYNTH"
            path.write_bytes(image)

            rows = [
                iso_lowlevel.IsoFileRow(
                    image=str(path),
                    volume_id="TEST",
                    path="GROUP/F001",
                    extent_sector=10,
                    data_offset=SECTOR_SIZE * 3,
                    size=0,
                    is_directory=True,
                    known_type=False,
                    object_type="",
                    name_guess="",
                    inventory_method="iso9660",
                ),
                iso_lowlevel.IsoFileRow(
                    image=str(path),
                    volume_id="TEST",
                    path="GROUP/0000",
                    extent_sector=1,
                    data_offset=SECTOR_SIZE,
                    size=len(table),
                    is_directory=False,
                    known_type=False,
                    object_type="",
                    name_guess="",
                    inventory_method="iso9660",
                ),
                iso_lowlevel.IsoFileRow(
                    image=str(path),
                    volume_id="TEST",
                    path="GROUP/F002",
                    extent_sector=2,
                    data_offset=SECTOR_SIZE * 2,
                    size=16,
                    is_directory=False,
                    known_type=False,
                    object_type="",
                    name_guess="",
                    inventory_method="iso9660",
                ),
            ]

            labels = iso_lowlevel.decode_yamaha_menu_labels(path, rows)

            self.assertEqual(labels.group_labels, {"GROUP": "PIANO/KEYS/SYNTH"})
            self.assertEqual(labels.volume_labels, {("GROUP", "F001"): "Ep31 CP80"})


if __name__ == "__main__":
    unittest.main()
