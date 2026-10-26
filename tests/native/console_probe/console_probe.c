// For the tests that check fb and the programs it builds give the console, which the shell
// shares, back as they found it. Alone, it prints the console's output code page, like
// "out=437". "run <command>" and "break <command>" set the console to code page 437 and its
// screen buffer's mode without virtual terminal processing, run the command with the console as
// its stdout and stderr, and then print both as they were and are, like
// "before=437/0x3 after=437/0x3". "break" sends the command Ctrl+Break the moment it has set
// the console's code page to UTF-8, as fb and a program it builds do as they start, which is
// while they're still starting.
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("out=%u\n", GetConsoleOutputCP());
        return 0;
    }

    const int interrupt = strcmp(argv[1], "break") == 0;
    char command[8192] = "";
    for (int i = 2; i < argc; ++i) {
        strncat(command, "\"", sizeof(command) - strlen(command) - 1);
        strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
        strncat(command, "\" ", sizeof(command) - strlen(command) - 1);
    }

    SECURITY_ATTRIBUTES inherited = { sizeof(inherited), NULL, TRUE };
    HANDLE screen =
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &inherited, OPEN_EXISTING, 0, NULL);
    DWORD before = 0;
    if (screen == INVALID_HANDLE_VALUE || GetConsoleMode(screen, &before) == 0) {
        printf("no console\n");
        return 1;
    }

    before &= ~(DWORD)ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(screen, before);
    SetConsoleOutputCP(437);

    STARTUPINFOA startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = screen;
    startup.hStdError = screen;
    PROCESS_INFORMATION process;
    if (CreateProcessA(NULL, command, NULL, NULL, TRUE, interrupt ? CREATE_NEW_PROCESS_GROUP : 0,
                       NULL, NULL, &startup, &process) == 0) {
        printf("couldn't start %s\n", command);
        return 1;
    }

    if (interrupt) {
        const ULONGLONG start = GetTickCount64();
        while (GetConsoleOutputCP() != CP_UTF8 && GetTickCount64() - start < 10000) {}

        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD after = 0;
    GetConsoleMode(screen, &after);
    printf("before=437/0x%lx after=%u/0x%lx\n", (unsigned long)before, GetConsoleOutputCP(),
           (unsigned long)after);
    return 0;
}
