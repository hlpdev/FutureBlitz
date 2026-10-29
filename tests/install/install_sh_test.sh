#!/bin/sh
# install.sh on fake releases, whose fb only says its version, on a machine that uname, a shim,
# says is Linux or macOS: sh install_sh_test.sh <install.sh> <output folder>.
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
mkdir -p "$out/shim" "$out/archives"

fail() {
    echo "FAILED: $*" >&2
    exit 1
}

# uname says what FAKE_OS and FAKE_ARCH say, and curl only logs what it's asked for.
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
exit 22
EOF
chmod +x "$out/shim/uname" "$out/shim/curl"
PATH="$out/shim:$PATH"
export PATH

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
    grep -qF "/releases/download/v0.1.0/futureblitz-0.1.0-linux-x64.tar.gz" "$CURL_LOG" ||
        fail "the $how's version downloads $(cat "$CURL_LOG")"
done

echo "install.sh passed"
