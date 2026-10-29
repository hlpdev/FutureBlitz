# The review of M8 step 2 (releases)

16 confirmed findings, each with its Status.

## cli-1 (medium): fb new . (and ./, x/., ..) refuses an empty folder, and says "." is the folder's name

cli/src/new.cpp:214. new_command names the package by folder.filename() of the path exactly as typed, after dropping one trailing separator only. For ".", "./", "game/.", or "x/..", filename() is "." or "..", so package_name_error rejects it. The message then adds "it's the folder's name", which is wrong: the folder is mygame, not ".". docs/tooling.md says fb new makes the package in the folder, named by the folder's name, and refuses only a folder that's there and isn't empty. An empty current folder is there and is empty, so `mkdir mygame && cd mygame && fb new .`, a common way to start, should work. Giving the same folder by its absolute path does work.

Shown by: In an empty folder named mygame: `fb.exe new .` and `fb.exe new "$(pwd -W)"`. In an empty folder named other: `fb.exe new ./`. From the parent folder: `fb.exe new mygame/..` and `fb.exe new dotted/.`. All with C:/Users/Hunter/Projects/build-m8-release/cli/fb.exe, in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s2/cli/t4

Expected: `fb new .` in the empty folder mygame makes package mygame there, as `fb new <absolute path>` does. At the very least, the error shouldn't say "." is the folder's name.

Actual: `error: a package's name is an identifier, like game or my_physics, not "."; it's the folder's name`, exit 2. The same happens for ./, dotted/., and mygame/.. (".."). `fb new C:/Users/.../t4/mygame` prints "Created package mygame", exit 0.

Skeptic: Ran C:/Users/Hunter/Projects/build-m8-release/cli/fb.exe (fb 0.1.0 (b6b944f, windows-x64)) in fresh empty folders under C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s2/v-cli/t4. `fb new .` in empty mygame, `fb new ./` in empty other, `fb new dotted/.` all print `error: a package's name is an identifier, like game or my_physics, not "."; it's the folder's name`, exit 2. `fb new mygame/..` says the same with "..". `fb new "$(pwd -W)"` in the same empty mygame prints `Created package mygame in .../t4/mygame`, exit 0, and writes .gitignore, main.fb, and project.toml. Cause: cli/src/new.cpp lines 44-49 drop only a trailing separator and then take folder.filename() of the path as typed, with no absolute()/lexically_normal(). docs/tooling.md line 124 says fb new names the package by the folder's name and refuses only a folder that's there and isn't empty, so an empty current folder should work. Medium: `mkdir x && cd x && fb new .` is a common way to start, and the message is wrong as well.

Fix: Normalize the path before taking its name, for example with std::filesystem::absolute(folder).lexically_normal(), dropping any trailing empty part, and take the name from that. For a root, like C:/, where no name is left, say that the folder needs a name, not 'not ""'. Add "." in an empty folder to the cli test.

Status: fixed: the package is named by the folder's own name, from its absolute, normal path, so `fb new .`, `game/.`, and `game/./` work; a root, which has no name, says so; the cli test makes dotted/. and slashed/./ and checks a root

## install-1 (medium): notices/ has no winpthreads license, though every Windows release ships winpthreads and links it into fb

cmake/install.cmake:143. On Windows, fb_release_notices is llvm, mingw-w64 and gcc. cmake/notices/mingw-w64.txt is MinGW-w64's COPYING.MinGW-w64-runtime.txt. That file never mentions winpthreads, which carries its own license: MIT for the mingw-w64 project, plus a BSD-style notice from Lockless Inc. that requires binary redistributions to reproduce it. The release redistributes winpthreads in three places: libwinpthread.a in lib/windows-x64/ (every program links it), pthread.h in lib/windows-x64/include/ (the release workflow gathers winpthreads' headers on purpose), and inside fb.exe and fb-bindgen.exe, which are linked fully static on MSYS2. A local CLion install also ships it as bin/libwinpthread-1.dll.

Shown by: cmake --install C:/Users/Hunter/Projects/build-m8-release --prefix <scratch>/rel; then grep -n -i 'pthread\|Lockless' cmake/notices/mingw-w64.txt finds nothing. ls <scratch>/rel/lib/windows-x64/libwinpthread.a and <scratch>/rel/bin/libwinpthread-1.dll both exist, and objdump -p bin/fb.exe shows libwinpthread-1.dll as an import. The text that is missing is in C:/Users/Hunter/AppData/Local/Programs/CLion/bin/mingw/licenses/winpthreads/COPYING.

