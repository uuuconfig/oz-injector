// Test host: a plain console process that idles so the injector has a target.
#include <windows.h>
#include <cstdio>

int main() {
    std::printf("probe_host pid=%lu\n", GetCurrentProcessId());
    std::fflush(stdout);
    // Wait to be killed.
    for (;;) Sleep(1000);
}
