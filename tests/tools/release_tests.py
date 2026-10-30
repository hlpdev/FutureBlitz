"""tools/release's tests: winget's manifests and Homebrew's formula from a release's SHA256SUMS,
the MSI's ProductCode, and the apt repository repo.py makes from fake .debs, signed with a key
made for the test, which gpgv checks as apt does; with nfpm, the .deb nfpm.yaml makes. ctest runs
them; gpg comes from PATH, or Git's on Windows, and the tests that sign skip without it.

    python -m unittest release_tests
"""

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
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
RELEASE = ROOT / "tools" / "release"
sys.path.insert(0, str(RELEASE))
import packages  # noqa: E402
import repo  # noqa: E402

TARGETS = ("windows-x64", "linux-x64", "linux-arm64", "macos-arm64", "macos-x64")


def find_gpg() -> str | None:
    """gpg, from PATH, or else Git for Windows', whose folder goes on PATH for repo.py."""
    found = shutil.which("gpg")
    if found:
        return found
    git = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git" / "usr" / "bin"
    if (git / "gpg.exe").exists():
        os.environ["PATH"] = str(git) + os.pathsep + os.environ.get("PATH", "")
        return shutil.which("gpg")
    return None


def fake_sums(version: str, folder: Path) -> Path:
    """A SHA256SUMS for a release's archives and MSI, each name's hash made from the name."""
    names = [f"futureblitz-{version}-{target}.{'zip' if 'windows' in target else 'tar.gz'}"
             for target in TARGETS] + [f"futureblitz-{version}-windows-x64.msi"]
    lines = [f"{hashlib.sha256(name.encode()).hexdigest()}  {name}" for name in names]
    path = folder / "SHA256SUMS"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def ar(members: list[tuple[str, bytes]]) -> bytes:
    data = b"!<arch>\n"
    for name, content in members:
        data += f"{name:<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(content):<10}`\n".encode("ascii")
        data += content + (b"\n" if len(content) % 2 else b"")
    return data


def tar(files: dict[str, bytes], compress: str) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for name, content in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            archive.addfile(info, io.BytesIO(content))
    data = buffer.getvalue()
    return gzip.compress(data) if compress == "gz" else lzma.compress(data)


def fake_deb(folder: Path, architecture: str, version: str = "1.0.0-1", compress: str = "gz",
             package: str = "futureblitz") -> Path:
    """A .deb as dpkg-deb lays one out, whose control file has a Description of two lines."""
    control = (f"Package: {package}\nVersion: {version}\nArchitecture: {architecture}\n"
               "Maintainer: FutureBlitz <https://github.com/hlpdev/FutureBlitz/issues>\n"
               "Depends: libc6 (>= 2.35)\n"
               "Description: Compiler, language server, and tools\n"
               " FutureBlitz is a successor to Blitz3D.\n")
    data = ar([
        ("debian-binary", b"2.0\n"),
        (f"control.tar.{compress}", tar({"./control": control.encode()}, compress)),
        ("data.tar.gz", tar({"./usr/bin/fb": b"#!/bin/sh\n"}, "gz")),
    ])
    path = folder / f"{package}_{version}_{architecture}.deb"
    path.write_bytes(data)
    return path


def stanzas(text: str) -> list[dict[str, str]]:
    return [dict(repo.control_fields(block)) for block in text.split("\n\n") if block.strip()]


class PackagesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.folder, ignore_errors=True)
        self.sums = packages.read_sums(fake_sums("1.0.0", self.folder))

    def test_a_version_has_a_product_code_of_its_own(self) -> None:
        code = packages.product_code("1.0.0")
        self.assertEqual(code, packages.product_code("1.0.0"))
        self.assertNotEqual(code, packages.product_code("1.0.1"))
        self.assertEqual(str(uuid.UUID(code)).upper(), code)
        for bad in ("v1.0.0", "1.0", "1.0.0-rc.1", "01.0.0"):
            with self.assertRaises(packages.ReleaseError):
                packages.product_code(bad)

    def test_the_upgrade_code_is_the_msis(self) -> None:
        wxs = (ROOT / "tools" / "install" / "futureblitz.wxs").read_text(encoding="utf-8")
        self.assertIn(f'UpgradeCode="{packages.upgrade_code()}"', wxs)

    def test_sha256sum_marks_binary_files_with_a_star(self) -> None:
        sums = self.folder / "starred"
        sums.write_text(f"{'AB' * 32} *one.zip\n\n{'cd' * 32}  two words.tar.gz\n")
        self.assertEqual(packages.read_sums(sums),
                         {"one.zip": "ab" * 32, "two words.tar.gz": "cd" * 32})
        sums.write_text("not a hash  one.zip\n")
        with self.assertRaisesRegex(packages.ReleaseError, "starred:1"):
            packages.read_sums(sums)

    def test_winget_installs_the_msi_for_the_user(self) -> None:
        base = packages.url_base("1.0.0", None)
        manifests = packages.winget("1.0.0", self.sums, base)
        self.assertEqual(sorted(manifests), [
            "FutureBlitz.FutureBlitz.installer.yaml",
            "FutureBlitz.FutureBlitz.locale.en-US.yaml",
            "FutureBlitz.FutureBlitz.yaml",
        ])
        for text in manifests.values():
            self.assertIn("PackageIdentifier: FutureBlitz.FutureBlitz\nPackageVersion: 1.0.0\n", text)
            self.assertIn("ManifestVersion: 1.10.0\n", text)
            self.assertNotIn("\t", text)
        installer = manifests["FutureBlitz.FutureBlitz.installer.yaml"]
        msi = "futureblitz-1.0.0-windows-x64.msi"
        self.assertIn(f"  InstallerUrl: https://github.com/hlpdev/FutureBlitz/releases/download/"
                      f"v1.0.0/{msi}\n", installer)
        self.assertIn(f"  InstallerSha256: {self.sums[msi].upper()}\n", installer)
        self.assertIn(f"ProductCode: '{{{packages.product_code('1.0.0')}}}'\n", installer)
        self.assertIn("InstallerType: wix\nScope: user\n", installer)
        locale = manifests["FutureBlitz.FutureBlitz.locale.en-US.yaml"]
        self.assertIn("License: Apache-2.0\n", locale)
        self.assertIn("ManifestType: defaultLocale\n", locale)

    def test_the_formula_takes_each_targets_archive(self) -> None:
        formula = packages.homebrew("1.0.0", self.sums,
                                    packages.url_base("1.0.0", "file:///releases/"))
        self.assertTrue(formula.startswith("# Generated"))
        self.assertIn("class Futureblitz < Formula\n", formula)
        self.assertIn('  version "1.0.0"\n', formula)
        self.assertIn('  license "Apache-2.0"\n', formula)
        blocks = re.findall(r'url "file:///releases/futureblitz-1\.0\.0-([a-z0-9-]+)\.tar\.gz"\n'
                            r'\s+sha256 "([0-9a-f]{64})"', formula)
        self.assertEqual([target for target, _ in blocks],
                         ["macos-arm64", "macos-x64", "linux-arm64", "linux-x64"])
        for target, digest in blocks:
            self.assertEqual(digest, self.sums[f"futureblitz-1.0.0-{target}.tar.gz"])
        # on_macos holds macOS' archives and the macOS it needs; on_linux, Linux's.
        macos = formula[formula.index("on_macos do"):formula.index("on_linux do")]
        self.assertIn("depends_on macos: :sequoia", macos)
        self.assertIn("macos-x64", macos)
        self.assertNotIn("linux", macos)
        self.assertIn('assert_match "fb #{version} ", shell_output("#{bin}/fb --version")', formula)
        self.assertIn('system bin/"fb", "new", "hello"', formula)
        self.assertIn('shell_output("#{bin}/fb run").strip', formula)
        # Each block that opens closes, the class's too.
        lines = [line.strip() for line in formula.splitlines()]
        opened = sum(line.endswith(" do") or line.startswith(("class ", "def ")) for line in lines)
        self.assertEqual(opened, lines.count("end"))

    def run_script(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(RELEASE / "packages.py"), *arguments],
                              capture_output=True, text=True)

    def test_the_script_writes_them(self) -> None:
        sums = str(self.folder / "SHA256SUMS")
        winget = self.folder / "winget"
        self.assertEqual(self.run_script("winget", "v1.0.0", sums, str(winget)).returncode, 0)
        self.assertEqual(len(list(winget.glob("*.yaml"))), 3)
        formula = self.folder / "tap" / "futureblitz.rb"
        self.assertEqual(self.run_script("homebrew", "1.0.0", sums, str(formula)).returncode, 0)
        self.assertIn('sha256 "', formula.read_text())
        printed = self.run_script("product-code", "1.0.0")
        self.assertEqual(printed.stdout.strip(), packages.product_code("1.0.0"))

    def test_a_release_without_a_file_is_refused(self) -> None:
        sums = str(self.folder / "SHA256SUMS")
        refused = self.run_script("winget", "1.0.1", sums, str(self.folder / "out"))
        self.assertEqual(refused.returncode, 1)
        self.assertIn("SHA256SUMS has no futureblitz-1.0.1-windows-x64.msi", refused.stderr)
        refused = self.run_script("homebrew", "1.0", sums, str(self.folder / "f.rb"))
        self.assertEqual(refused.returncode, 1)
        self.assertIn("three numbers", refused.stderr)


