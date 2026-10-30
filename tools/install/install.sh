#!/bin/sh
# Installs FutureBlitz on Linux or macOS from its GitHub Releases into ~/.futureblitz, or
# $FUTUREBLITZ_HOME, and puts its bin/ on PATH in the shell's startup file. Running it again
# updates it.
#
#   curl -fsSL https://futureblitz.dev/install.sh | sh
#   curl -fsSL https://futureblitz.dev/install.sh | sh -s -- --version 1.0.0
#   sh install.sh --archive futureblitz-1.0.0-linux-x64.tar.gz
#
# Each release has it too: https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh
#
# --no-modify-path leaves the shell's startup files as they are. A release it downloads is
# checked against that release's SHA256SUMS before it's unpacked, and the latest is the one
# GitHub's releases/latest/download/ redirects to, which needs no API, so no API's rate limit
# stops it. An --archive is installed as it is.
#
# All of it is in main, which the last line runs, so a download cut short runs none of it.
set -eu

fail() {
    echo "error: $*" >&2
    exit 1
}

download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        fail "downloading FutureBlitz needs curl or wget"
    fi
}

# Sets sha256 to a file's SHA-256, in lowercase hex.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256="$(sha256sum "$1")"
    elif command -v shasum >/dev/null 2>&1; then
        sha256="$(shasum -a 256 "$1")"
    elif command -v openssl >/dev/null 2>&1; then
        sha256="$(openssl dgst -sha256 -r "$1")"
    else
        fail "checking the download needs sha256sum, shasum, or openssl"
    fi
    sha256="$(echo "${sha256%% *}" | tr 'A-F' 'a-f')"
}

main() {
    repository="hlpdev/FutureBlitz"
    version="${FUTUREBLITZ_VERSION:-}"
    archive=""
    modify_path=1

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

    if [ -z "$archive" ]; then
        # The release's SHA256SUMS names its archive for this machine, and so the latest release's
        # version.
        if [ -z "$version" ]; then
            sums="https://github.com/$repository/releases/latest/download/SHA256SUMS"
        else
            sums="https://github.com/$repository/releases/download/v$version/SHA256SUMS"
        fi
        download "$sums" "$work/SHA256SUMS" ||
            fail "couldn't download $sums; is ${version:-the latest} a release?"
        # Each line is a hash, a space, and a name, after a '*' for a binary file; a version is
        # letters, digits, '.', '+', and '-', so the name is a file's in the folder it goes in.
        file="futureblitz-[0-9A-Za-z.+-]*-$target\.tar\.gz"
        line="$(tr -d '\r' <"$work/SHA256SUMS" |
            sed -n "s/^\([0-9a-fA-F]\{64\}\) [ *]\($file\)\$/\1 \2/p" | head -n 1)"
        [ -n "$line" ] || fail "the release's SHA256SUMS has no archive for $target"
        expected="$(echo "${line%% *}" | tr 'A-F' 'a-f')"
        name="${line#* }"
        found="${name#futureblitz-}"
        found="${found%-"$target".tar.gz}"
        [ -z "$version" ] || [ "$found" = "$version" ] ||
            fail "the SHA256SUMS of $version is for $found's archive"
        version="$found"

        echo "Downloading FutureBlitz $version for $target"
        archive="$work/$name"
        download "https://github.com/$repository/releases/download/v$version/$name" "$archive" ||
            fail "couldn't download $name; is $version a release, with one for $target?"
        sha256_of "$archive"
        [ "$sha256" = "$expected" ] ||
            fail "$name's SHA-256 is $sha256, not the $expected its release's SHA256SUMS has," \
                "so it wasn't installed"
    fi

    [ -f "$archive" ] || fail "there's no archive $archive"
    mkdir -p "$work/unpacked"
    tar -xzf "$archive" -C "$work/unpacked" || fail "couldn't unpack $archive"
    # The archive holds one folder, futureblitz-<version>-<target>, which has to be this
    # machine's, and its fb has to run here, before anything there is replaced.
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
                # A terminal's bash on macOS is a login shell, which reads the first of these
                # there is, so a new .bash_profile would hide a .profile.
                profile="$HOME/.bash_profile"
                for file in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
                    if [ -f "$file" ]; then
                        profile="$file"
                        break
                    fi
                done
                added_to "$profile"
            else
                # On Linux a terminal's bash reads .bashrc, and a login shell, as over SSH,
                # .profile.
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
}

main "$@"