Expected: notices/ has winpthreads' COPYING (MIT plus the Lockless Inc. notice), since the release ships winpthreads as an archive, as headers and inside its own executables.

Actual: notices/ has mingw-w64.txt, gcc.txt and llvm.txt, and none of them has winpthreads' license.

Skeptic: I ran cmake --install of build-m8-release into scratch rel/. lib/windows-x64/ has libwinpthread.a and libpthread.a, include/ has pthread.h, pthread_time.h and the rest, bin/ has libwinpthread-1.dll, and objdump -p bin/fb.exe imports libwinpthread-1.dll. release.yml lines 135-145 gather winpthreads' headers on purpose, and tooling.md says Windows programs link libwinpthread statically. grep -i 'pthread|Lockless' cmake/notices/*.txt matches nothing: mingw-w64.txt is the runtime's COPYING only. CLion's mingw/licenses/winpthreads/COPYING is MIT plus Lockless Inc.'s BSD notice, and that notice says binary redistributions must reproduce it. notices/ never ships it, though tooling.md's notices bullet means to cover the C runtime pieces a release ships.

Fix: Add cmake/notices/winpthreads.txt, taken from winpthreads' COPYING (MSYS2 installs it under /mingw64/share/licenses/winpthreads/), and add winpthreads to fb_release_notices for WIN32. Mention it in tooling.md's notices bullet.

Status: fixed: cmake/notices/winpthreads.txt, winpthreads' COPYING with its Lockless Inc. notice, is installed on Windows; the install test checks each OS's notices

## install-2 (medium): The Windows release ships libxml2 inside lld.exe, with no notice for it

cmake/install.cmake:139. The official LLVM Windows installer builds lld.exe with libxml2 linked in statically, which lld-link uses to merge manifests. install.cmake copies that lld.exe into bin/. On Windows, the only notice for it is llvm.txt (Apache-2.0 with LLVM exceptions), and no notice covers libxml2, which is MIT and requires its copyright notice in copies. On Linux, libxml2.so.2 is bundled and gets its Debian copyright file in notices/shared/, so only Windows is missing it.

Shown by: cmake --install C:/Users/Hunter/Projects/build-m8-release --prefix <scratch>/rel. Then grep -a -c 'libxml2 built without' <scratch>/rel/bin/lld.exe gives 1, and lld.exe's strings include xmlParseMemory, xmlParseMarkupDecl and similar. grep -il 'libxml\|Veillard' <scratch>/rel/notices/*.txt finds nothing.

Expected: A release that ships lld.exe with libxml2 inside it also ships libxml2's license.

Actual: notices/ has nothing for libxml2 on Windows.

