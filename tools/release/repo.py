#!/usr/bin/env python3
"""Makes FutureBlitz's apt and dnf repositories from its .debs and .rpms, signed with gpg, for
dl.futureblitz.dev/apt and /rpm. publish.yml runs it over every release's packages, those already
in R2 and the new release's, then uploads what it made; docs/release-setup.md makes the key.

    python tools/release/repo.py <out> <package or folder>... --key <fingerprint>
                                 [--base-url URL] [--gnupg-home DIR] [--no-rpm]

<out>/apt is a flat-pool repository, `deb [signed-by=...] <base>/apt stable main`: its packages in
pool/main/f/futureblitz/, dists/stable/main/binary-<arch>/Packages and Packages.gz, each in that
folder's by-hash/SHA256/<its hash> too, for Release's Acquire-By-Hash, and dists/stable/Release,
its detached signature Release.gpg, and InRelease, the two in one. <out>/rpm
is a dnf repository: its packages in packages/, createrepo_c's repodata/, and repomd.xml.asc, its
signature, which a .repo's repo_gpgcheck checks; each .rpm has to be signed with the key already
(nfpm signs them in release.yml), which its gpgcheck checks. Each has the public key, as
futureblitz.gpg (binary, for apt's signed-by) and futureblitz.asc, and rpm/ a futureblitz.repo.

The .debs are read here, so the apt side needs only gpg, and its test runs anywhere; the rpm side
needs createrepo_c, and rpmkeys to check the packages' signatures.
"""

import argparse
import email.utils
import gzip
import hashlib
import io
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib
from pathlib import Path

SUITE = "stable"
COMPONENT = "main"
# Debian's names for the targets' architectures, and rpm's.
DEB_ARCHITECTURES = ("amd64", "arm64")
RPM_ARCHITECTURES = ("x86_64", "aarch64")
DEFAULT_BASE_URL = "https://dl.futureblitz.dev"
# Fields dpkg-scanpackages adds to a package's stanza, before its Description.
HASHES = (("MD5sum", "md5"), ("SHA1", "sha1"), ("SHA256", "sha256"))


class RepoError(Exception):
    pass


def ar_members(data: bytes) -> dict[str, bytes]:
    """The files in an ar archive, which a .deb is: debian-binary, control.tar.*, and data.tar.*."""
    if not data.startswith(b"!<arch>\n"):
        raise RepoError("it isn't an ar archive")
    members = {}
    at = 8
    # A member's data is padded to an even length, which the last one's may not be.
    while at < len(data) and data[at:] != b"\n":
        header = data[at:at + 60]
        if len(header) < 60 or header[58:60] != b"`\n":
            raise RepoError("an ar member's header is broken or cut off")
        name = header[0:16].decode("ascii", "replace").strip().rstrip("/")
        size_field = header[48:58].decode("ascii", "replace").strip()
        if not size_field.isdigit():
            raise RepoError(f"{name}'s size isn't a number")
        size = int(size_field)
        at += 60
        if at + size > len(data):
            raise RepoError(f"{name} runs past the file's end")
        members[name] = data[at:at + size]
        at += size + (size % 2)
    return members


def deb_control(path: Path) -> str:
    """A .deb's control file, from its control.tar.gz, .xz, or plain .tar. The .deb has to have
    all of its members whole, its data.tar too, which apt would download and dpkg refuse."""
    try:
        members = ar_members(path.read_bytes())
    except RepoError as error:
        raise RepoError(f"{path.name} isn't a .deb: {error}") from None
    if not any(name.startswith("data.tar") for name in members):
        raise RepoError(f"{path.name} isn't a .deb: it has no data.tar")
    for name, data in members.items():
        if not name.startswith("control.tar"):
            continue
        try:
            if name == "control.tar.gz":
                data = gzip.decompress(data)
            elif name == "control.tar.xz":
                data = lzma.decompress(data)
            elif name != "control.tar":
                raise RepoError(f"{path.name}'s {name} is compressed in a way this can't read")
            with tarfile.open(fileobj=io.BytesIO(data)) as tar:
                for entry in tar.getmembers():
                    if entry.name in ("control", "./control"):
                        control = tar.extractfile(entry)
                        if control:
                            return control.read().decode("utf-8")
        except (EOFError, OSError, zlib.error, lzma.LZMAError, tarfile.TarError,
                UnicodeDecodeError) as error:
            raise RepoError(f"{path.name}'s {name} is broken: {error}") from None
        raise RepoError(f"{path.name}'s {name} has no control file")
    raise RepoError(f"{path.name} has no control.tar")


