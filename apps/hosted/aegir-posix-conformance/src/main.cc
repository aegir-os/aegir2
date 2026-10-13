/*
 * aegir-posix-conformance: the POSIX memory-interface conformance suite.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Derived from The Open Group Base Specifications Issue 8 (IEEE Std
 * 1003.1-2024), fetched by `make deps` into third_party/posix-spec/issue8:
 * mmap.html, munmap.html, mprotect.html, posix_madvise.html. The standard's
 * text is a reference and is not redistributed; what is here is our own
 * assertions, each naming the clause it came from. No part of this program was
 * derived from Aegir's own tests or its implementation.
 *
 * The profile is Aegir's, stated so a test outside it is a named skip and not a
 * silent pass: the dispatcher answers mmap (222), munmap (215), mprotect (226)
 * and madvise (233, advisory only) and does not answer msync (227), so msync is
 * not claimed. posix_madvise is claimed only as an advisory hint whose error
 * cases below are measured against the standard, because a hint that always
 * returns zero is a deviation the suite is supposed to say out loud.
 *
 * Markers, machine-readable for the runner:
 *     AEGIR_POSIX_CONFORMANCE_OK <passed>/<total> [<n> skipped]
 *     AEGIR_POSIX_CONFORMANCE_FAIL <passed>/<total> [<n> skipped]
 *
 * A plain POSIX program: mmap, munmap, mprotect, posix_madvise and nothing
 * else, so the runtime stands it up before main (aegir-crt0's constructor).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kRun = 3 * kPage;

int passed = 0;
int failed = 0;
int skipped = 0;

void check(char const *name, bool ok)
{
    if (ok) {
        ++passed;
        printf("posix: ok   %s\n", name);
    } else {
        ++failed;
        printf("posix: FAIL %s\n", name);
    }
    (void)fflush(nullptr);
}

void skip(char const *name, char const *reason)
{
    ++skipped;
    printf("posix: SKIP %s (%s)\n", name, reason);
    (void)fflush(nullptr);
}

}  // namespace

int main()
{
    /* mmap: DESCRIPTION ("shall establish a mapping"), RETURN VALUE (MAP_FAILED
     * on failure), ERRORS ("[EINVAL] The value of len is zero"; "[EINVAL] The
     * value of flags is invalid (neither MAP_PRIVATE nor MAP_SHARED is set)"). */
    errno = 0;
    void *const zero = mmap(nullptr, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap: len zero is EINVAL", zero == MAP_FAILED && errno == EINVAL);

    errno = 0;
    void *const bad_flags = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS /* neither MAP_PRIVATE nor MAP_SHARED */, -1, 0);
    check("mmap: neither MAP_PRIVATE nor MAP_SHARED is EINVAL",
          bad_flags == MAP_FAILED && errno == EINVAL);

    char *const anon = static_cast<char *>(
        mmap(nullptr, kRun, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check("mmap: a run is mapped", anon != MAP_FAILED);
    if (anon == MAP_FAILED) {
        printf("AEGIR_POSIX_CONFORMANCE_FAIL %d/%d\n", passed, passed + failed);
        (void)fflush(nullptr);
        _exit(1);
    }
    check("mmap: a fresh anonymous run is zero-filled", anon[0] == 0 && anon[kRun - 1] == 0);
    anon[0] = 'a';
    anon[kRun - 1] = 'z';
    check("mmap: the run reads its bytes back", anon[0] == 'a' && anon[kRun - 1] == 'z');

    /* mprotect: RETURN VALUE (zero on success), ERRORS ("[ENOMEM] Addresses ...
     * specify one or more pages which are not mapped"). A non-null address the
     * process was never given is the case, not nullptr. */
    errno = 0;
    check("mprotect: an unmapped range is ENOMEM",
          mprotect(reinterpret_cast<void *>(kPage), kPage, PROT_READ) == -1 && errno == ENOMEM);
    check("mprotect: a mapped page can be made read-only", mprotect(anon, kPage, PROT_READ) == 0);
    check("mprotect: a read-only page still reads", anon[0] == 'a');
    check("mprotect: it can be made writable again",
          mprotect(anon, kPage, PROT_READ | PROT_WRITE) == 0);

    /* munmap: RETURN VALUE (zero on success), ERRORS ("[EINVAL] The len
     * argument is 0"); and a mapping that follows lands on memory that is there
     * and zero-filled, which is what a release owes a churning allocator. */
    errno = 0;
    check("munmap: len zero is EINVAL", munmap(anon, 0) == -1 && errno == EINVAL);
    check("munmap: a run is released", munmap(anon, kRun) == 0);
    char *const again = static_cast<char *>(
        mmap(nullptr, kRun, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check("munmap: a mapping after the release succeeds", again != MAP_FAILED);
    if (again != MAP_FAILED) {
        check("munmap: the mapping after the release is zero-filled",
              again[0] == 0 && again[kRun - 1] == 0);
        (void)munmap(again, kRun);
    }

    /* posix_madvise: RETURN VALUE ("zero" or "an error number"), ERRORS
     * ("[EINVAL] The value of advice is invalid"; "[ENOMEM] Addresses ... partly
     * or completely outside the range allowed for the address space"). The
     * first is a hint and may pass trivially; the two error cases are the
     * standard's, and a no-op fails them. */
    char *const adv = static_cast<char *>(
        mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (adv == MAP_FAILED) {
        skip("posix_madvise", "no mapping to advise");
    } else {
        check("posix_madvise: NORMAL returns zero", posix_madvise(adv, kPage, POSIX_MADV_NORMAL) == 0);
        check("posix_madvise: an invalid advice is EINVAL", posix_madvise(adv, kPage, 0x7fff) == EINVAL);
        check("posix_madvise: an outside range is ENOMEM",
              posix_madvise(reinterpret_cast<void *>(kPage), kPage, POSIX_MADV_NORMAL) == ENOMEM);
        (void)munmap(adv, kPage);
    }

    /* msync is not in the profile: the dispatcher answers no such syscall. It is
     * a named skip, so a reader sees the gap rather than an absent test. */
    skip("msync", "not in Aegir's profile: no msync syscall");

    printf("AEGIR_POSIX_CONFORMANCE_%s %d/%d", failed == 0 ? "OK" : "FAIL", passed,
           passed + failed);
    if (skipped != 0) {
        printf(" (%d skipped)", skipped);
    }
    printf("\n");
    (void)fflush(nullptr);
    _exit(failed == 0 ? 0 : 1);
}