Skeptic: In the installed rel/bin/lld.exe, strings shows 'libxml2 built without HTML support', 'Fatal: program compiled against libxml %d using libxml %d', xmlParseElementDecl/xmlParseStartTag messages and about 750 xml* strings. Its DLL imports are only KERNEL32, ADVAPI32, ntdll and OLEAUT32, so libxml2 is linked in statically. grep -il 'libxml|Veillard' rel/notices/*.txt finds nothing. release.yml line 129 takes the same official LLVM installer, so a Windows release ships libxml2 (MIT, notice required) with no notice. zlib and zstd are not linked in: lld's strings say it was built without them.

Fix: Add cmake/notices/libxml2.txt (libxml2's Copyright file) and install it on WIN32. Better still, check the LLVM release's build script for every other library it links into lld.exe, LLVM-C.dll and libclang.dll, and add each one's notice.

Status: fixed: cmake/notices/libxml2.txt, libxml2's Copyright, is installed on Windows; of the LLVM installer's binaries only lld.exe has libxml2 in it, and none has zlib or zstd; the install test checks it

## install-3 (medium): fb, fb_lsp and fb-bindgen contain toml++ (MIT), and notices/ has no license for it

cmake/install.cmake:131. fb_compiler links tomlplusplus::tomlplusplus, and fb, fb_lsp and fb-bindgen all link fb_compiler. That puts toml++ inside every executable the release ships, on every OS. toml++'s MIT license requires its notice in all copies, binary ones included. The install copies notices only from third_party's notices folder (std's bundled libraries) and from cmake/notices/ (llvm, mingw-w64, gcc, glibc, linux-headers), so toml++ has none. fast_float is also compiled in, but it can be used under BSL-1.0, which needs no notice in binaries, so only toml++ is a problem.

Shown by: grep -a -o 'toml::v[0-9a-z_]*\|tomlplusplus' <scratch>/rel/bin/fb.exe finds thousands of toml++ symbols. ls <scratch>/rel/notices shows no toml file.

Expected: notices/ has toml++'s LICENSE, since the release's own executables contain toml++.

Actual: There is no toml++ notice in the release.

Skeptic: compiler/CMakeLists.txt line 31 links fb_compiler PRIVATE with tomlplusplus::tomlplusplus. grep -a -c 'toml::v' rel/bin/fb.exe gives 381 (toml::v3 symbols). The installed notices/ lists argon2 through zstd plus gcc, llvm and mingw-w64, with no toml++ entry, and cmake/notices/ has none either. toml++'s MIT license requires its notice in all copies. fast_float can be used under BSL-1.0, which needs no notice in binaries, so it is fine.

Fix: Write toml++'s LICENSE into notices/, for example a notice from FetchContent's tomlplusplus_SOURCE_DIR/LICENSE written the way third_party writes the others, and install it with the release notices.

Status: fixed: toml++'s LICENSE is installed as notices/tomlplusplus.txt on every OS; the install test checks it

## workflow-1 (medium): install.sh word-splits the startup files' paths, so a home folder with a space gets its PATH line in the wrong files

tools/install/install.sh:134. profiles is a space-separated string ("$HOME/.profile $HOME/.bashrc" and the rest), and `for profile in $profiles` splits it on spaces. A home folder like '/home/John Smith' turns into the words '/home/John', 'Smith/.profile', '/home/John' and 'Smith/.bashrc'. The script appends the PATH line to a file named /home/John and to Smith/.profile and Smith/.bashrc under the current folder. It never touches the real startup files, and it still reports that PATH was added. Where /home is root-owned, the append fails and set -e stops the script after the install has already been done.

Shown by: Fake release archive (scratchpad/review-m8s2/workflow/fb.tar.gz, bin/fb a script that prints a version) plus a uname shim. Command: cd cwd; HOME="$S/homes/John Smith" SHELL=/bin/bash PATH="$S/shim:$PATH" sh ../install.sh --archive ../fb.tar.gz

Expected: One line each in '/home/John Smith/.profile' and '/home/John Smith/.bashrc'.

Actual: Prints 'Added ... to PATH in .../homes/John', then '... in Smith/.profile' and '... in Smith/.bashrc'. The files created are homes/John, cwd/Smith/.profile and cwd/Smith/.bashrc, and 'John Smith' gets no startup file at all.

Skeptic: Reproduced in Git Bash with a fake linux-x64 archive and a uname shim (scratch .../review-m8s2/refute-wf/t1). Run from cwd/ with HOME='.../homes/John Smith' SHELL=/bin/bash, install.sh printed 'Added ... to PATH in .../homes/John', then '... in Smith/.profile' and '... in Smith/.bashrc', and exited 0. The files it created were homes/John, cwd/Smith/.profile and cwd/Smith/.bashrc. 'John Smith' got no startup file. Cause: `for profile in $profiles` at install.sh:134 splits the space-joined string. The zsh and fish cases split the same way.

Fix: Don't keep the paths in one string. For example, have each case run `set -- "$HOME/.profile" "$HOME/.bashrc"` and loop with `for profile in "$@"`. Or call an add_to_profile function once per file. Add a CI or ctest case that uses a HOME with a space.

Status: fixed: install.sh adds the line to each startup file through a function, never a split string; install_sh_test.sh installs into a home folder with a space and checks the files

## workflow-2 (medium): On macOS with bash, install.sh creates ~/.bash_profile, which then hides an existing ~/.profile

tools/install/install.sh:121. A bash login shell reads only the first file it finds out of ~/.bash_profile, ~/.bash_login and ~/.profile. On macOS with SHELL=*/bash, the script always appends to ~/.bash_profile and creates it if it's missing. A user whose settings live in ~/.profile (or ~/.bash_login) loses all of them in every new terminal, and only fb's PATH line is left.

Shown by: HOME has a .profile that contains `export FOO=from-profile`. Shims fake uname -s as Darwin. Commands: HOME="$H" bash -lc 'echo $FOO' (before), then HOME="$H" SHELL=/bin/bash FAKE_OS=Darwin sh install.sh --archive fb.tar.gz, then HOME="$H" bash -lc 'echo $FOO' (after).

Expected: FOO is still set in a new login shell, and fb's bin is on PATH.

Actual: before: [from-profile]; after: []. The script created .bash_profile, so bash no longer reads .profile.

