#!/bin/sh
# install.sh on fake releases, whose fb only says its version, on a machine that uname, a shim,
# says is Linux or macOS, with downloads from a folder of fake GitHub Releases, which curl, a shim
# too, serves: sh install_sh_test.sh <install.sh> <output folder>.
set -eu

script="$1"
out="$2"
# Git's sh on Windows, whose tar takes C: for a host.
if command -v cygpath >/dev/null 2>&1; then
    script="$(cygpath -u "$script")"
    out="$(cygpath -u "$out")"
fi
script="$(cd "$(dirname "$script")" && pwd)/$(basename "$script")"
rm -rf "$out"
mkdir -p "$out/shim" "$out/archives" "$out/releases"

fail() {
    echo "FAILED: $*" >&2
    exit 1
}

# uname says what FAKE_OS and FAKE_ARCH say, and curl logs what it's asked for and gives the file
# of FAKE_RELEASES' for its URL, as GitHub's releases/latest/download/ and releases/download/
# would, or fails as curl -f does.
cat >"$out/shim/uname" <<'EOF'
#!/bin/sh
case "$1" in
-s) echo "$FAKE_OS" ;;
-m) echo "$FAKE_ARCH" ;;
*) echo "$FAKE_OS" ;;
esac
EOF
cat >"$out/shim/curl" <<'EOF'
#!/bin/sh
echo "curl $*" >>"$CURL_LOG"
releases="https://github.com/hlpdev/FutureBlitz/releases"
case "$2" in
"$releases/latest/download/"*) file="latest/${2##*/}" ;;
"$releases/download/"*) file="${2#"$releases/download/"}" ;;
*) exit 22 ;;
esac
[ -n "${FAKE_RELEASES:-}" ] && [ -f "$FAKE_RELEASES/$file" ] || exit 22
cp "$FAKE_RELEASES/$file" "$4"
EOF
chmod +x "$out/shim/uname" "$out/shim/curl"
PATH="$out/shim:$PATH"
export PATH
# Every install goes in a home of the test's own, never the machine's.
unset FUTUREBLITZ_HOME FUTUREBLITZ_VERSION

# archive <name> <target> <fb's body>: a release for target whose bin/fb runs body.
archive() {
    folder="$out/archives/$1/futureblitz-0.1.0-$2"
    mkdir -p "$folder/bin" "$folder/std"
    printf '#!/bin/sh\n%s\n' "$3" >"$folder/bin/fb"
    chmod +x "$folder/bin/fb"
    echo "$1" >"$folder/std/which.txt"
    tar -C "$out/archives/$1" -czf "$out/archives/$1.tar.gz" "futureblitz-0.1.0-$2"
}
archive good linux-x64 'echo "fb 0.1.0 (test, linux-x64)"'
archive newer linux-x64 'echo "fb 0.1.0 (newer, linux-x64)"'
archive arm linux-arm64 'echo "fb 0.1.0 (test, linux-arm64)"'
archive broken linux-x64 'exit 126'
archive mac macos-arm64 'echo "fb 0.1.0 (test, macos-arm64)"'

# A file's SHA-256, as the release's SHA256SUMS has it.
sum() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# release <version> <archive>...: a fake GitHub Release of each archive (as name:target), with
# its SHA256SUMS, as sha256sum writes it, with a name that isn't an archive's too.
release() {
    folder="$out/releases/v$1"
    version="$1"
    shift
    mkdir -p "$folder"
    : >"$folder/SHA256SUMS"
    for each in "$@"; do
        file="futureblitz-$version-${each#*:}.tar.gz"
        cp "$out/archives/${each%%:*}.tar.gz" "$folder/$file"
        printf '%s  %s\n' "$(sum "$folder/$file")" "$file" >>"$folder/SHA256SUMS"
    done
    printf '%s  install.sh\n' "$(sum "$script")" >>"$folder/SHA256SUMS"
}
release 0.1.0 good:linux-x64 mac:macos-arm64
mkdir -p "$out/releases/latest"
cp "$out/releases/v0.1.0/SHA256SUMS" "$out/releases/latest/SHA256SUMS"
# 0.1.1's archive isn't the one its SHA256SUMS names, and 0.1.2 has none for linux-x64.
release 0.1.1 newer:linux-x64
printf '%s  futureblitz-0.1.1-linux-x64.tar.gz\n' "$(sum "$out/archives/good.tar.gz")" \
    >"$out/releases/v0.1.1/SHA256SUMS"
