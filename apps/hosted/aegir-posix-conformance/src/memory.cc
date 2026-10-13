/*
 * aegir-posix-conformance: the memory area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Cases from mmap.html, munmap.html, mprotect.html, posix_madvise.html,
 * posix_memalign.html, msync.html, mlock.html, munlock.html, mlockall.html,
 * munlockall.html, shm_open.html and shm_unlink.html of The Open Group Base
 * Specifications Issue 8.
 */

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

constexpr size_t kPage = 4096;
constexpr size_t kRun = 3 * kPage;

}  // namespace

void memory_surface()
{
    using namespace aegir_posix_conformance;
    section("memory");

    /* mmap: RETURN VALUE (MAP_FAILED on failure), ERRORS ("[EINVAL] The value of
     * len is zero"; "[EINVAL] The value of flags is invalid (neither MAP_PRIVATE
     * nor MAP_SHARED is set)"). */
    errno = 0;
    check("mmap", "len zero is EINVAL",
          mmap(nullptr, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
                  MAP_FAILED &&
              errno == EINVAL);
    errno = 0;
    check("mmap", "neither MAP_PRIVATE nor MAP_SHARED is EINVAL",
          mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
              errno == EINVAL);

    errno = 0;
    char *const run = static_cast<char *>(
        mmap(nullptr, kRun, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check("mmap", "a fresh anonymous mapping is mapped", run != MAP_FAILED);
    if (run != MAP_FAILED) {
        check("mmap", "a fresh anonymous mapping is zero-filled",
              run[0] == 0 && run[kRun - 1] == 0);
        run[0] = 'a';
        run[kRun - 1] = 'z';
        check("mmap", "the mapping reads its bytes back", run[0] == 'a' && run[kRun - 1] == 'z');
    }

    /* MAP_FIXED: the standard has it replace the mapping at the address; Aegir's
     * profile refuses it (specs/posix.md). A named deviation, not a pass. */
    deviation("mmap", "MAP_FIXED",
              "Aegir's profile refuses MAP_FIXED (specs/posix.md); the standard replaces");
    skip("mmap", "a file-backed mapping", "the file surface is the files area's");

    /* munmap: RETURN VALUE (zero on success), ERRORS ("[EINVAL] The addr argument
     * is not a multiple of the page size"; "[EINVAL] The len argument is 0"). */
    errno = 0;
    check("munmap", "len zero is EINVAL",
          run != MAP_FAILED && munmap(run, 0) == -1 && errno == EINVAL);
    errno = 0;
    check("munmap", "an unaligned addr is EINVAL",
          run != MAP_FAILED && munmap(run + 1, kPage) == -1 && errno == EINVAL);
    check("munmap", "a mapping is released", run != MAP_FAILED && munmap(run, kRun) == 0);

    /* A release owes a churning allocator memory that is there and zero-filled
     * when the address is handed out again. */
    char *const again = static_cast<char *>(
        mmap(nullptr, kRun, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check("munmap", "a mapping after the release succeeds", again != MAP_FAILED);
    if (again != MAP_FAILED) {
        check("munmap", "the mapping after the release is zero-filled",
              again[0] == 0 && again[kRun - 1] == 0);
        (void)munmap(again, kRun);
    }

    /* mprotect: RETURN VALUE (zero on success), ERRORS ("[ENOMEM] Addresses ...
     * are not mapped"). The byte is written *before* the page is narrowed: the
     * read-only case reads a value, it does not create one. */
    char *const page = static_cast<char *>(
        mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    errno = 0;
    check("mprotect", "an unmapped range is ENOMEM",
          mprotect(reinterpret_cast<void *>(kPage), kPage, PROT_READ) == -1 && errno == ENOMEM);
    if (page != MAP_FAILED) {
        page[0] = 'q';
    }
    check("mprotect", "a mapped page is made read-only",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ) == 0);
    check("mprotect", "a read-only page still reads", page != MAP_FAILED && page[0] == 'q');
    check("mprotect", "it is made writable again",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ | PROT_WRITE) == 0);
    /* The EINVAL for an unaligned addr is at the *syscall*: libc rounds addr
     * down to the pages containing the range (musl's mprotect), which is the
     * description's own wording, so it is not reachable through the interface. */
    skip("mprotect", "an unaligned addr is EINVAL",
         "musl rounds addr down to a page boundary, so the syscall's EINVAL is unreachable");

    /* posix_madvise: RETURN VALUE ("zero" or "an error number"), ERRORS
     * ("[EINVAL] The value of advice is invalid"; "[ENOMEM] Addresses ... outside
     * the range allowed for the address space"). */
    check("posix_madvise", "NORMAL returns zero",
          page != MAP_FAILED && posix_madvise(page, kPage, POSIX_MADV_NORMAL) == 0);
    check("posix_madvise", "an invalid advice is EINVAL",
          page != MAP_FAILED && posix_madvise(page, kPage, 0x7fff) == EINVAL);
    check("posix_madvise", "an outside range is ENOMEM",
          posix_madvise(reinterpret_cast<void *>(kPage), kPage, POSIX_MADV_NORMAL) == ENOMEM);
    if (page != MAP_FAILED) {
        (void)munmap(page, kPage);
    }

    /* posix_memalign: RETURN VALUE ("zero" or "an error number"), ERRORS
     * ("[EINVAL] The value of alignment is not a power of two multiple of
     * sizeof(void *)"). */
    void *aligned = nullptr;
    check("posix_memalign", "a sufficiently-aligned block is returned",
          posix_memalign(&aligned, 64, kPage) == 0 && aligned != nullptr &&
              (reinterpret_cast<uintptr_t>(aligned) % 64) == 0);
    if (aligned != nullptr) {
        static_cast<char *>(aligned)[0] = 'x';
        free(aligned);
        aligned = nullptr;
    }
    check("posix_memalign", "an alignment that is not a power of two is EINVAL",
          posix_memalign(&aligned, 3, kPage) == EINVAL);

    /* The holes. Each call must fail; the errno is printed, so a hole that
     * closes is visible as the case going red rather than as silence. */
    void *const hole = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                            -1, 0);
    if (hole != MAP_FAILED) {
        errno = 0;
        unavailable("msync", "is not implemented", msync(hole, kPage, MS_SYNC) == -1, errno);
        errno = 0;
        unavailable("mlock", "is not implemented", mlock(hole, kPage) == -1, errno);
        errno = 0;
        unavailable("munlock", "is not implemented", munlock(hole, kPage) == -1, errno);
        (void)munmap(hole, kPage);
    }
    errno = 0;
    unavailable("mlockall", "is not implemented", mlockall(MCL_CURRENT) == -1, errno);
    errno = 0;
    unavailable("munlockall", "is not implemented", munlockall() == -1, errno);
    errno = 0;
    unavailable("shm_open", "is not implemented",
                shm_open("/aegir-conformance", O_CREAT | O_RDWR, 0600) == -1, errno);
    errno = 0;
    unavailable("shm_unlink", "is not implemented", shm_unlink("/aegir-conformance") == -1,
                errno);
}

}  // namespace aegir_posix_conformance