def control_fields(text: str) -> list[tuple[str, str]]:
    """A control file's fields, in order, each value with its continuation lines."""
    fields: list[tuple[str, str]] = []
    for line in text.rstrip("\n").split("\n"):
        if line[:1] in (" ", "\t") and fields:
            name, value = fields[-1]
            fields[-1] = (name, value + "\n" + line)
        elif ":" in line:
            name, value = line.split(":", 1)
            fields.append((name.strip(), value.strip()))
        elif line.strip():
            raise RepoError(f"a control file's line isn't a field: {line!r}")
    return fields


def digests(data: bytes) -> dict[str, str]:
    return {name: hashlib.new(name, data).hexdigest() for name in ("md5", "sha1", "sha256")}


def gpg_path(gpg: str, path: Path) -> str:
    """A path as gpg takes it. Git for Windows' gpg is an MSYS program, with
    msys-2.0.dll beside it, whose agent and keyrings take only a POSIX path, /c/Users/...; any
    other gpg takes the path as it is."""
    if os.name != "nt" or not (Path(gpg).parent / "msys-2.0.dll").exists():
        return str(path)
    full = path.resolve()
    return "/" + full.drive.rstrip(":").lower() + "/" + "/".join(full.parts[1:])


def gpg_command(home: Path | None) -> list[str]:
    gpg = shutil.which("gpg")
    if not gpg:
        raise RepoError("signing needs gpg, which isn't on PATH")
    command = [gpg, "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase", ""]
    if home:
        command += ["--homedir", gpg_path(gpg, home)]
    return command


