from __future__ import annotations

import hashlib
import io
import struct
import tarfile
import zipfile
from pathlib import Path

import pytest

import release_metadata
import version_metadata

COMMIT = "8c783a71369a41768ae848830e06cc92c873fb3f"
IDENTITY = "0.6.0-pre-8c783a7"


def branch_version() -> version_metadata.VersionMetadata:
    return version_metadata.VersionMetadata(1, "0.6.0-pre", "0.6.0", 0, 6, 0, "", False, True)


def installer_bytes(extension: str) -> bytes:
    if extension == ".deb":

        def member(name: str, contents: bytes) -> bytes:
            header = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(contents):<10}`\n"
            return header.encode() + contents + (b"\n" if len(contents) % 2 else b"")

        return b"!<arch>\n" + b"".join(
            member(name, contents)
            for name, contents in (
                ("debian-binary", b"2.0\n"),
                ("control.tar.gz", b"control"),
                ("data.tar.gz", b"data"),
            )
        )
    if extension == ".rpm":
        lead = b"\xed\xab\xee\xdb\x03\x00" + bytes(90)
        header = b"\x8e\xad\xe8\x01" + bytes(12)
        return lead + header + header + b"payload"
    if extension == ".exe":
        dos = bytearray(64)
        dos[:2] = b"MZ"
        struct.pack_into("<I", dos, 60, 64)
        # PE32 executable header; the NSIS launcher is independent of payload architecture.
        coff = struct.pack("<HHIIIHH", 0x14C, 1, 0, 0, 0, 224, 0x102)
        optional = struct.pack("<H", 0x10B) + bytes(222)
        return bytes(dos) + b"PE\0\0" + coff + optional + bytes(40) + b"payload"
    footer = bytearray(512)
    struct.pack_into(">4sII", footer, 0, b"koly", 4, 512)
    return b"payload" + footer


def write_native(path: Path) -> None:
    executable = "bin/axklib.exe" if "-windows-" in path.name else "bin/axklib"
    files = {
        "LICENSE": b"project",
        "licenses/dependency/copyright": b"dependency",
        executable: b"binary",
    }
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path, "w") as archive:
            for name, contents in files.items():
                archive.writestr(name, contents)
    else:
        with tarfile.open(path, "w:gz") as archive:
            for name, contents in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(contents)
                archive.addfile(entry, io.BytesIO(contents))


def write_assets(directory: Path, identity: str = IDENTITY) -> list[Path]:
    directory.mkdir(parents=True, exist_ok=True)
    files: list[Path] = []
    for platform, extension in release_metadata.RELEASE_ASSET_EXTENSIONS.items():
        path = directory / f"axklib-cli-{identity}-{platform}{extension}"
        write_native(path)
        files.append(path)
    for platform, architecture, extension in release_metadata.DESKTOP_RELEASE_TARGETS:
        path = directory / f"axkdeck-{identity}-{platform}-{architecture}{extension}"
        path.write_bytes(installer_bytes(extension))
        files.append(path)
    return sorted(files)


def test_branch_target_retains_exact_name_without_ref_cleanup() -> None:
    target = release_metadata.draft_release_target("branch", "features/audio", branch_version())
    assert target.tag_name == "snapshot/features/audio"
    assert target.prerelease
    assert not hasattr(target, "cleanup_tag")


def test_checksums_cover_only_the_twelve_verified_packages(tmp_path: Path) -> None:
    paths = write_assets(tmp_path)
    assets = release_metadata.prepare_release_assets(
        tmp_path, version=branch_version(), ref_type="branch", ref_name="v0.6.0", commit=COMMIT
    )
    checksum = tmp_path / "SHA256SUMS"
    assert checksum.read_text() == "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in paths
    )
    assert len(assets) == 13
    assert {asset.name for asset in assets} == {path.name for path in paths} | {"SHA256SUMS"}
    assert all(asset.size == asset.path.stat().st_size for asset in assets)


@pytest.mark.parametrize("extension", [".deb", ".rpm", ".exe", ".dmg"])
def test_nonempty_garbage_is_not_an_installer(tmp_path: Path, extension: str) -> None:
    paths = write_assets(tmp_path)
    next(path for path in paths if path.suffix == extension).write_bytes(b"not an installer")
    with pytest.raises(ValueError, match="invalid desktop"):
        release_metadata.verify_release_assets(tmp_path)


@pytest.mark.parametrize("identity", ["0.6.0-pre-deadbee", "main-8c783a7", "0.6.0"])
def test_complete_but_wrong_build_is_rejected(tmp_path: Path, identity: str) -> None:
    write_assets(tmp_path, identity)
    with pytest.raises(ValueError, match="identity"):
        release_metadata.prepare_release_assets(
            tmp_path, version=branch_version(), ref_type="branch", ref_name="v0.6.0", commit=COMMIT
        )
    assert not (tmp_path / "SHA256SUMS").exists()


def test_unexpected_directory_is_rejected(tmp_path: Path) -> None:
    write_assets(tmp_path)
    (tmp_path / "stale").mkdir()
    with pytest.raises(ValueError, match="regular file"):
        release_metadata.verify_release_assets(tmp_path)


def test_zip_rejects_duplicate_normalized_entries(tmp_path: Path) -> None:
    paths = write_assets(tmp_path)
    archive = next(path for path in paths if "windows-x64.zip" in path.name)
    with zipfile.ZipFile(archive, "a") as package:
        package.writestr("./bin/axklib.exe", b"duplicate")
    with pytest.raises(ValueError, match="duplicate"):
        release_metadata.verify_release_assets(tmp_path)


def test_zip_checks_actual_payload_crc(tmp_path: Path) -> None:
    paths = write_assets(tmp_path)
    archive = next(path for path in paths if "windows-x64.zip" in path.name)
    contents = bytearray(archive.read_bytes())
    contents[contents.index(b"binary")] ^= 1
    archive.write_bytes(contents)
    with pytest.raises(ValueError, match="invalid native"):
        release_metadata.verify_release_assets(tmp_path)


@pytest.mark.parametrize("problem", ["empty", "wrong-platform"])
def test_native_archive_rejects_invalid_required_executable(tmp_path: Path, problem: str) -> None:
    paths = write_assets(tmp_path)
    path = next(path for path in paths if "windows-x64.zip" in path.name)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("LICENSE", b"project")
        archive.writestr("licenses/dependency/copyright", b"dependency")
        archive.writestr(
            "bin/axklib.exe" if problem == "empty" else "bin/axklib",
            b"" if problem == "empty" else b"binary",
        )
    with pytest.raises(ValueError, match="empty|wrong platform"):
        release_metadata.verify_release_assets(tmp_path)


def test_artifact_bundles_cannot_hide_duplicate_filenames(tmp_path: Path) -> None:
    bundles = tmp_path / "downloads"
    first = bundles / "release-first"
    write_assets(first)
    second = bundles / "release-second"
    second.mkdir()
    (second / next(first.iterdir()).name).write_bytes(b"duplicate")
    with pytest.raises(ValueError, match="duplicate"):
        release_metadata.collect_release_artifacts(bundles, tmp_path / "flat")


def test_archive_rejects_link_escaping_the_package(tmp_path: Path) -> None:
    paths = write_assets(tmp_path)
    archive = next(path for path in paths if path.name.endswith("linux-x64.tar.gz"))
    with tarfile.open(archive, "w:gz") as package:
        entry = tarfile.TarInfo("licenses/escape")
        entry.type = tarfile.SYMTYPE
        entry.linkname = "../../outside"
        package.addfile(entry)
    with pytest.raises(ValueError, match="unsafe"):
        release_metadata.verify_release_assets(tmp_path)
