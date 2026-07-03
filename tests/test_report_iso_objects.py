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
            self.assertIsNone(items[0].payload_offset)
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
if __name__ == "__main__":
    unittest.main()



