from pathlib import Path

import axklib
from axklib.objects import current as objects


def _standalone_payload(name: bytes = b"TestSample") -> bytes:
    payload = bytearray(0x42)
    payload[0 : len(objects.OBJECT_MAGIC)] = objects.OBJECT_MAGIC
    payload[0x0C:0x10] = b"SMPL"
    payload[0x10:0x14] = (0x42).to_bytes(4, "big")
    payload[0x1C:0x20] = (0).to_bytes(4, "big")
    payload[0x20:0x24] = (0).to_bytes(4, "big")
    payload[0x32 : 0x32 + len(name)] = name
    return bytes(payload)


def test_open_loads_standalone_object(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    source.write_bytes(_standalone_payload())

    container = axklib.open(source)

    assert container.kind == "standalone_object"
    assert len(container.objects) == 1
    assert container.objects[0].type == "SMPL"
    assert container.objects[0].name == "TestSample"


def test_open_many_returns_structured_error_for_unsupported_file(tmp_path: Path) -> None:
    source = tmp_path / "not-axklib.bin"
    source.write_bytes(b"not an axklib container")

    results = axklib.open_many([source])

    assert len(results) == 1
    assert results[0].container is None
    assert results[0].error is not None
    assert results[0].error.error_code == "AXKLIB_CONTAINER_OPEN_FAILED"
