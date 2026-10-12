# Project rules

- If, for whatever reason, something the user has asked for, or the way they've asked for it, 
  isn't working, you **STOP** and ask for clarification. You do not unilaterally change the 
  plan under any circumstances. This is the quickest way to get ditched as a model.

- No arbitrary and hardcoded limits without explicit permission. Capacity tables should
  grow on demand.
- There is no such thing as a "flake" - a broken run is a broken run. We must find out
  why. It succeeding on a subsequent re-run is unacceptable. A failure is a failure.
- No seL4 errors / warnings should be present. When seL4 logs something, that means we're
  doing something wrong. Fix it.
- Any use of `make test` or other long running bash process must use `timeout` so as to
  detect any 'wedged' process. But `timeout` signals only the process it started, so
  killing a wrapper can leave its children running with nobody to stop them: a
  timed-out `make run` left 47 QEMU machines going for hours before anyone noticed.
  So a timed-out or killed run is not finished until you have looked for what it left
  behind (`ps -eo args | grep '[q]emu-system'`, and the like), and a wrapper that takes
  its own children down on the way out is worth having -- scripts/run_target.py does,
  and a signal that arrives as an ordinary exit is what makes that work.
- Use the git repository for history.
- Prefer the mechanism over the reminder. Two of the rules in this file are mechanisms --
  `pipefail`, and a build whose fallback is the revert -- and both held every time they were
  used. Three others asked for care in prose: edit by unique content, read the exact lines
  first, land the smallest piece. They were correct, and the same class of mistake still
  happened seven times *after* they were written. So when a mistake repeats, change the tool
  rather than write it down harder. For edits the mechanism is the edit tool: it requires its
  anchor to occur exactly once and refuses otherwise -- and it refuses to touch a file that
  has not been read in the session -- so a silent no-op is impossible and a multi-line
  pattern is not expressible. A script doing `str.replace`, `re.sub` or a line-number slice
  is the failure mode, not the tool: `replace` returns its input unchanged when nothing
  matches, and a slice lands wherever the numbers point -- across a brace, inside a block, or
  over a neighbour's declaration. Every one of those seven reverts was one of those three.
  Where a script is genuinely needed (a rename across many files), assert each replacement
  before writing and read the region back.
- Make the revert the fallback in the same command as the build --
  `make build || git checkout -- <the directory>` -- and chain the build with `&&`, never with
  `;`. Both are the same idea from two sides: one makes a failure leave the tree standing, the
  other makes it stop the work. A check that cannot fail the command is not a check --
  `make build ; make run ; git commit` committed a `-Werror` build error and a message claiming
  a verification that had not happened, because the `;` let the failure through. That guard and
  this chain each earned their place in one afternoon: seven reverts that never touched the
  repository, and one commit that did.
- Land the small change you are sure of rather than the whole change you are not: the build
  is the checkpoint, so each piece wants its own. A round that reverts has produced nothing a
  reader or the next round can use; a round that commits one certain piece has produced
  something.
- Read the *established implementation* before designing, not only the manual. The manual
  says what is legal; `projects/seL4_libs` and `projects/sel4test` say how it is done, and
  they have already been broken by other people in the places where the shape matters.
  Designing our own shape first means rediscovering, one kernel error at a time, the
  constraints theirs already respects. Two of those, from `sel4utils/src/process.c`
  (`next_free_slot`, `sel4utils_mint_cap_to_process`), worth stating on their own:
  - **every capability installed into a CSpace is named by a `cspacepath` -- root, slot
    and depth -- that was handed out for it.** Never derive one object's slot by adding to
    another's: `device_frame + i` is adjacency reasoning, and adjacency is something that
    happens to hold, not something that is guaranteed.
  - **a slot cursor advances only when the install succeeds.** A cursor that moves on
    failure leaves it behind the slots actually in use, and the next allocation lands on
    top of one (`seL4_DeleteFirst`, "the destination slot is occupied").
- seL4's documentation is in this repository, and it comes before inference. For any
  question about how the kernel behaves, read the manual (`kernel/manual/parts/*.tex`
  -- `vspace.tex` for mappings and page sharing, `objects.tex` for untypeds and the
  device-frame restrictions, `ipc.tex`, `threads.tex`, `io.tex`), the implementation
  under `kernel/src/`, the generated API stubs in
  `out/<target>/libsel4/include/interfaces/` (each listing the errors its call can
  return), and the upstream users in `projects/seL4_libs/` and `projects/sel4test/`
  as worked examples. Quoting the passage (file:line) is the answer to a question the
  docs answer; measuring is the answer to a question they do not; guessing is not an
  answer.
- Decided on specifications belong in the `specs/` folder off the root of the repo.
- Code should be modular - do not create overly large single files. Use proper naming
  and separation of concerns into separate source files.
- All compiler warnings must be fixed. Disabling the warning is not acceptable.
- Commit after every milestone / phase is finished.
- Edit from the file, not from memory. Read the exact lines before changing them, change
  one file per step when the change is coupled, and rebuild between steps. A format
  change that spans a writer and its readers is one change to the same commit, not two.
  Assertions on an edit that did not apply are cheap; three failed anchors in one batch
  cost a round each, and every one of them was quoting a line from memory. Two further
  shapes of the same mistake cost another round each, and the anchor check cannot catch
  either, because in both the anchor *matched*:
  - **A removal's replacement is a copy, never a composition.** Deleting lines means
    naming the block and writing back the lines around it *as the file has them*, copied
    out of a read of them. A line typed because it "should be there" is an invented line.
    Four landed this arc -- `boot_kit.stream = boot_terminal_untyped;`,
    `launcher_kit.stream = launch_port;`, `kit_.stream = stream_endpoint_;`, and a stray
    `bool busy_now = false;` -- each put in place of a line being removed, each plausible,
    and the build caught only the first two. If the replacement is not a copy of text
    just read, it is wrong; when the change is a pure deletion, the replacement is the
    surrounding lines and nothing else.
  - **One kind of call per step.** Batched calls run concurrently, so a `grep` sent
    beside the edits it is meant to verify reads the file as it was *before* them -- it
    reported a landed fix as missing twice this arc -- and two edits to one file in one
    message can interleave. Read and grep in their own step, before or after; edit in
    their own step, and one file at a time when a file takes more than one edit.
