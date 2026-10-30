#!/usr/bin/env python3
"""Checks that a published release has what the download page promises: every file release.yml
makes, each as SHA256SUMS has it; a valid Authenticode signature on setup.exe and the MSI; winget's
manifest of the version in microsoft/winget-pkgs; and the Homebrew tap's formula at the version.
docs/release-setup.md's launch checklist runs it once publish.yml is done.

    python tools/release/launch_check.py <version> [--repository owner/name] [--tap owner/name]

It prints each check, and exits 1 when one fails. GITHUB_TOKEN, when it's set, goes with its
requests to GitHub's API. A signature is checked with PowerShell's Get-AuthenticodeSignature on
Windows, and osslsigncode elsewhere.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packages  # noqa: E402

REPOSITORY = packages.REPOSITORY
TAP = "hlpdev/homebrew-tap"
TARGETS = ("windows-x64", "linux-x64", "linux-arm64", "macos-arm64", "macos-x64")
# The files each release's Windows installers are; their signatures are checked.
SIGNED = ("{base}-windows-x64-setup.exe", "{base}-windows-x64.msi")


def expected_files(version: str) -> list[str]:
    """Every file a release has, as docs/tooling.md's Releases lists them."""
    base = f"futureblitz-{version}"
    files = [f"{base}-{target}.{'zip' if target.startswith('windows') else 'tar.gz'}"
             for target in TARGETS]
    files += [name.format(base=base) for name in SIGNED]
    files += [f"{base}-macos-arm64.pkg", f"{base}-macos-x64.pkg"]
    files += [f"futureblitz_{version}-1_{arch}.deb" for arch in ("amd64", "arm64")]
    files += [f"futureblitz-{version}-1.{arch}.rpm" for arch in ("x86_64", "aarch64")]
    files += [f"{base}.vsix", "install.sh", "install.ps1", "SHA256SUMS"]
    return files


class Source:
    """Where the checks read from: GitHub, through its API and its raw files."""

    def __init__(self, token: str | None = None) -> None:
        self.token = token

    def fetch(self, url: str) -> bytes | None:
        """The body at `url`, or None for a 404."""
        request = urllib.request.Request(url, headers={"User-Agent": "futureblitz-launch-check"})
        if self.token and url.startswith("https://api.github.com/"):
            request.add_header("Authorization", f"Bearer {self.token}")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise

    def release(self, repository: str, version: str) -> dict | None:
        """The release v<version>: whether it's a draft, and its assets' names and URLs. None
        when there's no published one, as GitHub doesn't show a draft by its tag."""
        body = self.fetch(f"https://api.github.com/repos/{repository}/releases/tags/v{version}")
        if body is None:
            return None
        found = json.loads(body)
        return {
            "draft": bool(found.get("draft")),
            "assets": {asset["name"]: asset["browser_download_url"]
                       for asset in found.get("assets", [])},
        }


def authenticode(path: Path) -> tuple[bool, str]:
    """Whether `path` has a valid Authenticode signature, and what the check said."""
    if sys.platform == "win32":
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            return False, "there's no PowerShell to check it with"
        script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:FB_SIGNED; "
                  "\"$($s.Status) $($s.SignerCertificate.Subject)\"")
        result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", script],
                                env={**os.environ, "FB_SIGNED": str(path)}, capture_output=True,
                                text=True)
        said = result.stdout.strip()
        return result.returncode == 0 and said.startswith("Valid "), said or result.stderr.strip()
    checker = shutil.which("osslsigncode")
    if not checker:
        return False, "there's no osslsigncode to check it with"
    result = subprocess.run([checker, "verify", "-in", str(path)], capture_output=True, text=True)
    said = (result.stdout + result.stderr).strip().splitlines()
    return result.returncode == 0, said[-1] if said else ""


