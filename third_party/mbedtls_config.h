/* What TF-PSA-Crypto's default configuration needs for std.crypto, which a program can call from
 * any thread: its key slots and random generator are shared, so they take pthreads' locks
 * (winpthreads' on MinGW). */

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
