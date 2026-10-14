/*
 * aegir-posix-memory-test: the memory sub-arc's acceptance client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: mmap, munmap and the calls that set them
 * up -- open, close, memcmp -- and nothing else, so it carries no Aegir call of
 * its own. The runtime stands it up before `main` (aegir-crt0's constructor),
 * and its evidence is a marker it prints itself: `printf` is a libc call, not
 * the runtime's diagnostic writer, and the terminal mirrors a command's output
 * to the serial the acceptance's runner reads.
 *
 * What it proves is the memory half of the surface that
 * `specs/clang-on-aegir.md` calls the load-bearing gap: a *file's* bytes through
 * a mapping (what LLVM's MemoryBuffer does for every input object file), an
 * anonymous mapping written through and read back, `mprotect` changing a page's
 * protection and leaving the mapping usable, `munmap` returning it, a mapping
 * that follows landing on memory that is there and zero-filled, and MAP_FIXED
 * refused rather than quietly landing somewhere else.
 *
 * The frame behind each mapped page is what `mprotect` needs in hand, and the
 * heap keeps that record in chunks it maps as it needs them -- sizing it from the
 * arena at init was tried and cost the launcher the untyped it spawns commands
 * with (measured: "spawn: FAIL no untyped for the command's runtime").
 *
 * The file is the AEGIR volume's own, whose bytes the path view's client also
 * knows (scripts/make_disk.py's AEGIR_BFS_TREE), and it is read through the view
 * like any other path -- so this client measures memory and the path view at
 * once, without either knowing the other exists.
 *
 * Statuses:
 *    0  every check passed (and it says AEGIR_POSIX_MEMORY_OK on stdout as well)
 *  2+n  the n-th check failed (n counts the calls to require(), in source order)
 *
 * The per-check status is what the terminal's own `command exited <n>` line
 * carries to the runner, so a failure names itself there as well as on the
 * marker's channel (specs/posix.md).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr char const *kSource = "/AEGIR/AEGIR.TXT";
char const *const kAegirText = "aegir read this file off a disk it enumerated itself\n";
constexpr size_t kPage = 4096;
constexpr size_t kAnonymous = 3 * kPage;

/* Which check is running, so a failure can say so with a status rather than a
 * message (the header). */
int check = 0;

[[noreturn]] void fail(int status, char const *what)
{
    printf("AEGIR_POSIX_MEMORY_FAIL %s (check %d)\n", what, status - 2);
    (void)fflush(nullptr);
    _exit(status);
}

void require(bool ok, char const *what)
{
    ++check;
    if (!ok) {
        fail(2 + check, what);
    }
}

}  // namespace