- Architecture specific code must be abstracted to ensure we properly support multiple
  architectures (like riscv64, aarch64, x86-64, etc).
- Any third party dependencies will be vendored - we will not commit third party sources
  to this project repo. Pin the dependency (use the SHA hash!) and extract upon demand.
  We can maintain a patches/ folder for any changes needed to the third_party dependency.
- **A `QmpStep`'s `trigger` must be unique within its target.** The runner fires every step
  whose regex matches a console line (`scripts/run_target.py:497-501`), so two steps that
  share a cue both fire on the *first* occurrence, in list order. `times` caps how often one
  step fires; it does not order the steps or reserve the second occurrence for the second
  step. So two steps with one `trigger` are a race, and the later step's action lands on the
  first cue's line -- a latent flake that survives only while both actions happen to be
  harmless together. A cue that genuinely repeats (once per session, say) is *one* step with
  `times=0`, or a distinct cue per occurrence: when a new step wants an existing cue, the
  writer must print a distinguishing word for it, not reuse the string.
- **A refusal must name what ran out, with the numbers.** Nothing in this system says "no"
  in words. The kernel drops a capability destined for an occupied receive slot *silently*
  (`kernel/src/object/cnode.c:895-928`), a service that cannot carve answers with no words at
  all, and one `std::bad_alloc` at the link turned out to be four different shortages wearing
  one message: a spent untyped, a spent node table, a CSpace with no free slot, and a growth
  path with no piece to retype. Days went into that, because the marker said `page-no-slot`
  and nothing else. So a refusal writes the quantity that tells them apart -- `slots 4071 of
  4071, largest free 20 bits, l1 8, l2 12`, `largest free 20 bits with 691 pieces listed and
  1170 nodes in use` -- the way `aegir-heap`'s phase markers already do. A marker that cannot
  say which budget ran out *is* the bug; the diagnostic is part of the fix, not an extra.
- **A process's shape is read from its block, never decided by the program.** Eleven
  stand-ups adopted a two-level CSpace as a flat range, so their retype destinations landed
  in root slots while their slots were the L2 CNode's, and `DataTypes:ilbm.datatype` invoked
  four null caps because of it. One entry point that takes the block's numbers and is correct
  either way (`Allocator::adopt_slots_level_two` treats `l1 == 0` as the single-level shape)
  is what stops the twelfth: a per-program `if (l1 != 0)` is how eleven copies happened, and
  `bootstrap::cnode_l1_bits`/`cnode_bits`/`endpoint_depth` exist so that nobody has to decide.
- **A path that allocates in its own caller is re-entrancy guarded.** `grow_from_source`
  called the registered untyped source, the source took a slot *in the caller's process*, and
  that slot's allocation grew the CSpace, which asked the source again: 120 asks, 28
  receipts, the machine looping on memory it was being handed and never using. The node pool
  had carried the guard (`Allocator::growing_`) since it was written; the untyped source had
  not (`growing_`'s sibling `sourcing_` is now there too). Every "ask for more X" path carries
  the same flag, because the caller of a source is the one that allocates.
- **A release returns its slots as well as its memory.** mallocng churns `mmap`/`munmap`;
  every mapping takes a slot per frame and page table, and a release gives back the memory
  and none of the slots -- so a CSpace sized at spawn fills up no matter how well it grows,
  which is the wall `ld.lld` hits at the link whatever the two-level switch does. The
  mechanism already exists (`SlotPool`, `adopt_slot_pool`, `slot_release`); what is missing
  is a mapping that records the slots it took and frees them on release
  (`specs/userland.md`, "a used slot is still allocation-only").
- **The runner's report must be unmistakable in the log.** Its missing-cue list occupies the
  last ~115 lines of the console file, unlabelled, after the guest's own output -- so it reads
  as guest output, and it was read that way: `AEGIR-ACCEPTANCE-LINKED` "printed" three times
  when the guest had never printed it, which cost an hour of the wrong investigation. The
  runner labels every listed line (`missing: <cue>`) and writes a machine-readable verdict
  beside the build (`verdict=FAIL cause=cue-timeout missing=29 last_cue=...`), so neither a
  person nor an agent has to parse interleaved console text to learn whether a step happened.
- **A run must say why it ended, and killing a guest is a tool, not a command line.** "The run
  stopped at `bureau: screen shortcut`" was ambiguous three ways -- guest idle, guest blocked,
  or guest killed by hand -- and once it was killed by hand: `pkill -f qemu-system` matches the
  shell running it, because the pattern is in that command line, and it took a live guest
  mid-link whose truncated log was then read as data. So the harness records the ending
  (`cause=` above) and `make qemu-kill` matches exactly, refuses while a run is live, and
  prints what it killed; nobody has to remember a bracket.
- **When a symptom returns, say which wall it is before re-fixing it.** "I could have sworn
  this was fixed" was asked twice, fairly, and both times the same symptom had a different
  cause behind it (`bad_alloc` at the link: first a CSpace sized from the image, later the
  source re-entrancy plus a CSpace filling with mapping slots). The numbers of the first rule
  are what make that one line instead of one night; the second half is to state the wall in
  the commit that fixes it, so a returning symptom is not re-litigated from scratch.