def gpg(home: Path | None, *arguments: str) -> None:
    result = subprocess.run(gpg_command(home) + list(arguments), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise RepoError(f"gpg {' '.join(arguments)} failed:\n"
                        + result.stdout.decode("utf-8", "replace"))


def export_key(home: Path | None, key: str, folder: Path) -> None:
    gpg(home, "--output", str(folder / "futureblitz.gpg"), "--export", key)
    gpg(home, "--armor", "--output", str(folder / "futureblitz.asc"), "--export", key)
    if (folder / "futureblitz.gpg").stat().st_size == 0:
        raise RepoError(f"gpg has no public key {key}")


def build_apt(out: Path, debs: list[Path], key: str, home: Path | None) -> None:
    pool = out / "pool" / COMPONENT / "f" / "futureblitz"
    pool.mkdir(parents=True)
    stanzas: dict[str, list[str]] = {architecture: [] for architecture in DEB_ARCHITECTURES}
    seen: dict[tuple[str, str, str], str] = {}
    for deb in sorted(debs, key=lambda path: path.name):
        fields = control_fields(deb_control(deb))
        values = dict(fields)
        for field in ("Package", "Version", "Architecture"):
            if field not in values:
                raise RepoError(f"{deb.name}'s control file has no {field}")
        architecture = values["Architecture"]
        if architecture not in stanzas:
            raise RepoError(f"{deb.name} is for {architecture}, not one of "
                            + ", ".join(DEB_ARCHITECTURES))
        identity = (values["Package"], values["Version"], architecture)
        if identity in seen:
            raise RepoError(f"{deb.name} and {seen[identity]} are both {' '.join(identity)}")
        seen[identity] = deb.name

        data = deb.read_bytes()
        shutil.copyfile(deb, pool / deb.name)
        sums = digests(data)
        added = [("Filename", (pool / deb.name).relative_to(out).as_posix()),
                 ("Size", str(len(data)))] + [(field, sums[name]) for field, name in HASHES]
        kept = [field for field in fields if field[0] not in dict(added)]
        before = [field for field in kept if field[0] != "Description"]
        after = [field for field in kept if field[0] == "Description"]
        stanzas[architecture].append(
            "".join(f"{name}: {value}\n" for name, value in before + added + after))

    dist = out / "dists" / SUITE
    indexes: list[tuple[str, bytes]] = []
    for architecture, entries in stanzas.items():
        folder = dist / COMPONENT / f"binary-{architecture}"
        folder.mkdir(parents=True)
        packages = "\n".join(entries).encode("utf-8")
        # Packages.gz has no name or time in it, so the same packages make the same file.
        packed = gzip.compress(packages, mtime=0)
        # Each index under by-hash/ too, by its SHA256, which apt fetches with Acquire-By-Hash,
        # so the InRelease a client has, old or new, names files that are there and never change.
        by_hash = folder / "by-hash" / "SHA256"
        by_hash.mkdir(parents=True)
        for name, data in (("Packages", packages), ("Packages.gz", packed)):
            (folder / name).write_bytes(data)
            (by_hash / hashlib.sha256(data).hexdigest()).write_bytes(data)
            indexes.append((f"{COMPONENT}/binary-{architecture}/{name}", data))

    date = email.utils.formatdate(usegmt=True).replace("GMT", "UTC")
    release = [
        "Origin: FutureBlitz",
        "Label: FutureBlitz",
        f"Suite: {SUITE}",
        f"Codename: {SUITE}",
        f"Date: {date}",
        f"Architectures: {' '.join(DEB_ARCHITECTURES)}",
        f"Components: {COMPONENT}",
        "Description: FutureBlitz's packages",
        "Acquire-By-Hash: yes",
    ]
    for field, name in (("MD5Sum", "md5"), ("SHA1", "sha1"), ("SHA256", "sha256")):
        release.append(f"{field}:")
        for path, data in indexes:
            release.append(f" {digests(data)[name]} {len(data)} {path}")
    (dist / "Release").write_text("\n".join(release) + "\n", encoding="utf-8", newline="\n")

    gpg(home, "--local-user", key, "--digest-algo", "SHA512", "--armor", "--detach-sign",
        "--output", str(dist / "Release.gpg"), str(dist / "Release"))
    gpg(home, "--local-user", key, "--digest-algo", "SHA512", "--clearsign",
        "--output", str(dist / "InRelease"), str(dist / "Release"))
    export_key(home, key, out)


def check_rpm_signatures(rpms: list[Path], public_key: Path) -> None:
    """Each .rpm is signed with the repository's key, as dnf's gpgcheck asks."""
    rpmkeys = shutil.which("rpmkeys")
    if not rpmkeys:
        raise RepoError("checking the .rpms' signatures needs rpmkeys (the rpm package)")
    with tempfile.TemporaryDirectory() as database:
        imported = subprocess.run([rpmkeys, "--dbpath", database, "--import", str(public_key)],
                                  capture_output=True, text=True)
        if imported.returncode != 0:
            raise RepoError(f"rpmkeys couldn't import the key: {imported.stderr}")
        for rpm in rpms:
            checked = subprocess.run([rpmkeys, "--dbpath", database, "--checksig", str(rpm)],
                                     capture_output=True, text=True)
            if checked.returncode != 0 or "signatures OK" not in checked.stdout:
                raise RepoError(f"{rpm.name} isn't signed with the repository's key: "
                                + (checked.stdout + checked.stderr).strip())


def repo_file(base_url: str) -> str:
    return (
        "[futureblitz]\n"
        "name=FutureBlitz\n"
        f"baseurl={base_url}/rpm\n"
        "enabled=1\n"
        "gpgcheck=1\n"
        "repo_gpgcheck=1\n"
        f"gpgkey={base_url}/rpm/futureblitz.asc\n"
    )


def build_rpm(out: Path, rpms: list[Path], key: str, home: Path | None, base_url: str) -> None:
    for rpm in rpms:
        if not any(rpm.name.endswith(f".{architecture}.rpm")
                   for architecture in RPM_ARCHITECTURES):
            raise RepoError(f"{rpm.name} is for none of " + ", ".join(RPM_ARCHITECTURES))
    createrepo = shutil.which("createrepo_c")
    if not createrepo:
        raise RepoError("the rpm repository needs createrepo_c")

    packages = out / "packages"
    packages.mkdir(parents=True)
    export_key(home, key, out)
    check_rpm_signatures(rpms, out / "futureblitz.asc")
    for rpm in rpms:
        shutil.copyfile(rpm, packages / rpm.name)
    made = subprocess.run([createrepo, "--quiet", "--no-database",
                           str(out)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if made.returncode != 0:
        raise RepoError("createrepo_c failed:\n" + made.stdout.decode("utf-8", "replace"))
    sign_repomd(out, key, home)
    (out / "futureblitz.repo").write_text(repo_file(base_url), encoding="utf-8", newline="\n")


def sign_repomd(out: Path, key: str, home: Path | None) -> None:
    repomd = out / "repodata" / "repomd.xml"
    gpg(home, "--local-user", key, "--digest-algo", "SHA512", "--armor", "--detach-sign",
        "--output", str(repomd) + ".asc", str(repomd))


def gather(paths: list[Path]) -> tuple[list[Path], list[Path]]:
    """The .debs and .rpms among the paths, and in the folders among them; one name once."""
    files: dict[str, Path] = {}
    for path in paths:
        found = sorted(path.rglob("*")) if path.is_dir() else [path]
        for file in found:
            if file.suffix not in (".deb", ".rpm") or not file.is_file():
                continue
            if file.name in files and files[file.name].read_bytes() != file.read_bytes():
                raise RepoError(f"{file} and {files[file.name]} differ, but have one name")
            files.setdefault(file.name, file)
    debs = [file for file in files.values() if file.suffix == ".deb"]
    rpms = [file for file in files.values() if file.suffix == ".rpm"]
    return debs, rpms


def build(out: Path, paths: list[Path], key: str, home: Path | None, base_url: str,
          rpm: bool = True) -> None:
    debs, rpms = gather(paths)
    if not debs:
        raise RepoError("there are no .debs")
    if rpm and not rpms:
        raise RepoError("there are no .rpms")
    if out.exists():
        shutil.rmtree(out)
    build_apt(out / "apt", debs, key, home)
    if rpm:
        build_rpm(out / "rpm", rpms, key, home, base_url.rstrip("/"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", type=Path, help="the folder apt/ and rpm/ go in, made anew")
    parser.add_argument("packages", type=Path, nargs="+", help=".debs, .rpms, and folders of them")
    parser.add_argument("--key", required=True, help="the signing key's fingerprint")
    parser.add_argument("--gnupg-home", type=Path, help="gpg's home folder, if not its own")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL,
                        help=f"where the repositories are served, {DEFAULT_BASE_URL} by default")
    parser.add_argument("--no-rpm", action="store_true", help="make only the apt repository")
    arguments = parser.parse_args()
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", arguments.key):
        print("error: --key is the key's fingerprint, 40 hex digits", file=sys.stderr)
        return 2
    try:
        build(arguments.out, arguments.packages, arguments.key, arguments.gnupg_home,
              arguments.base_url, rpm=not arguments.no_rpm)
    except (RepoError, OSError, tarfile.TarError, lzma.LZMAError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Made the repositories in {arguments.out}")
    return 0


if __name__ == "__main__":
    os.umask(0o022)
    sys.exit(main())
