#
# aegir-filesystem: Aegir's filesystem beside std::filesystem.
#
# Enumerating the namespace's volumes, with std::filesystem's error model --
# throwing overloads and error_code overloads -- over the freestanding
# aegir::vfs transport. Hosted because the error model is std::filesystem's,
# not because the transport is (specs/cxx.md step 5).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-filesystem CXX)

add_library(aegir-filesystem STATIC src/filesystem.cc src/volume_source.cc)

target_include_directories(aegir-filesystem PUBLIC include)
target_link_libraries(aegir-filesystem PUBLIC aegir-cxx-policy-hosted aegir-vfs-client)
