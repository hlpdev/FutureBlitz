#!/bin/sh
# Installs FutureBlitz on Linux or macOS from its GitHub Releases into ~/.futureblitz, or
# $FUTUREBLITZ_HOME, and puts its bin/ on PATH in the shell's startup file. Running it again
# updates it.
#
#   curl -fsSL https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh | sh
#   sh install.sh --version 0.1.0
#   sh install.sh --archive futureblitz-0.1.0-linux-x64.tar.gz
#
# --no-modify-path leaves the shell's startup files as they are.
set -eu

repository="hlpdev/FutureBlitz"
version="${FUTUREBLITZ_VERSION:-}"
archive=""
modify_path=1

fail() {
    echo "error: $*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
    --version)
        [ $# -ge 2 ] || fail "--version needs a version, like 0.1.0"
        version="$2"
        shift 2
        ;;
    --archive)
        [ $# -ge 2 ] || fail "--archive needs a release's .tar.gz"
        archive="$2"
        shift 2
        ;;
    --no-modify-path)
        modify_path=0
        shift
        ;;
    -h | --help)
        echo "Usage: install.sh [--version <version> | --archive <file.tar.gz>] [--no-modify-path]"
        echo
        echo "Installs FutureBlitz into ~/.futureblitz, or \$FUTUREBLITZ_HOME: the latest release,"
        echo "the one given, or a release's archive, and puts its bin/ on PATH unless it's told not to."
        exit 0
        ;;
    *)
        fail "unknown option '$1'; see install.sh --help"
        ;;
    esac
done
# v0.1.0 is the tag of 0.1.0, from --version or FUTUREBLITZ_VERSION.
version="${version#v}"

# The release for this machine, like linux-x64.
case "$(uname -s)" in
Linux) os=linux ;;
Darwin) os=macos ;;
*) fail "FutureBlitz's releases are for Linux, macOS, and Windows, whose is install.ps1" ;;
esac
case "$(uname -m)" in
x86_64 | amd64) arch=x64 ;;
aarch64 | arm64) arch=arm64 ;;
*) fail "FutureBlitz's releases are for x64 and arm64, not $(uname -m)" ;;
esac
# A shell under Rosetta on an arm64 Mac says x86_64.
if [ "$os" = macos ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
    arch=arm64
fi
target="$os-$arch"

root="${FUTUREBLITZ_HOME:-$HOME/.futureblitz}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        fail "downloading FutureBlitz needs curl or wget"
    fi
}

if [ -z "$archive" ]; then
    if [ -z "$version" ]; then
        download "https://api.github.com/repos/$repository/releases/latest" "$work/latest.json" ||
            fail "couldn't find FutureBlitz's latest release"
        version="$(sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' "$work/latest.json" | head -n 1)"
        [ -n "$version" ] || fail "couldn't read the latest release's version"
    fi

    name="futureblitz-$version-$target.tar.gz"
    echo "Downloading FutureBlitz $version for $target"
    archive="$work/$name"
    download "https://github.com/$repository/releases/download/v$version/$name" "$archive" ||
        fail "couldn't download $name; is $version a release, with one for $target?"
fi

[ -f "$archive" ] || fail "there's no archive $archive"
mkdir -p "$work/unpacked"
tar -xzf "$archive" -C "$work/unpacked" || fail "couldn't unpack $archive"
# The archive holds one folder, futureblitz-<version>-<target>, which has to be this machine's,
# and its fb has to run here, before anything there is replaced.
release="$(find "$work/unpacked" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
[ -n "$release" ] && [ -x "$release/bin/fb" ] || fail "$archive isn't a FutureBlitz release"
case "$(basename "$release")" in
futureblitz-*-"$target") ;;
*) fail "$archive is a release for another machine, not $target: $(basename "$release")" ;;
esac
installed="$("$release/bin/fb" --version 2>/dev/null)" || fail "$archive's fb doesn't run here"

# The release's folders replace those there, and anything else in the folder stays.
mkdir -p "$root"
for folder in "$release"/*; do
    rm -rf "$root/$(basename "$folder")"
    mv "$folder" "$root/"
done

# bin/ on PATH, in the startup files of the user's shell, once each.
line="export PATH=\"$root/bin:\$PATH\""
added_to() {
    if ! grep -qsF "$root/bin" "$1"; then
        mkdir -p "$(dirname "$1")"
        printf '\n# FutureBlitz\n%s\n' "$line" >>"$1"
        echo "Added $root/bin to PATH in $1"
    fi
}

if [ "$modify_path" = 1 ]; then
    case "${SHELL:-}" in
    */zsh) added_to "$HOME/.zshrc" ;;
    */bash)
        if [ "$os" = macos ]; then
            # A terminal's bash on macOS is a login shell, which reads the first of these there
            # is, so a new .bash_profile would hide a .profile.
            profile="$HOME/.bash_profile"
            for file in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
                if [ -f "$file" ]; then
                    profile="$file"
                    break
                fi
            done
            added_to "$profile"
        else
            # On Linux a terminal's bash reads .bashrc, and a login shell, as over SSH, .profile.
            added_to "$HOME/.profile"
            added_to "$HOME/.bashrc"
        fi
        ;;
    */fish)
        line="fish_add_path \"$root/bin\""
        added_to "$HOME/.config/fish/config.fish"
        ;;
    *) added_to "$HOME/.profile" ;;
    esac
fi

echo "Installed $installed in $root"
if [ "$os" = macos ] && ! xcrun --sdk macosx --show-sdk-path >/dev/null 2>&1; then
    echo "fb links programs with the macOS SDK; install Apple's Command Line Tools with: xcode-select --install"
fi
case ":$PATH:" in
*":$root/bin:"*) ;;
*)
    if [ "$modify_path" = 1 ]; then
        echo "Open a new terminal to use fb, or run: $line"
    else
        echo "Add $root/bin to PATH to use fb: $line"
    fi
    ;;
esac
