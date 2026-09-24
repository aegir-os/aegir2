#
# aegir-trinket: the Aegir GUI toolkit.
#
# Object-oriented, DPI-independent, internationalized widget toolkit
# inspired by Amiga Workbench/MUI with XEN look-and-feel.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-trinket CXX)

# Core library. locale.cc reads the .locale blobs generated from the pinned
# CLDR (scripts/gen_locale_data.py, below); full musl provides the C locale
# surface it uses. bidi.cc is the real UAX #9, with tables generated from the
# pinned Unicode Character Database. translation.cc is the gettext .mo parser
# (specs/locale.md).
# The font is embedded: projects/terminus-font is a pinned vendored project
# (manifests/aegir.xml) and the VFS's file syscalls are stubbed, so the BDF is
# compiled into the library by scripts/embed_binary.py rather than read at
# runtime (specs/trinket.md).
set(AEGIR_UCD_DIR "${CMAKE_SOURCE_DIR}/projects/ucd/unicode-16.0.0")
set(AEGIR_BIDI_TABLES "${CMAKE_CURRENT_BINARY_DIR}/bidi_tables.cc")
add_custom_command(
  OUTPUT "${AEGIR_BIDI_TABLES}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_bidi_tables.py"
          "${AEGIR_UCD_DIR}" "${AEGIR_BIDI_TABLES}"
  DEPENDS
    "${AEGIR_UCD_DIR}/extracted/DerivedBidiClass.txt"
    "${AEGIR_UCD_DIR}/BidiMirroring.txt"
    "${AEGIR_UCD_DIR}/BidiBrackets.txt"
    "${CMAKE_SOURCE_DIR}/scripts/gen_bidi_tables.py"
  COMMENT "Generating the UAX #9 tables from the pinned UCD"
  VERBATIM
)

# The character widths, from the same pinned UCD: the terminal's grid lays a
# combining mark in no cells and an East Asian wide character in two
# (specs/terminal.md). EastAsianWidth gives the wide ranges, the general
# category the zero-width ones.
set(AEGIR_WIDTH_TABLES "${CMAKE_CURRENT_BINARY_DIR}/width_tables.cc")
add_custom_command(
  OUTPUT "${AEGIR_WIDTH_TABLES}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_width_tables.py"
          "${AEGIR_UCD_DIR}" "${AEGIR_WIDTH_TABLES}"
  DEPENDS
    "${AEGIR_UCD_DIR}/EastAsianWidth.txt"
    "${AEGIR_UCD_DIR}/extracted/DerivedGeneralCategory.txt"
    "${CMAKE_SOURCE_DIR}/scripts/gen_width_tables.py"
  COMMENT "Generating the character-width table from the pinned UCD"
  VERBATIM
)

# The locale data: scripts/gen_locale_data.py compiles the pinned CLDR for the
# locales manifests/locales.toml names into the .locale blobs the toolkit
# embeds (locale_data.cc) and, beside them, a .locale file per locale for
# Locale::load and a future volume (specs/locale.md). The generator reads the
# fetched CLDR tree, so the glob below is what makes the build re-run when the
# pin moves.
file(GLOB_RECURSE AEGIR_CLDR_FILES "${CMAKE_SOURCE_DIR}/projects/cldr/46.0.0/*")
set(AEGIR_LOCALES_TOML "${CMAKE_SOURCE_DIR}/manifests/locales.toml")
set(AEGIR_LOCALE_DATA "${CMAKE_CURRENT_BINARY_DIR}/locale_data.cc")
set(AEGIR_LOCALE_FILES "${CMAKE_CURRENT_BINARY_DIR}/locales")
add_custom_command(
  OUTPUT "${AEGIR_LOCALE_DATA}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_locale_data.py"
          "${CMAKE_SOURCE_DIR}/projects/cldr/46.0.0" "${AEGIR_LOCALES_TOML}"
          "${AEGIR_LOCALE_DATA}" "${AEGIR_LOCALE_FILES}"
  DEPENDS
    "${AEGIR_LOCALES_TOML}"
    "${CMAKE_SOURCE_DIR}/scripts/gen_locale_data.py"
    ${AEGIR_CLDR_FILES}
  COMMENT "Compiling the CLDR data into .locale blobs"
  VERBATIM
)

