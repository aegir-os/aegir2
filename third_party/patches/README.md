# Patches to vendored components

Patches here are applied to the vendored trees by `make deps`
(`scripts/apply_patches.py`), which is idempotent: a patch already present is
skipped, so it is safe to run after every `repo sync`.

## Layout

Directory names mirror the vendored layout. A patch's project is its directory
relative to this one, and must be a project path from `manifests/aegir.xml`:

```
third_party/patches/kernel/0001-example.patch            -> kernel/
third_party/patches/projects/musllibc/0001-example.patch -> projects/musllibc/
```

Files are applied in lexical order, so number them (`0001-`, `0002-`, …).

Anything outside a vendored project path is rejected by
`scripts/apply_patches.py` and by `make deps-check`.

## Format

Plain patches that `git apply` accepts, generated from the pinned revision:

```sh
git -C projects/musllibc format-patch -1 --stdout > \
    third_party/patches/projects/musllibc/0001-<slug>.patch
```

Patch only vendored files — never our own sources, which live in this
repository and need no patch machinery.

## Kernel patches need a written justification

`kernel/` is GPL-2.0-only. Patching it means:

- our patch is a derivative work and must be licensed GPL-2.0-only and
  published,
- the seL4 proofs for that configuration no longer hold,
- per seL4's trademark policy the result may no longer be called "seL4".

So a patch under `third_party/patches/kernel/` requires a recorded
justification in `specs/third_party.md` first. Prefer patching a BSD-2-Clause
component, or solving the problem in Aegir's own code.

## Reviewing a patch

Every patch must be explainable in one line: what breaks without it, and why
the fix cannot live in Aegir's own code. Patches accumulate as a maintenance
cost across seL4 upgrades, so they are a last resort, not a convenience.
