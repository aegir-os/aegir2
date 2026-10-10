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
