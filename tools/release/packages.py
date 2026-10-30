#!/usr/bin/env python3
"""What a release's package managers read, made from its SHA256SUMS: winget's manifests, for
FutureBlitz.FutureBlitz in microsoft/winget-pkgs, and Homebrew's formula, for hlpdev/homebrew-tap.
release.yml makes both to check them, and publish.yml again to publish them; docs/tooling.md's
Releases says what each is.

    python tools/release/packages.py product-code <version>
    python tools/release/packages.py winget <version> <SHA256SUMS> <folder> [--url-base URL]
    python tools/release/packages.py homebrew <version> <SHA256SUMS> <file> [--url-base URL]

The MSI's ProductCode is made from its version, so a manifest knows it without reading the MSI,
and release.yml builds the MSI with it. --url-base is where the release's files are, by default
its GitHub Release; a test points it at a folder of its own.
"""

import argparse
import re
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REPOSITORY = "hlpdev/FutureBlitz"
WINGET_ID = "FutureBlitz.FutureBlitz"
WINGET_SCHEMA = "1.10.0"
# The namespace each version's ProductCode is made in, which never changes.
PRODUCT_CODES = uuid.UUID("849f0a4b-0cd7-48f1-88fb-ad85111a13e0")
VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
# The one sentence every package manager describes FutureBlitz with, and what a release installs.
SUMMARY = "A compiled, general-purpose language with BASIC's readability and native speed."
INSTALLS = ("This installs fb, which builds, runs, tests, and formats FutureBlitz programs, fb-lsp, "
            "its language server, and fb-bindgen, which makes modules from C headers.")


class ReleaseError(Exception):
    pass


def check_version(version: str) -> str:
    if not VERSION.match(version):
        raise ReleaseError(f"a release's version is three numbers, like 1.0.0, not {version!r}")
    return version


def product_code(version: str) -> str:
    """The MSI's ProductCode for a version, as WiX takes it: upper case, without braces."""
    return str(uuid.uuid5(PRODUCT_CODES, f"FutureBlitz {check_version(version)}")).upper()


def upgrade_code() -> str:
    """The MSI's UpgradeCode, from tools/install/futureblitz.wxs, the same for every version."""
    wxs = (ROOT / "tools" / "install" / "futureblitz.wxs").read_text(encoding="utf-8")
    found = re.search(r'<Package\b[^>]*\bUpgradeCode="([0-9A-Fa-f-]{36})"', wxs)
    if not found:
        raise ReleaseError("futureblitz.wxs's Package has no UpgradeCode")
    return found.group(1).upper()


def read_sums(path: Path) -> dict[str, str]:
    """SHA256SUMS, as sha256sum writes it: each file's name and its hash, in lower case."""
    sums = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        found = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if not found:
            raise ReleaseError(f"{path}:{number} isn't a hash and a file's name: {line!r}")
        sums[found.group(2)] = found.group(1).lower()
    return sums


def sum_of(sums: dict[str, str], name: str) -> str:
    if name not in sums:
        raise ReleaseError(f"SHA256SUMS has no {name}")
    return sums[name]


def url_base(version: str, given: str | None) -> str:
    base = given or f"https://github.com/{REPOSITORY}/releases/download/v{version}"
    return base.rstrip("/")


def winget(version: str, sums: dict[str, str], base: str) -> dict[str, str]:
    """winget's three manifests, by their files' names: the version's, the installer's, and the
    default locale's. The installer is the MSI, which installs for the user alone."""
    msi = f"futureblitz-{version}-windows-x64.msi"
    code = "{" + product_code(version) + "}"
    head = f"PackageIdentifier: {WINGET_ID}\nPackageVersion: {version}\n"
    foot = f"ManifestVersion: {WINGET_SCHEMA}\n"

    def schema(kind: str) -> str:
        url = f"https://aka.ms/winget-manifest.{kind}.{WINGET_SCHEMA}.schema.json"
        return f"# yaml-language-server: $schema={url}\n\n"

    return {
        f"{WINGET_ID}.yaml": schema("version") + head
        + "DefaultLocale: en-US\nManifestType: version\n" + foot,
        f"{WINGET_ID}.installer.yaml": schema("installer") + head
        + "MinimumOSVersion: 10.0.0.0\n"
        + "InstallerType: wix\n"
        + "Scope: user\n"
        + "InstallModes:\n- interactive\n- silent\n- silentWithProgress\n"
        + "UpgradeBehavior: install\n"
        + "Commands:\n- fb\n- fb-lsp\n- fb-bindgen\n"
        + f"ProductCode: '{code}'\n"
        + "AppsAndFeaturesEntries:\n"
        + "- DisplayName: FutureBlitz\n"
        + "  Publisher: FutureBlitz\n"
        + f"  ProductCode: '{code}'\n"
        + f"  UpgradeCode: '{{{upgrade_code()}}}'\n"
        + "Installers:\n"
        + "- Architecture: x64\n"
        + f"  InstallerUrl: {base}/{msi}\n"
        + f"  InstallerSha256: {sum_of(sums, msi).upper()}\n"
        + "ManifestType: installer\n" + foot,
        f"{WINGET_ID}.locale.en-US.yaml": schema("defaultLocale") + head
        + "PackageLocale: en-US\n"
        + "Publisher: FutureBlitz\n"
        + "PublisherUrl: https://futureblitz.dev\n"
        + f"PublisherSupportUrl: https://github.com/{REPOSITORY}/issues\n"
        + "PackageName: FutureBlitz\n"
        + "PackageUrl: https://futureblitz.dev\n"
        + "License: Apache-2.0\n"
        + f"LicenseUrl: https://github.com/{REPOSITORY}/blob/HEAD/LICENSE\n"
        + f"ShortDescription: {SUMMARY}\n"
        + f"Description: {SUMMARY} {INSTALLS}\n"
        + "Moniker: futureblitz\n"
        + "Tags:\n- basic\n- compiler\n- language\n- programming-language\n"
        + f"ReleaseNotesUrl: https://github.com/{REPOSITORY}/releases/tag/v{version}\n"
        + "ManifestType: defaultLocale\n" + foot,
    }