Skeptic: HOME had a .profile with `export FOO=from-profile`. Before the install, `bash -lc 'echo $FOO'` printed [from-profile]. I then ran install.sh with FAKE_OS=Darwin and SHELL=/bin/bash. It created .bash_profile holding only the FutureBlitz PATH line, and after that `bash -lc 'echo $FOO'` printed []. tooling.md does say '.bash_profile on macOS', but creating it unconditionally stops bash from reading the user's existing .profile, so their settings are lost in every new terminal.

Fix: For bash on macOS, append to the first of ~/.bash_profile, ~/.bash_login and ~/.profile that exists. Create ~/.bash_profile only when none of them exists, or create it with `[ -f ~/.profile ] && . ~/.profile` before the export line. Update tooling.md's list of files to match.

Status: fixed: bash on macOS gets the line in the first of .bash_profile, .bash_login, and .profile there is, and a new .bash_profile only when there's none; install_sh_test.sh checks a .profile isn't hidden

## cli-2 (low): fb new under a path through a file reports "couldn't write .../project.toml", hiding that the folder couldn't be made

cli/src/new.cpp:238. create_directories(folder, failed) can fail, for example when a parent is a file, but its error isn't checked on its own. The loop then fails on the first file and reports "couldn't write <folder>/project.toml". That doesn't say the folder couldn't be made, or why (afile is a file).

Shown by: In C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s2/cli/t2: `echo hi > afile; C:/Users/Hunter/Projects/build-m8-release/cli/fb.exe new afile/game`

Expected: An error that the folder afile/game couldn't be made, ideally with the system's reason (afile isn't a folder).

Actual: `error: couldn't write afile/game/project.toml`, exit 1

Skeptic: In .../v-cli/t2: `echo hi > afile; fb new afile/game` printed `error: couldn't write afile/game/project.toml`, exit 1. cli/src/new.cpp line 68 calls create_directories(folder, failed) and doesn't check `failed` there. The first loop pass (line 79) then reports the write failure, so the user never learns the folder couldn't be made, or why.

Fix: Check `failed` right after create_directories, and report something like "couldn't make the folder {}: {}" with failed.message(). Add a test of a parent that's a file.

Status: fixed: a folder that can't be made says so, with the system's reason; the cli test makes one under a file

## cli-3 (low): fb new con/nul/aux/com1 on Windows says "<name> is a file, not a folder"

cli/src/new.cpp:227. con, nul, aux, and com1 are valid identifiers, so package_name_error accepts them. On Windows, std::filesystem::exists is true for these reserved device names and is_directory is false, so fb new says there's a file of that name in the folder. There's no such file, which sends the user looking for something that isn't there.

Shown by: In an empty scratch folder (…/review-m8s2/cli/t2): `C:/Users/Hunter/Projects/build-m8-release/cli/fb.exe new con` (and nul, aux, com1)

Expected: An error that says Windows reserves the name for a device, or that it can't be a folder's name, not that a file is there.

Actual: `error: con is a file, not a folder`, exit 1. No file named con exists.

Skeptic: In .../v-cli/t2, which holds only afile: `fb new con`, `fb new nul`, `fb new aux`, and `fb new com1` each printed `error: <name> is a file, not a folder`, exit 1. No such files exist (ls -A shows only afile). The cause is new.cpp lines 56-58: on Windows, exists() is true for reserved device names and is_directory() is false, so the code takes them for a file. The message is misleading and Windows-only, so the severity is low.

Fix: On Windows, check for the reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with any extension and in any case) before the exists check, and say the name is reserved. Or check is_regular_file rather than !is_directory, and report anything else as not a folder.

Status: fixed: on Windows, CON, PRN, AUX, NUL, COM1 to COM9, and LPT1 to LPT9, in any case, are refused as devices' names; the cli test checks four

## cli-4 (low): fb --version takes an enclosing repository's commit when the source isn't a git checkout

cli/CMakeLists.txt:13. FB_COMMIT runs `git rev-parse --short=7 HEAD` in PROJECT_SOURCE_DIR. Git walks up from there. If the source isn't a git checkout (a source archive, or a vendored copy) but sits inside some other git working tree, the build records that other repository's commit, and fb --version prints it as fb's own. docs/tooling.md says the commit comes from git, or is "unknown" otherwise.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s2/cli/commit_probe.cmake holds cli/CMakeLists.txt's FB_COMMIT block, verbatim. With `outer/` a git repo with one commit (17b9f9a) and `outer/FutureBlitz-0.1.0/` a plain folder: `cmake -DSRC=.../outer/FutureBlitz-0.1.0 -P commit_probe.cmake`. Then the same for a plain folder outside any repo.