release 0.1.2 mac:macos-arm64

# A home folder with a space: the PATH line goes in its .profile and .bashrc, and nowhere else.
home="$out/homes/John Smith"
mkdir -p "$home" "$out/cwd"
(cd "$out/cwd" && HOME="$home" SHELL=/bin/bash FAKE_OS=Linux FAKE_ARCH=x86_64 \
    sh "$script" --archive "$out/archives/good.tar.gz") || fail "install.sh failed"
grep -qF "$home/.futureblitz/bin" "$home/.profile" || fail "no PATH line in .profile"
grep -qF "$home/.futureblitz/bin" "$home/.bashrc" || fail "no PATH line in .bashrc"
[ ! -e "$out/homes/John" ] && [ ! -e "$out/cwd/Smith" ] || fail "a PATH line went elsewhere"
[ "$(cat "$home/.futureblitz/std/which.txt")" = good ] || fail "the release isn't installed"

# Running it again updates, and adds no second PATH line.
HOME="$home" SHELL=/bin/bash FAKE_OS=Linux FAKE_ARCH=x86_64 \
    sh "$script" --archive "$out/archives/newer.tar.gz" >/dev/null || fail "the update failed"
[ "$(cat "$home/.futureblitz/std/which.txt")" = newer ] || fail "the update isn't installed"
[ "$(grep -cF "$home/.futureblitz/bin" "$home/.bashrc")" = 1 ] || fail "two PATH lines"

# Another machine's release, or one whose fb doesn't run, leaves the one that's there.
for bad in arm broken mac; do
    if HOME="$home" SHELL=/bin/bash FAKE_OS=Linux FAKE_ARCH=x86_64 \
        sh "$script" --archive "$out/archives/$bad.tar.gz" >/dev/null 2>&1; then
        fail "the $bad archive was installed"
    fi
    [ "$(cat "$home/.futureblitz/std/which.txt")" = newer ] || fail "the $bad archive replaced it"
done

# macOS' bash reads the first of .bash_profile, .bash_login, and .profile, so a .profile that's
# there gets the line, and no .bash_profile hides it.
mac="$out/homes/mac"
mkdir -p "$mac"
echo 'export FOO=from-profile' >"$mac/.profile"
HOME="$mac" SHELL=/bin/bash FAKE_OS=Darwin FAKE_ARCH=arm64 \
    sh "$script" --archive "$out/archives/mac.tar.gz" >/dev/null || fail "the macOS install failed"
[ ! -e "$mac/.bash_profile" ] || fail "a .bash_profile hides .profile"
grep -qF "$mac/.futureblitz/bin" "$mac/.profile" || fail "no PATH line in macOS' .profile"

# --no-modify-path leaves the startup files.
plain="$out/homes/plain"
mkdir -p "$plain"
HOME="$plain" SHELL=/bin/zsh FAKE_OS=Linux FAKE_ARCH=x86_64 \
    sh "$script" --archive "$out/archives/good.tar.gz" --no-modify-path >/dev/null ||
    fail "--no-modify-path failed"
[ ! -e "$plain/.zshrc" ] || fail "--no-modify-path wrote .zshrc"

