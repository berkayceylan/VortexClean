// A stand-in "application process" for the ProcessInspector tests. The test
// copies this executable into a synthetic bundle and launches it from there.
//
//   process_child                      exits on SIGTERM (default disposition)
//   process_child stubborn             ignores SIGTERM, so only SIGKILL stops it
//   process_child <mode> <seconds>     as above, with an explicit lifetime
//
// It announces readiness on stdout and never outlives its lifetime (one minute
// by default): alarm(2) delivers SIGALRM, whose default action terminates it.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "stubborn") == 0) {
        ::signal(SIGTERM, SIG_IGN);
    }
    const long requested = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 60;
    ::alarm(requested > 0 && requested <= 3600 ? static_cast<unsigned>(requested) : 60U);
    const char ready[] = "ready\n";
    (void)::write(STDOUT_FILENO, ready, sizeof(ready) - 1);
    for (;;) ::pause();
}