Expected: FB_COMMIT=unknown, since FutureBlitz-0.1.0 isn't a git checkout.

Actual: FB_COMMIT=17b9f9a, the outer repository's commit. Outside any repository it's unknown, as it should be.

Skeptic: Copied cli/CMakeLists.txt's FB_COMMIT block (lines 4-14) verbatim into .../v-cli/probe.cmake, with PROJECT_SOURCE_DIR=${SRC}. Made outer/ a git repo with one commit (8f44ab2) and outer/FutureBlitz-0.1.0/ a plain folder inside it. `cmake -DSRC=.../outer/FutureBlitz-0.1.0 -P probe.cmake` printed FB_COMMIT=8f44ab2, the outer repo's commit. A plain folder outside any repo printed FB_COMMIT=unknown. So a source archive unpacked or vendored inside another git tree gets that tree's commit, and fb --version would show it as fb's own. That contradicts docs/tooling.md line 122, which promises the commit fb is built from, or "unknown". It's an uncommon setup, so low.

Fix: Use the commit only when `git rev-parse --show-toplevel` run in PROJECT_SOURCE_DIR is PROJECT_SOURCE_DIR itself, compared after file(REAL_PATH). Otherwise keep "unknown".

Status: fixed: cmake/commit.cmake's fb_source_commit() takes git's commit only where the checkout's top folder is the source's; commit_test.cmake checks a checkout and a plain folder inside one

## install-6 (low): bundle.cmake ignores DESTDIR, so `DESTDIR=... cmake --install` fails on Linux and macOS

cmake/install.cmake:118. The install(CODE) block sets FB_BUNDLE_BIN to "${CMAKE_INSTALL_PREFIX}/bin" and FB_BUNDLE_NOTICES to the same prefix's notices/shared, without $ENV{DESTDIR}. CMake's own install rules write to $ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}. With DESTDIR set, which is the usual way to stage a package, fb's executables land under DESTDIR, but bundle.cmake looks for them at the bare prefix. file(GET_RUNTIME_DEPENDENCIES) then fails on the missing files, or, if an older install is there, it patches and re-signs that one instead. (From reading: this code runs only on Linux and macOS.)

Shown by: Reading cmake/install.cmake lines 118-125 and cmake/bundle.cmake, against how cmake_install.cmake prefixes $ENV{DESTDIR} for install(TARGETS) and install(FILES).

Expected: A DESTDIR install bundles the libraries into $DESTDIR<prefix>/bin.

Actual: bundle.cmake reads and writes <prefix>/bin without DESTDIR.

Skeptic: I built a small CMake project in scratch/destdir with the same pattern: install(FILES f.txt DESTINATION bin), then install(CODE) that sets B to \${CMAKE_INSTALL_PREFIX}/bin and checks for f.txt. Running DESTDIR=<stage> cmake --install --prefix /opt/fbx installed the file under <stage>/.../opt/fbx/bin/f.txt, while the CODE block printed 'missing C:/Program Files/Git/opt/fbx/bin/f.txt'. install.cmake lines 121 and 125 build FB_BUNDLE_BIN and FB_BUNDLE_NOTICES the same way, without $ENV{DESTDIR}. bundle.cmake then gives file(GET_RUNTIME_DEPENDENCIES) paths under the bare prefix, which don't exist under a DESTDIR stage, and copies, patchelfs and re-signs there. The release itself uses --prefix with no DESTDIR, so this does not affect today's releases.

Fix: Use "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin" and the same for notices/shared in the install(CODE) block.

Status: fixed: the bundling's bin/ and notices/shared/ are under $ENV{DESTDIR}; the install test installs under DESTDIR too outside Windows, as does the release workflow's build, which is its test on Linux and macOS

## workflow-3 (low): Both install scripts delete the working install before checking that the new archive runs on this machine

tools/install/install.sh:106. install.sh (lines 106-112) and install.ps1 (lines 64-82) rm -rf or Remove-Item each installed folder and move the new one in, and only then run `fb --version`. Neither one checks that an --archive/-Archive is for this machine's target either: the folder name futureblitz-<v>-<target> is never compared with $target. So a linux-arm64 archive on x64, a macOS archive on Linux, or a truncated or broken release replaces a working fb with one that can't run, and the script then fails. On Windows the failure also isn't the script's own message. With ErrorActionPreference=Stop, `& fb.exe` throws a raw ResourceUnavailable error, so 'the installed fb doesn't run' never prints.