int main()
{
    /* A file's bytes through a mapping: the whole of what a compiler's
     * MemoryBuffer asks, and the mapping is private and read-only to the caller,
     * so the volume's own bytes cannot change. */
    int const fd = open(kSource, O_RDONLY);
    require(fd >= 0, "open /AEGIR/AEGIR.TXT");
    size_t const length = strlen(kAegirText);
    void *mapped = mmap(nullptr, kPage, PROT_READ, MAP_PRIVATE, fd, 0);
    require(mapped != MAP_FAILED, "mmap the file");
    require(memcmp(mapped, kAegirText, length) == 0, "the mapping holds the file's bytes");
    require(static_cast<char const *>(mapped)[length] == '\0',
            "the rest of the page reads as zero");
    require(munmap(mapped, kPage) == 0, "munmap the file");
    require(close(fd) == 0, "close the file");

    /* An anonymous mapping, written through and read back: the pages a program
     * gets for its own work are zero-filled, and they are writable. */
    char *arena = static_cast<char *>(
        mmap(nullptr, kAnonymous, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(arena != MAP_FAILED, "mmap anonymous read/write");
    require(arena[0] == 0 && arena[kAnonymous - 1] == 0, "a fresh mapping reads as zero");
    for (size_t i = 0; i < kAnonymous; ++i) {
        arena[i] = static_cast<char>('a' + (i % 26));
    }
    require(arena[kAnonymous - 1] == static_cast<char>('a' + ((kAnonymous - 1) % 26)),
            "a written mapping reads back its own bytes");

    /* mprotect: a mapping's protection can be changed and the mapping stays
     * usable -- read-only here, then writable again. What the rights mean is the
     * hardware's business, so this checks what a program relies on: the pages keep
     * what they held, and the mapping takes writes again once it is writable. */
    require(mprotect(arena, kPage, PROT_READ) == 0, "mprotect a page read-only");
    require(arena[0] == 'a', "a read-only mapping still reads its bytes");
    require(mprotect(arena, kPage, PROT_READ | PROT_WRITE) == 0, "mprotect it writable again");
    arena[0] = 'z';
    require(arena[0] == 'z', "and it takes a write again");

    /* munmap gives it back, and a mapping that follows lands on memory that is
     * there and zero-filled -- which is what a real munmap owes a program that
     * turns over large allocations. */
    require(munmap(arena, kAnonymous) == 0, "munmap the anonymous mapping");
    char *again = static_cast<char *>(
        mmap(nullptr, kAnonymous, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(again != MAP_FAILED, "mmap again after the release");
    require(again[0] == 0 && again[kAnonymous - 1] == 0, "the mapping that follows is zeroed");
    require(munmap(again, kAnonymous) == 0, "munmap the second mapping");

    /* MAP_FIXED is refused, and the refusal is a boundary worth pinning: the
     * arena's free area is the one interval between the break and its cursor, so
     * a hole punched into it could not be handed back, and a live mapping cannot
     * be replaced (specs/posix.md, specs/memory.md). A program that asks for an
     * address it was never given gets MAP_FAILED rather than a mapping that
     * lands somewhere else. */
    void *const refused = mmap(reinterpret_cast<void *>(kPage), kPage, PROT_READ,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    require(refused == MAP_FAILED, "MAP_FIXED at an address we were not given is refused");

    /* A block grown repeatedly with realloc, so it leaves mallocng's small-size
     * path for the mmap-serviced one and grows within it -- the allocator
     * pattern a linker's SmallVector makes, run here without the linker. Bytes
     * written before a growth must survive it: a group whose meta link was lost
     * underneath a live pointer fails this, and the failure names itself in the
     * exit status. */
    unsigned char *grown = nullptr;
    size_t grown_length = 0;
    bool grown_ok = true;
    for (size_t want = 1024; want <= (4u * 1024u * 1024u); want *= 2) {
        unsigned char *const moved = static_cast<unsigned char *>(realloc(grown, want));
        if (moved == nullptr) {
            grown_ok = false;
            break;
        }
        if (grown_length != 0 && (moved[0] != 0xa5 || moved[grown_length - 1] != 0x5a)) {
            grown_ok = false;
        }
        for (size_t i = 0; i < want; ++i) {
            moved[i] = static_cast<unsigned char>(i & 0xff);
        }
        /* Mark the ends so the next growth can prove they survived. */
        moved[0] = 0xa5;
        moved[want - 1] = 0x5a;
        grown = moved;
        grown_length = want;
    }
    require(grown_ok, "a block grown past the mmap threshold keeps its bytes");
    free(grown);

    /* The linker's own pattern, which the growth above does not have: a file
     * mapped in, and a block grown repeatedly with realloc *while the mapping is
     * live* -- one heap serves both, so the growth must not take the mapping's
     * pages and the mapping must not take the growth's. Repeated, because a
     * one-shot interleave can pass by luck of placement; the input object files
     * that a link maps and unmaps are what this stands in for. */
    int const interleave_fd = open(kSource, O_RDONLY);
    require(interleave_fd >= 0, "open the file for the mapping-and-growth interleave");
    bool interleave_ok = true;
    for (int round = 0; interleave_ok && round < 8; ++round) {
        void const *const live = mmap(nullptr, kPage, PROT_READ, MAP_PRIVATE, interleave_fd, 0);
        if (live == MAP_FAILED) {
            interleave_ok = false;
            break;
        }
        unsigned char *block = nullptr;
        size_t block_length = 0;
        for (size_t want = 1024; want <= (2u * 1024u * 1024u); want *= 2) {
            unsigned char *const moved =
                static_cast<unsigned char *>(realloc(block, want));
            if (moved == nullptr || memcmp(live, kAegirText, length) != 0 ||
                (block_length != 0 && (moved[0] != 0x3c || moved[block_length - 1] != 0xc3))) {
                interleave_ok = false;
                break;
            }
            for (size_t i = 0; i < want; ++i) {
                moved[i] = static_cast<unsigned char>(i & 0xff);
            }
            moved[0] = 0x3c;
            moved[want - 1] = 0xc3;
            block = moved;
            block_length = want;
        }
        free(block);
        if (munmap(const_cast<void *>(live), kPage) != 0) {
            interleave_ok = false;
        }
    }
    require(interleave_ok,
            "a block grown under a live file mapping keeps its bytes and the mapping its");
    require(close(interleave_fd) == 0, "close the interleave file");

    /* The linker's *output*: a file mapped PROT_READ|PROT_WRITE, MAP_SHARED, so
     * the writes are the file's and the release is what carries them back --
     * LLVM's FileOutputBuffer writes through such a mapping and its commit is an
     * unmap followed by a rename. Interleaved with the growth, because that is
     * the mixture a link makes: a writable shared mapping is live while the
     * linker's own structures grow. The file is created and read back through
     * the POSIX view, so the bytes proved are a file's on disk, not the
     * mapping's in memory. */
    constexpr char const *kShared = "/SCRATCH/MEMORY.SHARED";
    constexpr size_t kSharedSize = 64u * 1024u;
    int shared_fd = open(kShared, O_RDWR | O_CREAT | O_TRUNC, 0644);
    require(shared_fd >= 0, "create the shared file");
    require(ftruncate(shared_fd, static_cast<off_t>(kSharedSize)) == 0,
            "size the shared file before mapping it");
    char *const shared = static_cast<char *>(
        mmap(nullptr, kSharedSize, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0));
    require(shared != MAP_FAILED, "mmap the file writable and shared");
    unsigned char *under = nullptr;
    size_t under_length = 0;
    bool shared_ok = true;
    for (size_t want = 1024; want <= (2u * 1024u * 1024u); want *= 2) {
        unsigned char *const moved = static_cast<unsigned char *>(realloc(under, want));
        if (moved == nullptr ||
            (under_length != 0 && (moved[0] != 0x5e || moved[under_length - 1] != 0xe5))) {
            shared_ok = false;
            break;
        }
        for (size_t i = 0; i < want; ++i) {
            moved[i] = static_cast<unsigned char>(i & 0xff);
        }
        moved[0] = 0x5e;
        moved[want - 1] = 0xe5;
        under = moved;
        under_length = want;
        /* The writable mapping is still ours to write while the block grows. */
        for (size_t i = 0; i < kSharedSize; ++i) {
            shared[i] = static_cast<char>('A' + (i % 26));
        }
    }
    require(shared_ok, "a block grown while a writable shared mapping is live");
    require(munmap(shared, kSharedSize) == 0, "munmap the shared mapping");
    free(under);
    require(close(shared_fd) == 0, "close the shared file");

    /* The write-back belongs to the unmap: the file on disk must hold what was
     * written through the mapping. */
    shared_fd = open(kShared, O_RDONLY);
    require(shared_fd >= 0, "reopen the shared file after the release");
    char back[26];
    require(pread(shared_fd, back, sizeof(back), 0) == static_cast<ssize_t>(sizeof(back)),
            "read the shared file back");
    bool wrote_back = true;
    for (size_t i = 0; i < sizeof(back); ++i) {
        if (back[i] != static_cast<char>('A' + (i % 26))) {
            wrote_back = false;
        }
    }
    require(wrote_back, "a shared mapping's writes reached the file on the release");
    require(close(shared_fd) == 0, "close after reading the shared file back");
    require(unlink(kShared) == 0, "unlink the shared file");

    printf("AEGIR_POSIX_MEMORY_OK\n");
    (void)fflush(nullptr);
    _exit(0);
}
