"""Structural checks shared by native packaging and draft preparation."""

from __future__ import annotations

import gzip
import posixpath
import stat
import struct
import tarfile
import zipfile
from pathlib import Path, PurePosixPath


def _archive_name(value: str) -> str:
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or ".." in path.parts
        or "\\" in value
        or ":" in value
        or any(ord(character) < 32 for character in value)
    ):
        raise ValueError(f"unsafe archive path: {value!r}")
    return str(path)


def native_archive_entries(path: Path) -> set[str]:
    names: set[str] = set()
    links: dict[str, str] = {}

    def add(value: str, *, directory: bool, link: str | None = None) -> None:
        name = _archive_name(value)
        if directory:
            return
        if name in names:
            raise ValueError(f"duplicate archive entry: {name}")
        names.add(name)
        if link is not None:
            links[name] = _archive_name(link)

    try:
        if path.name.endswith(".zip"):
            with zipfile.ZipFile(path) as archive:
                for entry in archive.infolist():
                    mode = entry.external_attr >> 16
                    if stat.S_ISLNK(mode):
                        if entry.file_size > 4096:
                            raise ValueError(f"unsafe archive link: {entry.filename}")
                        target = archive.read(entry).decode("utf-8")
                        target = posixpath.normpath(
                            posixpath.join(posixpath.dirname(entry.filename), target)
                        )
                        add(entry.filename, directory=False, link=target)
                    elif stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                        raise ValueError(f"unsafe archive entry: {entry.filename}")
                    else:
                        if (
                            _archive_name(entry.filename)
                            in {"LICENSE", "bin/axklib", "bin/axklib.exe"}
                            and entry.file_size == 0
                        ):
                            raise ValueError(
                                f"empty required native archive entry: {entry.filename}"
                            )
                        add(entry.filename, directory=entry.is_dir())
                if archive.testzip() is not None:
                    raise ValueError(f"invalid native archive CRC: {path.name}")
        elif path.name.endswith(".tar.gz"):
            # Reading tar members alone need not consume the gzip trailer/CRC.
            with gzip.open(path, "rb") as stream:
                while stream.read(1024 * 1024):
                    pass
            with tarfile.open(path, "r:gz") as tar_archive:
                for member in tar_archive.getmembers():
                    if member.issym() or member.islnk():
                        target = member.linkname
                        if member.issym():
                            target = posixpath.join(posixpath.dirname(member.name), target)
                        add(member.name, directory=False, link=posixpath.normpath(target))
                    elif member.isfile() or member.isdir():
                        if (
                            _archive_name(member.name)
                            in {"LICENSE", "bin/axklib", "bin/axklib.exe"}
                            and member.size == 0
                        ):
                            raise ValueError(f"empty required native archive entry: {member.name}")
                        add(member.name, directory=member.isdir())
                    else:
                        raise ValueError(f"unsafe archive entry: {member.name}")
        else:
            raise ValueError(f"unsupported native release archive: {path.name}")
    except (tarfile.TarError, zipfile.BadZipFile, OSError, EOFError, UnicodeError) as error:
        raise ValueError(f"invalid native release archive: {path.name}: {error}") from error
    for name, target in links.items():
        visited = {name}
        while target in links and target not in visited:
            visited.add(target)
            target = links[target]
        if target not in names or target in visited:
            raise ValueError(f"unsafe or dangling archive link: {name}")
    return names


def verify_desktop_package(path: Path) -> None:
    """Container checks complement, rather than replace, native install/inspection CI."""
    size = path.stat().st_size
    valid = False
    with path.open("rb") as stream:
        if path.suffix == ".deb" and stream.read(8) == b"!<arch>\n":
            members: set[str] = set()
            valid = True
            while stream.tell() < size:
                header = stream.read(60)
                if len(header) != 60 or header[58:] != b"`\n":
                    valid = False
                    break
                try:
                    name = header[:16].decode("ascii").strip().removesuffix("/")
                    length = int(header[48:58])
                except (UnicodeError, ValueError):
                    valid = False
                    break
                if length <= 0 or name in members or stream.tell() + length > size:
                    valid = False
                    break
                members.add(name)
                if name == "debian-binary":
                    valid = valid and stream.read(length) == b"2.0\n"
                else:
                    stream.seek(length, 1)
                if length % 2 and stream.read(1) != b"\n":
                    valid = False
                    break
            valid = (
                valid
                and "debian-binary" in members
                and all(
                    any(name == prefix or name.startswith(prefix + ".") for name in members)
                    for prefix in ("control.tar", "data.tar")
                )
            )
        elif path.suffix == ".rpm":
            stream.seek(0)
            lead = stream.read(96)
            valid = len(lead) == 96 and lead[:6] == b"\xed\xab\xee\xdb\x03\x00"
            for signature in (True, False):
                header = stream.read(16)
                if len(header) != 16 or header[:8] != b"\x8e\xad\xe8\x01\0\0\0\0":
                    valid = False
                    break
                entries, data_size = struct.unpack(">II", header[8:])
                end = stream.tell() + entries * 16 + data_size
                if end > size:
                    valid = False
                    break
                stream.seek((end + 7) // 8 * 8 if signature else end)
            valid = valid and stream.tell() < size
        elif path.suffix == ".exe":
            stream.seek(0)
            dos = stream.read(64)
            if len(dos) == 64 and dos[:2] == b"MZ":
                offset = struct.unpack_from("<I", dos, 60)[0]
                if 64 <= offset <= size - 26:
                    stream.seek(offset)
                    pe = stream.read(26)
                    sections = struct.unpack_from("<H", pe, 6)[0]
                    optional_size = struct.unpack_from("<H", pe, 20)[0]
                    valid = (
                        pe[:4] == b"PE\0\0"
                        and pe[24:26] in (b"\x0b\x01", b"\x0b\x02")
                        and 1 <= sections <= 96
                        and optional_size >= 64
                        and offset + 24 + optional_size + sections * 40 <= size
                    )
        elif path.suffix == ".dmg" and size > 512:
            stream.seek(-512, 2)
            valid = stream.read(12) == struct.pack(">4sII", b"koly", 4, 512)
    if not valid:
        raise ValueError(f"invalid desktop release package container: {path.name}")