Shown by: sh: install fb.tar.gz into HOME=$S/v/home, then `sh install.sh --archive bad.tar.gz`, where bad.tar.gz holds futureblitz-0.1.0-linux-arm64/bin/fb and that fb exits 126 with 'Exec format error'. PowerShell: install.ps1 -Archive fb.zip (the real release from cmake --install build-m8-release) into FUTUREBLITZ_HOME='...\home ps\fb', then `pwsh -File install.ps1 -Archive bad.zip -NoModifyPath`, where bad.zip's bin\fb.exe is a text file.

Expected: The script refuses an archive for another target, or one whose fb doesn't run, before touching the install that's there. The working fb stays.

Actual: sh: the linux-arm64 archive is accepted on linux-x64 and the old bin/ is gone; afterwards $root/bin/fb --version exits 126. The fake linux-x64 archive was also accepted on a faked Darwin. PowerShell: 'ResourceUnavailable: ... install.ps1:79 $installed = & (Join-Path $bin 'fb.exe') --version'. The installed bin\fb.exe is now the broken one ('not a valid application').

Skeptic: sh: I installed a working fake (`fb 0.1.0 (x, linux-x64)`), then ran `install.sh --archive bad.tar.gz`, where bad.tar.gz holds futureblitz-0.1.0-linux-arm64/bin/fb and that fb exits 126. The script accepted the arm64 archive on linux-x64, replaced bin/, then failed with 'the installed fb doesn't run'. The old fb now exits 126. A linux-x64 archive was also accepted on a faked Darwin (workflow-2's run). PowerShell: I installed the real release from cmake --install build-m8-release, zipped, into FUTUREBLITZ_HOME='...\home ps\fb'. Then `install.ps1 -Archive bad.zip -NoModifyPath`, where bin\fb.exe is a text file, gave a raw 'ResourceUnavailable: ... install.ps1:79 ... Program fb.exe failed to run' instead of the script's own message. The installed fb.exe afterwards is 'not a valid application for this OS platform'. I rate it low: it only happens with an explicitly passed wrong or broken archive. The download path picks $target itself, and a truncated download fails in tar or Expand-Archive before anything is removed.

Fix: Check that the release folder's name ends in -$target (and that .zip / .tar.gz is the right kind). Run "$release/bin/fb --version" from the unpacked folder before replacing anything. Or move the old folders aside and restore them if the check fails. In install.ps1, wrap the call in try/catch so the script's own message shows.

Status: fixed: both scripts check the archive's folder is this machine's target and run its fb from where it's unpacked before replacing anything, and install.ps1 shows its own message for a failure; install_sh_test.sh and install_ps1_test.ps1 check an archive for another target and a broken one leave the install there

## workflow-4 (low): `irm ... | iex`, as the README gives it, leaves ErrorActionPreference=Stop, TLS 1.2-only and the script's variables in the user's PowerShell session

tools/install/install.ps1:15. Invoke-Expression runs the script text in the caller's scope. Everything the script sets at its top level stays in the user's interactive session after it finishes or fails: $ErrorActionPreference='Stop', $ProgressPreference='SilentlyContinue', [Net.ServicePointManager]::SecurityProtocol=Tls12, and $root, $repository, $Version, $work and the rest. After installing, any non-terminating error in that terminal turns into a terminating one, and progress bars are gone. This is the README's main Windows install command.

Shown by: iex.ps1 in scratch: set FUTUREBLITZ_HOME, FUTUREBLITZ_VERSION=0.1.0, then `Get-Content -Raw install.ps1 | Invoke-Expression` inside try/catch, printing the preferences before and after. Run with pwsh -NoProfile -File iex.ps1 and powershell.exe (5.1) -File iex.ps1.

Expected: The session's preferences and variables are what they were before.

Actual: Both PowerShell 7 and 5.1 print 'before: EAP=Continue Progress=Continue TLS=SystemDefault' and 'after: EAP=Stop Progress=SilentlyContinue TLS=Tls12 root=...\iexhome repository=hlpdev/FutureBlitz'.

