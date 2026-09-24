# Ownership, and the home

The namespace is open until this lands (`specs/vfs.md`): every caller resolves
every name, and the badge chain the VFS mints is built but unchecked. This
spec closes it far enough for a session's home to be the session's -- the
"home volumes" the permission decision waits on (`specs/bfs.md` decision 7).
It is `specs/authority.md`'s model made mechanical: **the capability was never
handed over**, so a user's things are the ones the user can resolve.

## The model

- A **volume has an owner**. A volume a filesystem registers is the system's
  (`register` sets no owner); the system class owns everything it has not
  given away.
- A volume is **public** or private. A public volume is resolvable by every
  caller; the system volume is public (`kFlagBoot` implies it), and any volume
  may be marked so (`kFlagPublic`). A private volume is resolvable by its
  owner and by the system class, and by no one else.
- **resolve** checks the owner. The caller is the system class (badge bit 62
  clear), or the volume is public, or the caller's user index (badge bits
  24..61) equals the owner's, or the answer is a refusal. The check is on the
  volume, before the mint: a caller never receives a capability to a volume it
  may not see.

The gate is resolve's alone -- not a per-call check in each filesystem, which
would hold the namespace's policy in the wrong place and let a caller hold a
capability it may not use. `bind` inherits the gate because it resolves its
member with the binder's badge (`specs/namespace.md`: the ownership model
decides who may bind what), so a user cannot bind an alias standing for a
volume it may not resolve. `count`/`describe` report only the volumes the
caller may resolve, so a private name is not even disclosed.

The badge helpers live with the rest of the badge space
(`aegir/ipc/port.h`): a user is bit 62, its index is bits 24..61, and a
system badge is any badge without bit 62.

## The view (`mount`)

A user's home is a directory on the system volume, but it is also a volume of
its own to resolve. A **view** is a volume that stands for a sub-path of
another: it carries the source's port, a base path, a name, and an owner.

- **mount** — words: the source path, the view's name, the owner badge, flags.
  Answer: the name the view got (a duplicate gains the `_N` suffix, as
  `register` does). The source is resolved for the caller, so an alias works --
  auth mounts over `Sys:Homes/<name>` -- and the path's rest is the view's
  base. Only the system class may mount; auth is the system class. A mount
  whose source the caller may not resolve is refused.
- **mount is idempotent by name**: a mount whose name is already a view with
  the same owner index returns that view rather than making a second. auth
  logs in again and again (`specs/auth.md`'s reclaim proves sixteen), and each
  login must not spend another view.
- Resolving a view prepends its base to the caller's rest. The view is a
  **root**: a parent climb at the top stays at the top, so `Home:/` is the
  home and not `Sys:Homes`. (The bind-mount clamp: a view that could climb out
  of its base would not be the private volume it is named.)
- The view's own name and owner are what the gate checks, so a session's home
  is a private volume the session resolves and another badge cannot. `Home:`
  is a per-badge alias to it (`bind`); the view is the thing that is owned.

## The home at login

auth makes the home a volume the user owns, in this order, after the
credential is accepted and before the spawn (`specs/auth.md`):

1. **ensure** — one `mkdir` of the home path through the namespace, as today.
2. **own** — the directory is set to the user's index and to owner-only:
   `Owner` (uid and gid both the row index) and `Protect` (0700), the BFS
   setters `specs/bfs.md` decision 7 names. Without them the home stays
   root-owned, and a second user reaching it through the public `Sys:` would
   read it -- the view is not enough on its own.
3. **mount** — a view over the home path, named for the user, owned by the
   session's badge.
4. **bind** — `Home:` → the view, replacing any earlier binding.

A home that will not make, own, or mount is logged and the bind is made
anyway, exactly as today: the failure surfaces at the session's first write,
which is where it belongs.

## Two halves, and their order

The home needs the namespace's half and the filesystem's half, and they are
two changes rather than one:

1. **The namespace's half** (this spec, `specs/vfs.md`): the owner, the
   public/private flag, the view and `mount`, the resolve gate, and auth's
   own-mount-bind at login. The proof is that a session resolves `Home:` and
   another badge's resolve of the same view is refused.
2. **The filesystem's half** (`specs/bfs.md` decision 7): `Owner` and
   `Protect`, create-defaults taken from the caller's badge, and the
   per-operation mode check. The proof is that a second badge resolving
   `Sys:Homes/<user>` -- the public path the view is meant to hide -- is
   refused at the directory.

The namespace's half can stand first: until the filesystem's half lands, the
`Sys:` path still reaches the home, and the two commits together close it.

## The test

The test bed grows a second user (the packer runs the source rows, so one more
row is one more login target), and the proof is two-sided:

- the first user's session resolves `Home:`, creates and writes a file there;
- a resolve of the home's own view by the second user's badge is refused;
- the second user reaching `Sys:Homes/<first>` is refused at the directory,
  while the system class reads the same file as today.
