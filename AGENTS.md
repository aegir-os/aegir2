# Project rules

- No arbitrary and hardcoded limits without explicit permission. Capacity tables should
  grow on demand.
- Any use of `make test` or other long running bash process must use `timeout` so as to
  detect any 'wedged' process.
- Use the git repository for history.
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
- Architecture specific code must be abstracted to ensure we properly support multiple
  architectures (like riscv64, aarch64, x86-64, etc).
- Any third party dependencies will be vendored - we will not commit third party sources
  to this project repo. Pin the dependency (use the SHA hash!) and extract upon demand.
  We can maintain a patches/ folder for any changes needed to the third_party dependency.
