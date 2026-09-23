#
# aegir-vfs-client: Aegir's VFS calls in a freestanding library.
#
# The VFS's namespace -- resolve a path, enumerate the volumes -- and a
# volume's reads, listings and handles, wrapped so a caller asks for a file
# instead of building IPC (specs/vfs.md). Freestanding, because freestanding
# services link it and so does the runtime's dispatcher; the hosted
# aegir::filesystem wrapper sits over it for the std::filesystem error model.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-vfs-client CXX)

add_library(aegir-vfs-client STATIC src/vfs.cc)

target_include_directories(aegir-vfs-client PUBLIC include)
target_link_libraries(aegir-vfs-client PUBLIC aegir-cxx-policy aegir-ipc aegir-namespace aegir-volume sel4 sel4runtime)