def homebrew(version: str, sums: dict[str, str], base: str) -> str:
    """The formula, futureblitz.rb, which installs the prebuilt archive for the Mac's or the Linux
    machine's target into libexec, as the release's folder, with its programs linked into bin."""

    def archive(target: str, indent: str) -> str:
        name = f"futureblitz-{version}-{target}.tar.gz"
        return (f'{indent}url "{base}/{name}"\n'
                f'{indent}sha256 "{sum_of(sums, name)}"\n')

    return (
        "# Generated by tools/release/packages.py from the release's SHA256SUMS; don't edit it.\n"
        "class Futureblitz < Formula\n"
        f'  desc "{SUMMARY}"\n'
        '  homepage "https://futureblitz.dev"\n'
        f'  version "{version}"\n'
        '  license "Apache-2.0"\n'
        "\n"
        "  on_macos do\n"
        "    # fb runs on macOS 15 and newer.\n"
        "    depends_on macos: :sequoia\n"
        "\n"
        "    on_arm do\n"
        + archive("macos-arm64", "      ")
        + "    end\n"
        "    on_intel do\n"
        + archive("macos-x64", "      ")
        + "    end\n"
        "  end\n"
        "\n"
        "  on_linux do\n"
        "    on_arm do\n"
        + archive("linux-arm64", "      ")
        + "    end\n"
        "    on_intel do\n"
        + archive("linux-x64", "      ")
        + "    end\n"
        "  end\n"
        "\n"
        "  def install\n"
        "    # The release's folder as it is: fb finds lib/ and std/ beside its bin/, through the\n"
        "    # links.\n"
        '    libexec.install Dir["*"]\n'
        '    bin.install_symlink libexec/"bin/fb", libexec/"bin/fb-lsp", libexec/"bin/fb-bindgen"\n'
        "  end\n"
        "\n"
        "  test do\n"
        '    assert_match "fb #{version} ", shell_output("#{bin}/fb --version")\n'
        '    system bin/"fb", "new", "hello"\n'
        '    cd "hello" do\n'
        '      assert_equal "Hello, world!", shell_output("#{bin}/fb run").strip\n'
        "    end\n"
        "  end\n"
        "end\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    code = commands.add_parser("product-code", help="the MSI's ProductCode for a version")
    code.add_argument("version")
    for name, what in (("winget", "the folder the manifests go in"), ("homebrew", "the .rb file")):
        command = commands.add_parser(name)
        command.add_argument("version")
        command.add_argument("sums", type=Path, help="the release's SHA256SUMS")
        command.add_argument("out", type=Path, help=what)
        command.add_argument("--url-base", help="where the release's files are")
    arguments = parser.parse_args()

    try:
        if arguments.command == "product-code":
            print(product_code(arguments.version.removeprefix("v")))
            return 0

        version = check_version(arguments.version.removeprefix("v"))
        sums = read_sums(arguments.sums)
        base = url_base(version, arguments.url_base)
        if arguments.command == "winget":
            arguments.out.mkdir(parents=True, exist_ok=True)
            for name, text in winget(version, sums, base).items():
                (arguments.out / name).write_text(text, encoding="utf-8", newline="\n")
        else:
            arguments.out.parent.mkdir(parents=True, exist_ok=True)
            arguments.out.write_text(homebrew(version, sums, base), encoding="utf-8", newline="\n")
    except (ReleaseError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
