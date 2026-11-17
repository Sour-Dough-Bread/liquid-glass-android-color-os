#include "exec_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace glass {
namespace {

std::string fail(const char* what) {
    return std::string(what) + ": " + strerror(errno);
}

bool probeExecmem(std::string* why) {
    const long page = sysconf(_SC_PAGESIZE);

    // RWX in one go, which is what gum asks for where it believes it can.
    void* p = mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        *why = fail("execmem: anonymous RWX mmap denied");
        return false;
    }
    munmap(p, page);

    // Write first, execute after: the W^X route. Same permission, but check the
    // route gum falls back to as well rather than assume.
    p = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        *why = fail("anonymous RW mmap failed");
        return false;
    }
    static_cast<volatile unsigned char*>(p)[0] = 0xd4;
    const bool ok = mprotect(p, page, PROT_READ | PROT_EXEC) == 0;
    if (!ok) *why = fail("execmem: mprotect(RX) of written anonymous memory denied");
    munmap(p, page);
    return ok;
}

bool probeExecmod(std::string* why) {
    const long page = sysconf(_SC_PAGESIZE);
    const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *why = fail("cannot open /proc/self/exe");
        return false;
    }
    // A fresh private mapping of our own executable: the same file, so the same
    // label the real patch is checked against, but a mapping nothing executes.
    void* p = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        *why = fail("cannot map /proc/self/exe executable");
        return false;
    }
    bool ok = mprotect(p, page, PROT_READ | PROT_WRITE) == 0;
    if (!ok) {
        *why = fail("mprotect(RW) of a private file mapping failed");
    } else {
        // Rewrite one byte with itself: the page is now a private copy, which
        // is the state the kernel checks execmod on.
        auto* b = static_cast<volatile unsigned char*>(p);
        b[0] = b[0];
        ok = mprotect(p, page, PROT_READ | PROT_EXEC) == 0;
        if (!ok) *why = fail("execmod: mprotect(RX) of a modified file page denied");
        if (ok) {
            ok = mprotect(p, page, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
            if (!ok) *why = fail("execmod: mprotect(RWX) of a modified file page denied");
        }
    }
    munmap(p, page);
    return ok;
}

}  // namespace

bool canPatchCode(std::string* why) {
    std::string scratch;
    std::string* w = why != nullptr ? why : &scratch;
    return probeExecmem(w) && probeExecmod(w);
}

}  // namespace glass