@unittest.skipUnless(find_gpg(), "gpg isn't here")
class RepoTests(unittest.TestCase):
    home: Path
    key: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.home = Path(tempfile.mkdtemp())
        made = subprocess.run(repo.gpg_command(cls.home) + [
            "--quick-gen-key", "FutureBlitz test <test@example.invalid>", "rsa2048", "sign",
            "never"], capture_output=True, text=True)
        if made.returncode != 0:
            raise RuntimeError(f"gpg couldn't make a key: {made.stderr}")
        listed = subprocess.run(repo.gpg_command(cls.home) + ["--list-keys", "--with-colons"],
                                capture_output=True, text=True).stdout
        cls.key = re.search(r"^fpr:+([0-9A-F]{40}):", listed, re.MULTILINE).group(1)

    @classmethod
    def tearDownClass(cls) -> None:
        gpgconf = shutil.which("gpgconf")
        if gpgconf:
            subprocess.run([gpgconf, "--homedir", str(cls.home), "--kill", "all"],
                           capture_output=True)
        shutil.rmtree(cls.home, ignore_errors=True)

    def setUp(self) -> None:
        self.folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.folder, ignore_errors=True)
        self.packages = self.folder / "packages"
        self.packages.mkdir()
        self.out = self.folder / "site"

    def run_repo(self, *paths: Path, key: str | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(RELEASE / "repo.py"), str(self.out),
                               *map(str, paths), "--key", key or self.key, "--gnupg-home",
                               str(self.home), "--no-rpm"], capture_output=True, text=True)

    @staticmethod
    def gpgv(keyring: Path, *files: Path) -> subprocess.CompletedProcess[str]:
        """gpgv checks a signature with the repository's own keyring, as apt does."""
        gpgv = shutil.which("gpgv") or "gpgv"
        return subprocess.run([gpgv, "--keyring", repo.gpg_path(gpgv, keyring),
                               *map(str, files)], capture_output=True, text=True)

    def verify(self, *files: Path) -> None:
        checked = self.gpgv(self.out / "apt" / "futureblitz.gpg", *files)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_the_apt_repository_indexes_and_signs_each_architecture(self) -> None:
        amd64 = fake_deb(self.packages, "amd64")
        arm64 = fake_deb(self.packages, "arm64", compress="xz")
        older = fake_deb(self.packages, "amd64", version="0.9.0-1")
        made = self.run_repo(self.packages)
        self.assertEqual(made.returncode, 0, made.stderr)

        apt = self.out / "apt"
        pool = apt / "pool" / "main" / "f" / "futureblitz"
        self.assertEqual(sorted(path.name for path in pool.iterdir()),
                         sorted(path.name for path in (amd64, arm64, older)))
        dist = apt / "dists" / "stable"
        for architecture, debs in (("amd64", [older, amd64]), ("arm64", [arm64])):
            folder = dist / "main" / f"binary-{architecture}"
            packages_text = (folder / "Packages").read_text(encoding="utf-8")
            self.assertEqual(gzip.decompress((folder / "Packages.gz").read_bytes()).decode(),
                             packages_text)
            entries = stanzas(packages_text)
            self.assertEqual([entry["Filename"] for entry in entries],
                             [f"pool/main/f/futureblitz/{deb.name}" for deb in debs])
            for entry, deb in zip(entries, debs):
                data = deb.read_bytes()
                self.assertEqual(entry["Package"], "futureblitz")
                self.assertEqual(entry["Architecture"], architecture)
                self.assertEqual(entry["Depends"], "libc6 (>= 2.35)")
                self.assertEqual(entry["Size"], str(len(data)))
                self.assertEqual(entry["SHA256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(entry["MD5sum"], hashlib.md5(data).hexdigest())
                self.assertEqual(entry["Description"], "Compiler, language server, and tools\n"
                                 " FutureBlitz is a successor to Blitz3D.")
                self.assertEqual(list(entry)[-1], "Description")

        release = dict(repo.control_fields((dist / "Release").read_text(encoding="utf-8")))
        self.assertEqual(release["Suite"], "stable")
        self.assertEqual(release["Architectures"], "amd64 arm64")
        self.assertEqual(release["Components"], "main")
        self.assertRegex(release["Date"], r"^[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} [\d:]{8} UTC$")
        listed = {}
        for line in release["SHA256"].split("\n"):
            if line.strip():
                digest, size, path = line.split()
                listed[path] = (digest, int(size))
        self.assertEqual(sorted(listed), sorted(
            f"main/binary-{architecture}/{name}" for architecture in ("amd64", "arm64")
            for name in ("Packages", "Packages.gz")))
        for path, (digest, size) in listed.items():
            data = (dist / path).read_bytes()
            self.assertEqual((hashlib.sha256(data).hexdigest(), len(data)), (digest, size), path)

        self.verify(dist / "InRelease")
        self.verify(dist / "Release.gpg", dist / "Release")
        self.assertIn("Origin: FutureBlitz", (dist / "InRelease").read_text(encoding="utf-8"))
        self.assertTrue((apt / "futureblitz.asc").read_text().startswith(
            "-----BEGIN PGP PUBLIC KEY BLOCK-----"))

    def test_a_changed_release_breaks_its_signature(self) -> None:
        fake_deb(self.packages, "amd64")
        self.assertEqual(self.run_repo(self.packages).returncode, 0)
        dist = self.out / "apt" / "dists" / "stable"
        release = dist / "Release"
        self.verify(dist / "Release.gpg", release)
        release.write_text(release.read_text() + "Extra: field\n")
        checked = self.gpgv(self.out / "apt" / "futureblitz.gpg", dist / "Release.gpg", release)
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("BAD signature", checked.stderr)

    def test_what_isnt_a_release_package_is_refused(self) -> None:
        cases = []
        other = self.folder / "other"
        other.mkdir()
        cases.append(([fake_deb(other, "i386")], "is for i386"))
        twice = self.folder / "twice"
        twice.mkdir()
        fake_deb(twice, "amd64")
        renamed = twice / "renamed.deb"
        shutil.copyfile(twice / "futureblitz_1.0.0-1_amd64.deb", renamed)
        cases.append(([twice], "are both futureblitz 1.0.0-1 amd64"))
        broken = self.folder / "broken.deb"
        broken.write_bytes(b"not a package")
        cases.append(([broken], "broken.deb isn't a .deb"))
        empty = self.folder / "empty"
        empty.mkdir()
        cases.append(([empty], "there are no .debs"))
        for paths, says in cases:
            refused = self.run_repo(*paths)
            self.assertEqual(refused.returncode, 1, says)
            self.assertIn(says, refused.stderr)

    def test_one_name_in_two_places_is_one_package_unless_they_differ(self) -> None:
        first = self.folder / "first"
        second = self.folder / "second"
        first.mkdir()
        second.mkdir()
        fake_deb(first, "amd64")
        fake_deb(second, "amd64")
        made = self.run_repo(first, second)
        self.assertEqual(made.returncode, 0, made.stderr)
        # The same package, but its control.tar compressed otherwise.
        fake_deb(second, "amd64", compress="xz")
        refused = self.run_repo(first, second)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("differ, but have one name", refused.stderr)

    def test_a_key_gpg_doesnt_have_is_refused(self) -> None:
        fake_deb(self.packages, "amd64")
        refused = self.run_repo(self.packages, key="0" * 40)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("gpg", refused.stderr)

    def test_the_rpm_repositorys_metadata_is_signed(self) -> None:
        rpm = self.folder / "rpm"
        (rpm / "repodata").mkdir(parents=True)
        (rpm / "repodata" / "repomd.xml").write_text("<repomd/>\n")
        repo.sign_repomd(rpm, self.key, self.home)
        repo.export_key(self.home, self.key, rpm)
        checked = self.gpgv(rpm / "futureblitz.gpg", rpm / "repodata" / "repomd.xml.asc",
                            rpm / "repodata" / "repomd.xml")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(repo.repo_file("https://dl.example.test"), (
            "[futureblitz]\nname=FutureBlitz\nbaseurl=https://dl.example.test/rpm\nenabled=1\n"
            "gpgcheck=1\nrepo_gpgcheck=1\ngpgkey=https://dl.example.test/rpm/futureblitz.asc\n"))

    @unittest.skipUnless(shutil.which("nfpm"), "nfpm isn't here")
    def test_nfpm_makes_a_deb_the_repository_takes(self) -> None:
        release = self.folder / "work" / "futureblitz"
        for folder in ("bin", "std", "lib/linux-x64"):
            (release / folder).mkdir(parents=True)
        for name in ("fb", "fb-lsp", "fb-bindgen"):
            (release / "bin" / name).write_text("#!/bin/sh\n")
        environment = dict(os.environ, VERSION="1.2.3", ARCH="arm64", RPM_SIGNING_KEY="")
        made = subprocess.run([shutil.which("nfpm"), "package", "-f", str(RELEASE / "nfpm.yaml"),
                               "-p", "deb", "-t", str(self.packages)], cwd=release.parent,
                              env=environment, capture_output=True, text=True)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        deb = self.packages / "futureblitz_1.2.3-1_arm64.deb"
        control = dict(repo.control_fields(repo.deb_control(deb)))
        self.assertEqual(control["Version"], "1.2.3-1")
        self.assertIn("libc6 (>= 2.35)", control["Depends"])
        members = repo.ar_members(deb.read_bytes())
        data = next(content for name, content in members.items() if name.startswith("data.tar"))
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            links = {entry.name: entry.linkname for entry in archive.getmembers() if entry.issym()}
            names = archive.getnames()
        self.assertEqual(links["./usr/bin/fb"], "/usr/lib/futureblitz/bin/fb")
        self.assertIn("./usr/lib/futureblitz/bin/fb-lsp", names)
        self.assertEqual(self.run_repo(self.packages).returncode, 0)


if __name__ == "__main__":
    unittest.main()