# m9-ops-6: the latest release is the one releases/latest/download/'s SHA256SUMS is of, with no
# call to GitHub's API, and m9-ops-5: its archive is installed once it's that file's hash.
fresh="$out/homes/download"
mkdir -p "$fresh"
FAKE_RELEASES="$out/releases"
CURL_LOG="$out/curl-latest.log"
export FAKE_RELEASES CURL_LOG
HOME="$fresh" FAKE_OS=Linux FAKE_ARCH=x86_64 \
    sh "$script" --no-modify-path >"$out/latest.txt" 2>&1 ||
    fail "the latest release wasn't installed: $(cat "$out/latest.txt")"
[ "$(cat "$fresh/.futureblitz/std/which.txt")" = good ] || fail "the latest release isn't installed"
grep -qF "Downloading FutureBlitz 0.1.0 for linux-x64" "$out/latest.txt" ||
    fail "the latest release's version wasn't said: $(cat "$out/latest.txt")"
! grep -qF "api.github.com" "$CURL_LOG" || fail "GitHub's API was called: $(cat "$CURL_LOG")"
grep -qF "/releases/latest/download/SHA256SUMS" "$CURL_LOG" ||
    fail "no SHA256SUMS: $(cat "$CURL_LOG")"
grep -qF "/releases/download/v0.1.0/futureblitz-0.1.0-linux-x64.tar.gz" "$CURL_LOG" ||
    fail "the archive wasn't downloaded: $(cat "$CURL_LOG")"

# An archive whose hash isn't its SHA256SUMS', or a release without one for this machine, isn't
# installed, and leaves the one that's there.
newer="$(sum "$out/archives/newer.tar.gz")"
good="$(sum "$out/archives/good.tar.gz")"
mismatch="futureblitz-0.1.1-linux-x64.tar.gz's SHA-256 is $newer, not the $good"
for bad in "0.1.1:$mismatch" "0.1.2:has no archive for linux-x64"; do
    if HOME="$fresh" FAKE_OS=Linux FAKE_ARCH=x86_64 \
        sh "$script" --version "${bad%%:*}" --no-modify-path >"$out/bad.txt" 2>&1; then
        fail "${bad%%:*} was installed"
    fi
    grep -qF "${bad#*:}" "$out/bad.txt" || fail "${bad%%:*} said: $(cat "$out/bad.txt")"
    [ "$(cat "$fresh/.futureblitz/std/which.txt")" = good ] || fail "${bad%%:*} replaced it"
done
unset FAKE_RELEASES

# A script cut short, as a dropped connection leaves it, runs none of its body.
cut="$out/truncated.sh"
head -c "$(($(wc -c <"$script") * 6 / 10))" "$script" >"$cut"
short="$out/homes/short"
mkdir -p "$short"
if HOME="$short" SHELL=/bin/bash FAKE_OS=Linux FAKE_ARCH=x86_64 \
    sh "$cut" --archive "$out/archives/good.tar.gz" >/dev/null 2>&1; then
    fail "a script cut short ran"
fi
[ ! -e "$short/.futureblitz" ] && [ ! -e "$short/.profile" ] || fail "a script cut short installed"

# A version's v is its tag's, from FUTUREBLITZ_VERSION as from --version.
for how in environment option; do
    CURL_LOG="$out/curl-$how.log"
    export CURL_LOG
    if [ "$how" = environment ]; then
        HOME="$plain" FUTUREBLITZ_VERSION=v0.1.0 FAKE_OS=Linux FAKE_ARCH=x86_64 \
            sh "$script" --no-modify-path >/dev/null 2>&1 || true
    else
        HOME="$plain" FAKE_OS=Linux FAKE_ARCH=x86_64 \
            sh "$script" --version v0.1.0 --no-modify-path >/dev/null 2>&1 || true
    fi
    grep -qF "/releases/download/v0.1.0/SHA256SUMS" "$CURL_LOG" ||
        fail "the $how's version downloads $(cat "$CURL_LOG")"
done

echo "install.sh passed"
