#
# aegir-font: the font service (specs/fonts.md).
#
# It owns the faces Sys:Fonts holds and one FreeType, and serves font.main:
# a client opens a face by its own family and style, reads its metrics, and
# asks for the glyphs it draws -- rasterized into a page the client owns
# (specs/fonts.md). Faces are found by scanning the tree, not by a path, so a
# face dropped anywhere under it is served without a rebuild.
#
# It runs the hosted runtime but never the toolkit: it draws nothing. The
# hosted runtime is where the VFS and libc++ come from, and FreeType is the
# one vendored library linked into it (libs/aegir-freetype/imported.cmake).
# It is packed into director's initrd under the flat name `aegir-font`, which
# is how the manifest resolves it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-font CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-font src/main.cc src/probe.cc)

  target_compile_options(aegir-font PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-font PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap aegir-runtime
                       aegir-bootstrap aegir-ipc aegir-log aegir-font-protocol aegir-namespace
                       aegir-mem freetype sel4 sel4runtime
  )
else()
  add_executable(aegir-font src/placeholder.cc)

  target_compile_options(aegir-font PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-font PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc sel4runtime
  )
endif()

set_target_properties(aegir-font PROPERTIES OUTPUT_NAME aegir-font)