# The built-in gettext catalogue: scripts/compile_po.py turns the committed .po
# into the embedded .mo image Translation::embedded parses. Reading a catalogue
# from the system is deferred, so the toolkit carries one (specs/locale.md).
set(AEGIR_TRANSLATION_PO "${CMAKE_CURRENT_SOURCE_DIR}/resources/translations/trinket.po")
set(AEGIR_TRANSLATION_DATA "${CMAKE_CURRENT_BINARY_DIR}/translation_data.cc")
add_custom_command(
  OUTPUT "${AEGIR_TRANSLATION_DATA}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/compile_po.py"
          "${AEGIR_TRANSLATION_PO}" "${AEGIR_TRANSLATION_DATA}"
  DEPENDS "${AEGIR_TRANSLATION_PO}" "${CMAKE_SOURCE_DIR}/scripts/compile_po.py"
  COMMENT "Compiling the gettext catalogue into an embedded .mo"
  VERBATIM
)
set(AEGIR_TERMINUS_BDF "${CMAKE_SOURCE_DIR}/projects/terminus-font/ter-u12n.bdf")
set(AEGIR_TERMINUS_EMBED "${CMAKE_CURRENT_BINARY_DIR}/terminus_12_bdf.cc")
add_custom_command(
  OUTPUT "${AEGIR_TERMINUS_EMBED}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/embed_binary.py"
          "${AEGIR_TERMINUS_BDF}" "${AEGIR_TERMINUS_EMBED}" terminus_12_bdf
  DEPENDS "${AEGIR_TERMINUS_BDF}" "${CMAKE_SOURCE_DIR}/scripts/embed_binary.py"
  COMMENT "Embedding Terminus 12"
  VERBATIM
)

add_library(aegir-trinket STATIC
    src/application.cc
    src/canvas.cc
    src/widget.cc
    src/layout.cc
    src/label.cc
    src/button.cc
    src/textbox.cc
    src/panel.cc
    src/menubar.cc
    src/window.cc
    src/font.cc
    src/unicode.cc
    src/locale.cc
    "${AEGIR_LOCALE_DATA}"
    src/bidi.cc
    "${AEGIR_BIDI_TABLES}"
    src/terminal_buffer.cc
    src/terminal_view.cc
    "${AEGIR_WIDTH_TABLES}"
    src/translation.cc
    "${AEGIR_TRANSLATION_DATA}"
    src/theme.cc
    src/theme_xen.cc
    src/worker.cc
    "${AEGIR_TERMINUS_EMBED}"
)

target_include_directories(aegir-trinket PUBLIC include)
# bidi_tables.h is internal and reaches the generated bidi_tables.cc by this
# path.
target_include_directories(aegir-trinket PRIVATE src)

# Dependencies: the hosted policy (which brings libc++ and full musl), the
# console window protocol, input's event words, the log, and the memory the
# application adopts -- aegir-bootstrap for the spawn kit's block, aegir-mem
# for the allocator and the window the slice is mapped through, and aegir-heap
# for the freeing heap the standard library allocates from.
target_link_libraries(aegir-trinket PUBLIC
    aegir-cxx-policy-hosted
    aegir-cxxabi-shim
    aegir-runtime
    aegir-console-protocol
    aegir-input
    aegir-log
    aegir-ipc
    aegir-bootstrap
    aegir-mem
    aegir-heap
    sel4
)

# The toolkit's font resources are vendored under projects/terminus-font and
# projects/noto-fonts; staging them into the build belongs with font loading
# (libs/aegir-trinket/src/font.cc's load_terminus), which is still a stub. This
# directory is not tracked -- git does not store an empty one -- so copy it only
# when it is present, or a fresh checkout fails to configure.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts")
  file(COPY resources/fonts/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/resources/fonts/)
endif()