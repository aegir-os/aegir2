# limits: resource limits, configuration

Status: decided (2026-09); implemented with the memory service's Phase 4
(specs/memory.md). Aegir's processes are bounded by **the machine** by default:
nothing limits how much of memory a user's programs may use until an operator
says so. This spec is that "says so" -- the configuration, the subjects it
applies to, and the precedence -- so that restricting a user is a deployment
decision, not a number someone guessed when the system was built.

It generalizes: memory is the first resource, because the memory service
(`specs/memory.md`) is where the first chokepoint is, but the shape covers any
resource a service can meter.

## The format is the system's

The file is the **universal configuration format**: the one `services.manifest`
already uses -- `[section]` headers, `key = value` lines, `#` comments, values
read to the end of the line. There is no second syntax to learn and no runtime
TOML parser to grow; a service that reads configuration reads this
(`aegir-descriptor` and `aegir-manifest` are two readers of it, and the limits
reader is a third). A value that needs a space is a value the format cannot
carry yet, and the format will grow one rule, not one parser, when it must.

## The file

`Sys:S/limits.manifest` -- a text file on the system volume, editable without
reimaging, sitting beside the system's startup scripts. It is read at boot and
may be re-read or served by the config service later (`specs/services.md`).

    # Sys:S/limits.manifest -- resource limits, opt-in (specs/limits.md).
    #
    # With no matching rule a user may use as much memory as the pool holds:
    # the limit is the machine. A rule adds visibility (log) or a backstop
    # (deny). A rule is <resource>_<action> = amount, in a subject's section.
    # Subjects are the default, a class, or a user; a user's class comes from
    # the user database (specs/auth.md), and a user: section overrides its class.

    [default]
    # memory_log = 64M        # uncomment to see who uses memory; no cap

    [class.desktop]
    memory_log = 1G
    memory_deny = 4G

    [user.rroland]
    memory_deny = 8G

The shipped file is comments plus the commented-out `[default] memory_log`
example: out of the box it restricts nothing, which is the point.

## Subjects, resources, actions

- **Subjects** are section names. `[default]` is the system default; `[class.<name>]`
  is a class; `[user.<name>]` is one user. A *class* is a named rule set -- the
  FreeBSD login-class idea (`login.conf`), the user's class named in the user
  database, a `user:` section overriding its class. Later: `[session.<name>]`,
  and jail-like subjects.
- **Resources** are the prefix of a key. `memory` means **committed** bytes --
  what a service has actually handed out, not address space -- which is the
  quantity the memory service holds and reclaims. Later: `cputime`, `maxproc`,
  `openfiles`, each metered by the service that owns it.
- **Actions** are the suffix of a key, rctl's (`rctl(8)`): `log` records the
  crossing and lets it through; `deny` refuses the allocation that crosses. The
  first cut is `log` and `deny`; `confirm` (a growth a supervisor must allow),
  `signal` and `throttle` come later. The memory service's `alloc`
  (`specs/memory.md`) is the one enforcement point today.
- **Amounts** are bytes with an optional `K`, `M` or `G` (binary: 1024), read as
  written, unquoted.

## Precedence

For one `(resource, action)`, the **most specific subject wins**:
`user:<name>` then `class:<name>` then `default`. A subject that states no rule
for a pair inherits the next broader one; a pair no subject states is
**unlimited** -- the machine. So a deployment can set one class default and
override a single user without restating the class.

## Membership

A user's class is the user database's (`specs/auth.md`): a `class=` field in the
row, defaulting to `default`. Membership is identity, in the user database; the
rules are policy, in this file -- the FreeBSD split (the password database names
the class, `login.conf`/`rctl.conf` hold the rules). There are no per-user limit
fields in the user database; a one-off override is a `[user.<name>]` section.

## Reading it

The service that enforces a resource reads this file, resolves each user's class
from the user database, and applies the precedence once at boot. For memory
(`specs/memory.md`), the memory service reads `Sys:S/limits.manifest` and
`users.db`, keyed by the user index a badge carries. The file lives on `Sys:`,
which comes up after the VFS; a service that reads it at boot retries until the
volume resolves, the same idiom `auth` uses for `C:` (`specs/auth.md`).

## What this is not

- A kernel policy. Limits are enforced by the service that holds the resource,
  not the kernel; a service that does not consult this file is unlimited.
- A complete policy engine. One resource, two actions and three subjects are the
  first cut; a rule that applies at runtime (the config service), a growth a
  supervisor confirms, and per-session subjects are later.
- A quota system with a default cap. There is no cap unless a rule makes one:
  the default is the machine, as `specs/authority.md` decided.
