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

- **A class is a user resource library** (`specs/libraries.md`). It reads the
  caller's file, and its code may be the caller's own from `Home:DataTypes`, so
  it runs with the **caller's authority** -- a child of the session's launching
  machinery, reclaimed with the session, never spawned by a system service. Its
  binary lives in `DataTypes/` (`png.datatype`) and is found by name on the
  caller's search path.
- **A class is started under the caller's badge, not the system's.** The
  session's launcher (`specs/launch.md`) starts it and the caller talks to the
  port the launch produced. Identification -- which class reads this file --
  runs candidate classes, so it too happens under the caller's authority; there
  is no system `datatypes` service that reads the user's file on the user's
  behalf. The first cut is the calling program (a launching program such as the
  Bureau or the demo) starting the class through the launcher it already holds;
  a per-session broker any of the session's processes may ask is the
  generalization.
- **The class decodes once and serves the frame a page at a time.** A message
  carries one capability (`specs/launch.md`, `specs/signal.md`), so an image
  larger than a page cannot cross in one call. The class decodes into its own
  memory -- the object -- and each `read` carries one page the caller owns: the
  class maps it, copies that slice of the frame into it and unmaps before
  answering. The frame is the pixels then the palette, and `Info` says how long
  each is, so the caller knows when the image is out. No pixel data crosses IPC
  as words. This is the font service's transfer page (`specs/fonts.md`),
  repeated until the image is out.
- **The class reads the file itself, through `vfs.namespace`,** as `aegir-font`
  reads a face directly from the volume (`specs/fonts.md`). The caller names a
  path and hands the class its own namespace capability at `open`, so the path
  resolves on the caller's badge; the file's bytes do not pass through the
  caller's address space to reach the decoder.

## The client API

The client half is a static library, `aegir-datatypes` (namespace
`aegir::datatypes`); it is `ServerFont`'s shape -- a port wrapped in a value
type. The Amiga's calls keep their names, adapted to C++:

    namespace aegir::datatypes {

    enum class Format : uint8_t { INDEXED, RGB, RGBA, GREY };

    struct Color { uint8_t r, g, b; };

    struct Info {
        unsigned width, height;
        Format format;
        unsigned stride;          /* bytes per row the class writes */
        unsigned palette_size;    /* entries when INDEXED */
        bool transparent;
        unsigned transparent_index;
    };

    /* The decoded image: the class's source and the caller's target, so both
     * sides name one shape (aegir/datatype/decoded.h). */
    struct Decoded {
        Info info;
        std::vector<uint8_t> pixels;
        std::vector<Color> palette;
    };

    class Object {
    public:
        Info info() const;                /* size, layout and palette */
        bool read(Decoded &into);         /* pull the frame, a page at a time */
        bool dispose_object();
    };

    /* The class is chosen from the file's content; the second form forces one
     * by name (the Amiga's NewObject with a class name). */
    Object new_object(std::string_view path);
    Object new_object(std::string_view class_name, std::string_view path);
    }

`info()` first, so the caller knows the size and layout; `read` loops the
frame's pages into `into`. A class's `Decoded` is its buffer and the caller's is
its own -- the bytes cross a page at a time. Encode (`write`) is a later method,
and method numbers make it additive (`specs/services.md`).

## Classes, identification, and the class list

- **A class is named for what it reads**, lowercase, one file per format:
  `ilbm.datatype`, `png.datatype`, `jpeg.datatype` (`specs/dos.md`'s naming).
- **`DataTypes:` is an alias to `Sys:DataTypes`**, per badge, read as a union
  with the caller's `Home:DataTypes` appended (`specs/namespace.md`). The class
  search is the caller's **program directory** first, then `DataTypes:`, the same
  order and the same reason as `LIBS:` (`specs/libraries.md`): a class shipped
  beside a program's binary is found whatever the current directory is. A class
  dropped into a session's own `Home:DataTypes` is found without touching the
  system volume -- the add-on property, and the same property `specs/fonts.md`
  gives a face in `Sys:Fonts`. A broker cannot see another process's program
  directory, so the caller's `new_object` request carries it
  (`specs/environment.md`).
- **The class list is the directory.** The broker lists `DataTypes:` at run
  time; a class is a file in it, so adding one is dropping it in -- no registry
  to update and no second list to keep in step, the same reason `LIBS:` is the
  list (`specs/libraries.md`). Identification is content-first: the broker
  starts a class -- under the caller's badge, and it lives for the session -- and
  asks it to `identify` the file, and the class, not a data file, is the
  authority on what it reads. A file extension may choose which class to ask
  first, but the class decides. A file no class claims is reported "no class",
  loudly, rather than shown as a wrong picture.
- **A class declares its protocol version**, so `open_library`'s version check
  (`specs/libraries.md`) can refuse a class older than the caller needs.

## The object model

The Amiga's `DoMethod(obj, DTM_*)` becomes the port's methods, hidden by the
client library. The first cut:

| method | fields | reply |
| --- | --- | --- |
| `identify` | a path | 1 when this class reads the file, else 0 |
| `info` | a path | 1 and the `Info` words |
| `read` | a path, an offset, one page capability | the bytes of the frame filled |
| `dispose_object` | -- | 1 |

The class holds the decoded object between calls, scoped to the caller's badge,
so a picture larger than a page is several `read` calls -- each carrying one
page the caller owns, which the class maps a copy of, writes and unmaps before
answering (`specs/fonts.md`'s transfer page). A refusal leaves the caller's page
untouched, and the count of bytes filled says where the frame ends.

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

- A class runs as the caller: the session's launcher starts it under the
  caller's badge, and it is reclaimed with the session.
- It is given the caller's `vfs.namespace` (to read the file), its own memory
  from the session's pool, and the ability to map the pages a `read` carries. It
  gets no other caller port.
- Because the class resolves the path on the *caller's* badge, a file the caller
  may not read is a file the class cannot decode for it -- and a class the caller
  supplies from `Home:DataTypes` has only the caller's own authority to abuse.

## What this is not

- **A drawing library.** `DTM_Draw` into a window is the GUI's, not this arc's;
  a viewer is a client that decodes and draws.
- **Encode and animation.** Write and multi-frame are later methods.
- **A general plugin ABI.** The class protocol is datatypes'; a second domain
  gets its own class protocol and broker (`specs/libraries.md`).

## Phases and acceptance

- **Phase 1 -- one class, the client, and the launch.** `ilbm.datatype` and the
  `aegir-datatypes` client, the class started under the caller's badge through
  the launcher the calling program holds, proved by the GUI demo opening an IFF
  ILBM from `Sys:` and showing the decoded frame. The pixel checks read the
  frame the client assembled, so a decode at the wrong stride or depth is a
  smudge rather than a passing call.
- **Phase 2 -- PNG**, through the vendored libpng; a second class proves the
  class list and the identification.
- **Phase 3 -- JPEG**, libjpeg-turbo.
- **Add-on acceptance** (with Phase 2): a class binary placed in a session's
  `Home:DataTypes` is found and used without rebuilding anything -- the property
  the whole shape exists for.

## Open, for review

- **The class reads a path and the caller's namespace** (decided above). A
  caller with bytes already in memory -- a network stream, an archive member --
  wants the other, and it is a second method, not a different model.
- **The `Info` vocabulary's edges.** The first cut is `INDEXED`, `RGB`, `RGBA`
  and `GREY` with a stated stride; planar storage stops at the class, so a
  client never sees a bit plane. What is still open is how alpha beyond ILBM's
  one transparent colour is named, and how a multi-frame image (an animation)
  exposes its frames.
