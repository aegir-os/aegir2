# datatypes: image formats as class services

Status: decided (2026-10). Depends on `specs/libraries.md` (a resource library
is a service), `specs/vfs.md` and `specs/services.md`.

The Amiga's `datatypes.library` is the system's answer to image formats: a
program does not link a PNG decoder, it asks `datatypes` to make an object for a
file, and a **class** -- `ilbm.datatype`, `png.datatype`, `jpeg.datatype`,
found by name in `DataTypes/` -- does the work. Classes are add-ons: dropping one
into `SYS:Classes/DataTypes/` teaches the whole system a format without
recompiling it. Aegir keeps the shape and the add-on property, and makes each
class a **service** instead of a library loaded into the caller
(`specs/libraries.md`).

This is the first real resource library, and the case that forces the mechanism:
which class serves a file is chosen at run time, from the file, by a process that
must start a component the caller did not name at build time.

## The shape

- **`datatypes` is the manager.** It owns the class registry and the class
  search path and serves one port (`datatypes.main`). It identifies a file's
  format, starts or finds the class provider, and returns its port. It does not
  decode anything itself.
- **Each class is a service.** Its binary lives in `DataTypes/`
  (`png.datatype`), is spawned by `datatypes` on first use, under the spawn
  right `spawns = DataTypes/*`, and declares the protocol version it speaks. The
  manager supervisises it (it holds the fault endpoint it made) and owns its
  memory, so a class outlives the session that first opened it
  (`specs/libraries.md`, "the manager owns the provider").
- **The decoded bitmap lands in a client frame.** The client owns the pixel
  memory and hands the class the frames; the class maps them through its window
  and writes the pixels, and unmaps when done. No pixel data crosses IPC as
  words. This is the console's slice and the gpu window's frames
  (`specs/console.md`), and the font service's glyph rasterized "into the
  caller's own page" is the same idea (`specs/fonts.md`).
- **The class reads the file itself, through `vfs.namespace`,** as `aegir-font`
  reads a face directly from the volume (`specs/fonts.md`). The caller names a
  path; the file's bytes do not pass through the caller's address space to reach
  the decoder.

## The client API

The client half is a static library, `aegir-datatypes` (namespace
`aegir::datatypes`); it is `ServerFont`'s shape -- a port wrapped in a value
type. The Amiga's calls keep their names, adapted to C++:

    namespace aegir::datatypes {

    enum class Format { INDEXED, RGB, RGBA, GREY, ... };

    struct Info {
        unsigned width, height, depth;
        Format format;
        /* palette words when INDEXED; alpha flag; the row stride the class
         * will use, so the client sizes its frame to the class's layout and
         * not a guess. */
    };

    class Object {
    public:
        Info info() const;                 // size and format, before decode
        bool read(Bitmap &into);           // decode into the caller's frames
        bool dispose();
    };

    /* The class is chosen from the file's content; the second form forces one
     * by name (the Amiga's NewObject with a class name). */
    Object open(std::string_view path);
    Object open(std::string_view class_name, std::string_view path);
    }

`info()` first, so the client can allocate exactly the frame the class asked for
-- the class states the layout, the client provides it. `read` is one call that
fills that frame. Encode (`write`) is a later method, and method numbers make it
additive (`specs/services.md`).

## Classes, identification, and the registry

- **A class is named for what it reads**, lowercase, one file per format:
  `ilbm.datatype`, `png.datatype`, `jpeg.datatype` (`specs/dos.md`'s naming).
- **`DataTypes:` is an alias to `Sys:DataTypes`**, per badge, read as a union
  with the caller's `Home:DataTypes` appended (`specs/namespace.md`), searched
  current-directory-first exactly as `LIBS:` is (`specs/libraries.md`). A class
  dropped into a session's own directory is found without touching the system
  volume -- the add-on property, and the same property `specs/fonts.md` gives a
  face in `Sys:Fonts`.
- **`DataTypes/classes.registry` maps a format to a class**: the file extension
  and a magic prefix, the `drivers.registry` data shape (`specs/services.md`).
  The manager checks the magic first -- content over name -- and falls back to
  the extension. A class may still refuse a file it was chosen for; the manager
  then reports "no class", loudly, rather than showing a wrong picture.
- **A class declares its protocol version**, so `open`'s version check
  (`specs/libraries.md`) can refuse a class older than the caller needs.

## The object model

The Amiga's `DoMethod(obj, DTM_*)` becomes the port's methods, hidden by the
client library. The first cut:

| method | words | reply |
| --- | --- | --- |
| `identify` | a path | the format, or nothing |
| `info` | a path | the `Info` row (size, format, stride) |
| `read` | a path, the client's frames | success, or the refusal |
| `dispose` | -- | 1 |

The frames arrive in the `read` call as capabilities (one per pixel page),
minted for the class; the class maps them read-write through its window, writes,
unmaps and discards them. A refusal leaves the client's frame untouched.

## The first classes

1. **ILBM** (`ilbm.datatype`) -- IFF InterLeaved BitMap, the Amiga's own picture
   format, no third-party tree. The compatibility case, and the proof of the
   mechanism before a codec drags in a build.
2. **PNG** (`png.datatype`) -- through libpng, vendored and pinned
   (`specs/third_party.md`).
3. **JPEG** (`jpeg.datatype`) -- through libjpeg-turbo.

The two codecs are the reason the service shape is right: each is hundreds of
kilobytes of code wanted by the file requester, the icon library and any future
viewer at once, and each lives in one process rather than in each caller.

## Authority

- `datatypes` holds `spawns = DataTypes/*`, a memory pool, and
  `vfs.namespace`.
- A class is spawned with `vfs.namespace` (to read the file), a pool, and the
  ability to map the frames a caller grants. It gets no other caller port.
- A caller's own `vfs.namespace` is what names the file; the class resolves the
  path on the *caller's* badge, so a file the caller may not read is a file the
  class cannot decode for it.

## What this is not

- **A drawing library.** `DTM_Draw` into a window is the GUI's, not this arc's;
  a viewer is a client that decodes and draws.
- **Encode and animation.** Write and multi-frame are later methods.
- **A general plugin ABI.** The class protocol is datatypes'; a second domain
  gets its own manager and its own protocol (`specs/libraries.md`).

## Phases and acceptance

- **Phase 1 -- the broker, one class, the client.** `datatypes`, `ilbm.datatype`
  and `aegir-datatypes`, proved by the GUI demo opening an IFF ILBM from `Sys:`
  and showing the decoded frame. The pixel checks read the frame the class
  wrote, so a decode that lands at the wrong stride or depth is caught as a
  smudge rather than passing on the call returning.
- **Phase 2 -- PNG**, through the vendored libpng; a second class proves the
  registry, the identification and the resident-provider count.
- **Phase 3 -- JPEG**, libjpeg-turbo.
- **Add-on acceptance** (with Phase 2): a class binary placed in a session's
  `Home:DataTypes` is found and used without rebuilding anything -- the property
  the whole shape exists for.

## Open, for review

- **Whether the class decodes from a path or from bytes the caller already
  holds.** The first cut is a path and `vfs.namespace`, matching `aegir-font`.
  A caller with bytes in memory (a network stream, an archive member) wants the
  other, and it is a second method, not a different model.
- **The `Info` stride and format vocabulary** -- how indexed palettes, alpha and
  planar Amiga bitmaps are named, since ILBM is planar and PNG is chunky, and
  the client must be able to render both.
