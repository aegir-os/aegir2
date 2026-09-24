#
# aegir-session-smoke: the first session, not a root task.
#
# Auth starts one when a login succeeds (specs/auth.md): it logs who it
# is -- the badge is the evidence, and the logger renders it -- resolves
# a name through the namespace, reads what the name points at, and
# signals ready. There is no input path yet, so a session is not
# interactive; this one is the proof the shape works.
#
# It is packed into director's initrd (apps/aegir-director/CMakeLists.txt)
# under the flat name `aegir-session-smoke`, which is how auth's spawner
# addresses it.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-session-smoke CXX)

add_executable(aegir-session-smoke src/main.cc)

target_compile_options(aegir-session-smoke PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-namespace and
# aegir-volume are the map and what the map points at, and aegir-metadata is
# the query the session asks of one of them (specs/vfs.md, specs/bfs.md).
target_link_libraries(
  aegir-session-smoke PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc
                              aegir-log aegir-namespace aegir-volume aegir-metadata sel4
                              muslc sel4runtime
)
