/*
 * aegir-posix-conformance: the POSIX conformance suite.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Derived from The Open Group Base Specifications Issue 8 (IEEE Std
 * 1003.1-2024), fetched by `make deps` into third_party/posix-spec/issue8 as a
 * *reference* (its text is not redistributed). Every case names the interface
 * it belongs to; `scripts/check_posix_coverage.py` cross-reads those names
 * against `manifests/posix-surface.toml` and the dispatcher, so an interface
 * with no case is a failed check and not a silence (specs/posix-conformance.md).
 *
 * Four kinds of line:
 *   posix: ok|FAIL  <interface>: <test>   -- an implemented interface, asserted
 *                                            against the standard
 *   posix: hole     <interface>: <test>   -- an interface Aegir does not
 *                                            implement; the call must fail, and
 *                                            the errno is printed
 *   posix: deviate  <interface>: <test>   -- a deliberate non-conformance
 *   posix: SKIP     <interface>: <test>   -- not testable in this process
 *
 * and one marker: AEGIR_POSIX_CONFORMANCE_OK|FAIL <passed>/<total>.
 *
 * A boot service: it stands itself up (the untyped, the VSpace root and the
 * window the spawn kit installed, then the heap) before its first printf.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

/* Static, like the greeter's, the test bed's and the C++ smoke's: the
 * allocator's untyped table and the scratch window's bookkeeping are tens of
 * kilobytes, and a service's stack is pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

constexpr size_t kPage = 4096;
constexpr size_t kRun = 3 * kPage;

int passed = 0;
int failed = 0;
int skipped = 0;
int holes = 0;
int deviations = 0;

void section(char const *area)
{
    printf("posix: -- %s --\n", area);
    (void)fflush(nullptr);
}

/* An implemented interface, asserted against the standard. The first argument
 * is the interface, so the coverage check can read which interfaces have cases;
 * everything after it is the case's own name. */
void check(char const *interface, char const *test, bool ok)
{
    if (ok) {
        ++passed;
        printf("posix: ok      %s: %s\n", interface, test);
    } else {
        ++failed;
        printf("posix: FAIL    %s: %s\n", interface, test);
    }
    (void)fflush(nullptr);
}

/* A hole the standard names and Aegir does not fill: the call must fail, and the
 * errno it returned is printed, so the hole is a number and not a promise. The
 * day it stops failing, this goes red. */
void unavailable(char const *interface, char const *test, bool failed_call, int err)
{
    if (failed_call) {
        ++holes;
        printf("posix: hole    %s: %s (errno %d)\n", interface, test, err);
    } else {
        ++failed;
        printf("posix: FAIL    %s: %s (expected a hole, but the call succeeded)\n", interface,
               test);
    }
    (void)fflush(nullptr);
}

/* A deliberate non-conformance (specs/posix.md): named and counted, not hidden
 * behind a pass. */
void deviation(char const *interface, char const *test, char const *reason)
{
    ++deviations;
    printf("posix: deviate %s: %s (%s)\n", interface, test, reason);
    (void)fflush(nullptr);
}

void skip(char const *interface, char const *test, char const *reason)
{
    ++skipped;
    printf("posix: SKIP    %s: %s (%s)\n", interface, test, reason);
    (void)fflush(nullptr);
}

/* The mapping authority the spawn kit installs: the delegated untyped (page
 * tables and frames are retyped from it), the VSpace root, and the window of
 * free addresses (the give_vspace grant). The pattern is the C++ smoke's, the
 * greeter's and the test bed's. */
bool adopt_memory()
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));

    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        g_objects.adopt_slots_level_two(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free,
                                        aegir::bootstrap::cnode_l1_bits(),
                                        aegir::bootstrap::cnode_bits(), 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

/* --- memory ----------------------------------------------------------------
 *
 * Cases from mmap.html, munmap.html, mprotect.html, posix_madvise.html,
 * posix_memalign.html and shm_open.html. */
void memory_surface()
{
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
    check("munmap", "len zero is EINVAL", run != MAP_FAILED && munmap(run, 0) == -1 &&
                                               errno == EINVAL);
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

    /* mprotect: RETURN VALUE (zero on success), ERRORS ("[EINVAL] The addr
     * argument is not a multiple of the page size"; "[ENOMEM] Addresses ... are
     * not mapped"). A non-null address the process was never given is the case,
     * not nullptr. */
    char *const page = static_cast<char *>(
        mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    errno = 0;
    check("mprotect", "an unmapped range is ENOMEM",
          mprotect(reinterpret_cast<void *>(kPage), kPage, PROT_READ) == -1 && errno == ENOMEM);
    /* The byte is written *before* the page is narrowed: the read-only case
     * reads a value, it does not create one -- writing after the mprotect faults
     * the very page the case is about. */
    if (page != MAP_FAILED) {
        page[0] = 'q';
    }
    check("mprotect", "a mapped page is made read-only",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ) == 0);
    check("mprotect", "a read-only page still reads", page != MAP_FAILED && page[0] == 'q');
    check("mprotect", "it is made writable again",
          page != MAP_FAILED && mprotect(page, kPage, PROT_READ | PROT_WRITE) == 0);
    /* mprotect.html's EINVAL for an unaligned addr is at the *syscall*: libc
     * rounds addr down to the pages containing the range (musl's mprotect),
     * which is the description's own wording, so the error is not reachable
     * through the interface. Named, not dropped. */
    skip("mprotect", "an unaligned addr is EINVAL",
         "musl rounds addr down to a page boundary, so the syscall's EINVAL is unreachable");

    /* posix_madvise: RETURN VALUE ("zero" or "an error number"), ERRORS
     * ("[EINVAL] The value of advice is invalid"; "[ENOMEM] Addresses ... outside
     * the range allowed for the address space"). The error cases are the
     * standard's, and a no-op fails them. */
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

}  // namespace

int main()
{
    if (!adopt_memory()) {
        aegir::debug_write("  posix-conformance: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  posix-conformance: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    memory_surface();

    printf("AEGIR_POSIX_CONFORMANCE_%s %d/%d (%d skipped, %d holes, %d deviations)\n",
           failed == 0 ? "OK" : "FAIL", passed, passed + failed, skipped, holes, deviations);
    (void)fflush(nullptr);

    /* The boot thread waits for this signal (the same one the greeter and the
     * C++ smoke send), so the marker can follow it. Returning, rather than
     * _exit, lets the runtime's exit bridge run. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return failed;
}
