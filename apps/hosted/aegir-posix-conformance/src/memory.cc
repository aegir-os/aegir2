/*
 * aegir-posix-conformance: the memory area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`): the RETURN VALUE and
 * ERRORS sections are the source, and the clause is quoted where a case is not
 * self-evident. A case whose subject Aegir does not implement asserts the
 * failure (the hole); one where Aegir answers something the page does not is a
 * named deviation, with the measurement.
 */

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
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
    section("memory");

    /* mmap.html, ERRORS. */
    errno = 0;
    check("mmap", "len zero is EINVAL",
          mmap(nullptr, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
                  MAP_FAILED &&
              errno == EINVAL); /* "[EINVAL] The value of len is zero." */
    errno = 0;
    check("mmap", "neither MAP_PRIVATE nor MAP_SHARED is EINVAL",
          mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
              errno == EINVAL); /* "[EINVAL] ... neither MAP_PRIVATE nor MAP_SHARED is set." */
    /* mmap.html, ERRORS, the MAP_ANONYMOUS-unset half: "[EBADF] The fildes
     * argument is not a valid open file descriptor." */
    errno = 0;
    void *const no_file = mmap(nullptr, kPage, PROT_READ, MAP_PRIVATE, -1, 0);
    int const no_file_errno = errno;
    if (no_file == MAP_FAILED && no_file_errno == EBADF) {
        check("mmap", "a file-backed mapping with a bad fildes is EBADF", true);
    } else {
        if (no_file != MAP_FAILED) {
            (void)munmap(no_file, kPage);
        }
        char reason[96];
        snprintf(reason, sizeof(reason), "mmap.html names EBADF; Aegir answers errno %d",
                 no_file_errno);
        deviation("mmap", "a file-backed mapping with a bad fildes is EBADF", reason);
    }

    /* mmap.html, DESCRIPTION: "Anonymous memory objects shall be initialized to
     * all bits zero." */
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

    /* mmap.html, DESCRIPTION and ERRORS: "It is implementation-defined whether
     * MAP_FIXED shall be supported", and an implementation that does not support
     * it answers ENOTSUP ("[ENOTSUP] MAP_FIXED or MAP_PRIVATE was specified in
     * the flags argument and the implementation does not support this
     * functionality"). Aegir's profile refuses MAP_FIXED (specs/posix.md), so the
     * clause is ENOTSUP; what it answers is measured. */
    if (run != MAP_FAILED) {
        errno = 0;
        void *const fixed =
            mmap(run, kPage, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (fixed == MAP_FAILED && errno == ENOTSUP) {
            check("mmap", "an unsupported MAP_FIXED is ENOTSUP", true);
        } else {
            deviation("mmap", "an unsupported MAP_FIXED is ENOTSUP",
                      "Aegir refuses MAP_FIXED with EINVAL; mmap.html names ENOTSUP");
        }
    }
    skip("mmap", "a file-backed mapping", "the file surface is the files area's");

    /* munmap.html, ERRORS. */
    errno = 0;
    check("munmap", "len zero is EINVAL",
          run != MAP_FAILED && munmap(run, 0) == -1 && errno == EINVAL);
    /* "[EINVAL] Addresses in the range [addr, addr + len) are outside the valid
     * range for the address space of a process." */
    errno = 0;
    if (munmap(reinterpret_cast<void *>(kPage), kPage) == -1 && errno == EINVAL) {
        check("munmap", "an address outside the address space is EINVAL", true);
    } else {
        deviation("munmap", "an address outside the address space is EINVAL",
                  "the call did not answer EINVAL");
    }
    /* "[EINVAL] The addr argument is not a multiple of the page size" (the
     * may-fail half). */
    errno = 0;
    check("munmap", "an unaligned addr is EINVAL",
          run != MAP_FAILED && munmap(run + 1, kPage) == -1 && errno == EINVAL);
    check("munmap", "a mapping is released", run != MAP_FAILED && munmap(run, kRun) == 0);

    /* A release owes a churning allocator memory that is there and zero-filled
     * when the address is handed out again (mmap.html's zero initialization). */
    char *const again = static_cast<char *>(
        mmap(nullptr, kRun, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check("munmap", "a mapping after the release succeeds", again != MAP_FAILED);
    if (again != MAP_FAILED) {
        check("munmap", "the mapping after the release is zero-filled",
              again[0] == 0 && again[kRun - 1] == 0);
        (void)munmap(again, kRun);
    }

    /* mprotect.html, ERRORS: "[ENOMEM] Addresses in the range [addr, addr + len)
     * are invalid for the address space of a process, or specify one or more
     * pages which are not mapped." RETURN VALUE: zero on success. */
    char *const page = static_cast<char *>(
        mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    errno = 0;
    check("mprotect", "an unmapped range is ENOMEM",
          mprotect(reinterpret_cast<void *>(kPage), kPage, PROT_READ) == -1 && errno == ENOMEM);
    /* The byte is written *before* the page is narrowed: mprotect.html's cases
     * read what the mapping holds, they do not create it. */
    if (page != MAP_FAILED) {
        page[0] = 'q';
    }
    check("mprotect", "a mapped page is made read-only",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ) == 0);
    check("mprotect", "a read-only page still reads", page != MAP_FAILED && page[0] == 'q');
    check("mprotect", "it is made writable again",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ | PROT_WRITE) == 0);
    /* "[EINVAL] The addr argument is not a multiple of the page size" is the
     * may-fail half, and libc rounds addr down to the pages containing the range
     * (musl's mprotect), which is the description's own wording -- so it is not
     * reachable through the interface. */
    skip("mprotect", "an unaligned addr is EINVAL",
         "musl rounds addr down to a page boundary, so the syscall's EINVAL is unreachable");

    /* posix_madvise.html, RETURN VALUE and ERRORS. */
    check("posix_madvise", "NORMAL returns zero",
          page != MAP_FAILED && posix_madvise(page, kPage, POSIX_MADV_NORMAL) == 0);
    check("posix_madvise", "an invalid advice is EINVAL",
          page != MAP_FAILED && posix_madvise(page, kPage, 0x7fff) == EINVAL);
    check("posix_madvise", "an outside range is ENOMEM",
          posix_madvise(reinterpret_cast<void *>(kPage), kPage, POSIX_MADV_NORMAL) == ENOMEM);
    if (page != MAP_FAILED) {
        (void)munmap(page, kPage);
    }

    /* posix_memalign.html, ERRORS. */
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

    /* The holes: interfaces Aegir does not implement. Each call must fail, and
     * the errno is printed, so a hole that closes goes red rather than silent.
     * The pages name what a successful call would do (msync.html, mlock.html,
     * mlockall.html, shm_open.html, shm_unlink.html). */
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