def check(version: str, source: Source, signature: Callable[[Path], tuple[bool, str]],
          repository: str = REPOSITORY, tap: str = TAP) -> list[tuple[str, str | None]]:
    """Each check's name, and why it failed, or None when it passed."""
    results: list[tuple[str, str | None]] = []
    release = source.release(repository, version)
    if release is None or release["draft"]:
        return [(f"the release v{version}", f"{repository} has no published release v{version}")]
    results.append((f"the release v{version}", None))

    assets = release["assets"]
    missing = [name for name in expected_files(version) if name not in assets]
    results.append(("every file the download page promises",
                    f"it has no {', '.join(missing)}" if missing else None))
    if "SHA256SUMS" not in assets:
        return results

    with tempfile.TemporaryDirectory() as folder:
        sums_file = Path(folder) / "SHA256SUMS"
        sums_file.write_bytes(source.fetch(assets["SHA256SUMS"]) or b"")
        try:
            sums = packages.read_sums(sums_file)
        except packages.ReleaseError as error:
            results.append(("SHA256SUMS", str(error)))
            return results
        unlisted = [name for name in assets if name != "SHA256SUMS" and name not in sums]
        results.append(("SHA256SUMS has every file",
                        f"it has no {', '.join(unlisted)}" if unlisted else None))

        for pattern in SIGNED:
            name = pattern.format(base=f"futureblitz-{version}")
            if name not in assets:
                continue
            path = Path(folder) / name
            path.write_bytes(source.fetch(assets[name]) or b"")
            if hashlib.sha256(path.read_bytes()).hexdigest() != sums.get(name):
                results.append((f"{name}'s signature", "it isn't the file SHA256SUMS names"))
                continue
            valid, said = signature(path)
            results.append((f"{name}'s signature",
                            None if valid else f"it isn't validly signed: {said}"))

    msi = f"futureblitz-{version}-windows-x64.msi"
    winget = (f"https://raw.githubusercontent.com/microsoft/winget-pkgs/master/manifests/f/"
              f"FutureBlitz/FutureBlitz/{version}/{packages.WINGET_ID}.installer.yaml")
    manifest = source.fetch(winget)
    if manifest is None:
        results.append(("winget's manifest", f"microsoft/winget-pkgs has no {version}"))
    elif msi in sums and f"InstallerSha256: {sums[msi].upper()}" not in manifest.decode():
        results.append(("winget's manifest", f"its InstallerSha256 isn't {msi}'s"))
    else:
        results.append(("winget's manifest", None))

    formula = source.fetch(f"https://raw.githubusercontent.com/{tap}/HEAD/Formula/futureblitz.rb")
    if formula is None:
        results.append(("the Homebrew formula", f"{tap} has no Formula/futureblitz.rb"))
    else:
        text = formula.decode()
        archives = [f"futureblitz-{version}-{target}.tar.gz" for target in TARGETS
                    if not target.startswith("windows")]
        stale = [name for name in archives if name in sums and f'sha256 "{sums[name]}"' not in text]
        if f'version "{version}"' not in text:
            results.append(("the Homebrew formula", f"{tap}'s formula isn't at {version}"))
        elif stale:
            results.append(("the Homebrew formula", f"it hasn't {', '.join(stale)}'s SHA-256"))
        else:
            results.append(("the Homebrew formula", None))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version", help="the published release's version, like 1.0.0")
    parser.add_argument("--repository", default=REPOSITORY)
    parser.add_argument("--tap", default=TAP)
    arguments = parser.parse_args()
    try:
        version = packages.check_version(arguments.version.removeprefix("v"))
        results = check(version, Source(os.environ.get("GITHUB_TOKEN")), authenticode,
                        arguments.repository, arguments.tap)
    except (packages.ReleaseError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for name, problem in results:
        print(f"ok      {name}" if problem is None else f"FAILED  {name}: {problem}")
    return 1 if any(problem is not None for _, problem in results) else 0


if __name__ == "__main__":
    sys.exit(main())
