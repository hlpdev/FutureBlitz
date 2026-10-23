// Prints the console's output code page, like "out=437", for the tests that check fb and the
// programs it builds give the console, which the shell shares, back as they found it.
#include <stdio.h>
#include <windows.h>

int main(void) {
    printf("out=%u\n", GetConsoleOutputCP());
    return 0;
}