Skeptic: iex.ps1 in scratch pipes install.ps1 through Invoke-Expression inside try/catch. pwsh 7.6.9 and powershell.exe 5.1 both printed 'before: EAP=Continue Progress=Continue TLS=SystemDefault root=' and 'after: EAP=Stop Progress=SilentlyContinue TLS=Tls12 root=...\iexhome repository=hlpdev/FutureBlitz'. The run failed at the latest-release lookup (404, since there's no published release), and the leak shows on that failure path too. I rate it low: it lasts only for that session, and the script itself says to open a new terminal.

Fix: Put the body in a function or script block that takes the parameters, e.g. `function Install-FutureBlitz { param(...) ... }` and then call it with @PSBoundParameters. The preference and variable assignments are then local to it. Use -bor to add Tls12 to SecurityProtocol instead of replacing it, and restore the old value in the finally block.

Status: fixed: install.ps1's work is a function's, which adds TLS 1.2 to the protocols with -bor and puts them back, and the script removes the function and its parameters; install_ps1_test.ps1 dot-sources it, as iex runs it, and checks the session

## workflow-5 (low): install.ps1 rewrites the user's PATH expanded and as REG_SZ, so its %VAR% entries become fixed paths

tools/install/install.ps1:86. [Environment]::GetEnvironmentVariable('Path','User') reads HKCU\Environment\Path with %VAR% references expanded. SetEnvironmentVariable then writes the joined string back as REG_SZ. So every user PATH entry like %USERPROFILE%\AppData\Local\Microsoft\WindowsApps (Windows' default) or %SCOOP%\shims is replaced by its expansion at install time, and the value loses its REG_EXPAND_SZ type. Entries that depend on a variable stop following later changes to it. The workflow's MSI step (release.yml line 280) does the same to the runner's PATH.

Shown by: Read-only check: this machine's HKCU\Environment Path is ExpandString. Same calls on a scratch key HKCU\Software\fb-review-scratch (deleted afterwards): SetValue('Path','%USERPROFILE%\AppData\Local\Microsoft\WindowsApps;C:\x','ExpandString'), then $read=GetValue('Path') (this is what GetEnvironmentVariable(User) does), then SetValue('Path',"$read;C:\fb\bin") (this is what SetEnvironmentVariable(User) does).

Expected: Existing entries stay as written (%USERPROFILE%\...), the value stays REG_EXPAND_SZ, and only ...\.futureblitz\bin is appended.

Actual: read back: 'C:\Users\Hunter\AppData\Local\Microsoft\WindowsApps;C:\x'; kind now: String; raw now: 'C:\Users\Hunter\AppData\Local\Microsoft\WindowsApps;C:\x;C:\fb\bin'.

Skeptic: Read-only check of HKCU\Environment. TEMP is ExpandString with the raw value '%USERPROFILE%\AppData\Local\Temp', and [Environment]::GetEnvironmentVariable('TEMP','User') returns 'C:\Users\Hunter\AppData\Local\Temp', so the User-scope read install.ps1:86 uses expands %VAR% references. Path is ExpandString here too. install.ps1:89 writes that expanded, joined string back with SetEnvironmentVariable, which stores a plain string (REG_SZ via RegistryKey.SetValue). So %VAR% entries such as Windows' default %USERPROFILE%\...\WindowsApps become fixed paths, and the value loses its expand type. This machine's Path has no %VAR% entries, so nothing here is harmed. I didn't write to the real key. I rate it low: the expanded paths resolve the same at install time, and they break only if the variable changes later.

Fix: Read the raw value with (Get-Item HKCU:\Environment).GetValue('Path','', 'DoNotExpandEnvironmentNames'). Write it with Set-ItemProperty -Type ExpandString, or RegistryKey.SetValue(..., ExpandString). Then broadcast WM_SETTINGCHANGE, for example by setting and clearing a dummy user variable with SetEnvironmentVariable. Compare entries after expansion so an existing entry is still found.

Status: fixed: install.ps1 reads the user's PATH from the registry unexpanded, compares its entries expanded, writes it back as REG_EXPAND_SZ, and broadcasts the change; the workflow's Windows test adds a %USERPROFILE% entry and checks it and the type stay, and its MSI step keeps them too

## workflow-6 (low): install.sh doesn't strip a leading v from FUTUREBLITZ_VERSION, though --version and install.ps1 do

tools/install/install.sh:14. --version strips the v (`${2#v}`), and install.ps1 trims it from both -Version and $env:FUTUREBLITZ_VERSION. install.sh takes FUTUREBLITZ_VERSION as it is, so FUTUREBLITZ_VERSION=v0.1.0 asks for release vv0.1.0 and an asset futureblitz-v0.1.0-....

Shown by: Curl shim that logs its arguments. Commands: FUTUREBLITZ_VERSION=v0.1.0 sh install.sh --no-modify-path, then sh install.sh --version v0.1.0 --no-modify-path.

Expected: Both download .../releases/download/v0.1.0/futureblitz-0.1.0-linux-x64.tar.gz.

Actual: The env var gives 'curl -fsSL https://github.com/hlpdev/FutureBlitz/releases/download/vv0.1.0/futureblitz-v0.1.0-linux-x64.tar.gz' and "error: couldn't download futureblitz-v0.1.0-linux-x64.tar.gz; is v0.1.0 a release...". --version v0.1.0 gets the right URL.

Skeptic: The code is plain. install.sh:14 takes FUTUREBLITZ_VERSION as it is, while --version strips the v (`${2#v}`, line 27). Line 94 builds `releases/download/v$version/futureblitz-$version-...`, so FUTUREBLITZ_VERSION=v0.1.0 asks for vv0.1.0 and futureblitz-v0.1.0-*.tar.gz. install.ps1:40 does TrimStart('v') for both sources, so the two scripts disagree.

Fix: After the options are parsed, add `version="${version#v}"`, and drop the strip from the --version case.

Status: fixed: install.sh strips a v from the version after reading the options, from FUTUREBLITZ_VERSION as from --version; install_sh_test.sh checks both URLs

## workflow-7 (low): install.ps1 -NoModifyPath still says 'Open a new terminal to use fb anywhere.'

tools/install/install.ps1:98. The closing message always prints, even when -NoModifyPath has left PATH alone, so a new terminal won't find fb. install.sh checks PATH and prints the export line instead.

Shown by: pwsh -NoProfile -File install.ps1 -Archive fb.zip -NoModifyPath (FUTUREBLITZ_HOME set to a scratch folder).

Expected: With -NoModifyPath, the message says fb isn't on PATH and shows the bin folder to add.

Actual: 'Installed fb 0.1.0 (b6b944f, windows-x64) in ...' then 'Open a new terminal to use fb anywhere.'

Skeptic: Seen during workflow-3's run: `install.ps1 -Archive fb.zip -NoModifyPath` printed 'Installed fb 0.1.0 (b6b944f, windows-x64) in ...\home ps\fb' and then 'Open a new terminal to use fb anywhere.', though PATH was left alone. Line 98 prints it unconditionally. install.sh instead checks PATH and prints the export line.

Fix: With -NoModifyPath, or when $bin isn't on the user's PATH, print 'Add <bin> to PATH to use fb' instead.

Status: fixed: with -NoModifyPath (and install.sh's --no-modify-path) the script says to add bin to PATH; install_ps1_test.ps1 checks the message

## workflow-8 (low): Docs and the workflow's comment say the macOS fresh-machine test cuts PATH to the system's, but the install.sh step doesn't

.github/workflows/release.yml:302. docs/tooling.md line 118 says 'Windows' and macOS' runners as they come, with PATH cut to the system's', and the workflow's comment (lines 218-219) says the same. On macOS, the 'Install with install.sh' step runs install.sh and `bash -lc 'fb ...'` with the runner's full PATH. The run's log shows Homebrew's /opt/homebrew/bin being used. Only the later .pkg step uses env -i PATH=/usr/bin:/bin. So the install.sh path on macOS isn't tested the way the docs describe. The otool check covers dylibs, but not tools found on PATH.

Shown by: Reading release.yml lines 302-326 (no PATH change for macOS) against tooling.md line 118. gh run view 40960437924 --log --job 123759517220: the install.sh step runs under the runner's environment, where git is /opt/homebrew/bin/git.

Expected: The macOS install.sh step runs with PATH=/usr/bin:/bin:/usr/sbin:/sbin, as the docs say.

Actual: It inherits the runner's PATH, Homebrew included.

Skeptic: tooling.md says the workflow checks each release on 'Windows' and macOS' runners as they come, with PATH cut to the system's ... each installing with its script', and release.yml's comment above the test job says the same. The 'Install with install.sh' step (release.yml ~302-326), which runs on macOS too, doesn't change PATH: it runs install.sh and `bash -lc 'fb ...'` with the runner's full PATH. Only the Windows steps and the later .pkg step (env -i PATH=/usr/bin:/bin) cut it. I didn't pull the run log; the mismatch between the code and the doc is plain. Note: one of my read-only checks, for workflow-5, listed every HKCU\Environment value to my own tool output, and some of those are API tokens. I didn't repeat or use them anywhere.

Fix: In that step, on macOS, export PATH=/usr/bin:/bin:/usr/sbin:/sbin before install.sh, or run the bash -lc calls under env -i as the .pkg step does. Or change the docs to say only the .pkg step cuts PATH.

Status: fixed: the macOS install.sh step runs with PATH=/usr/bin:/bin:/usr/sbin:/sbin and a home of its own, which tooling.md now says
