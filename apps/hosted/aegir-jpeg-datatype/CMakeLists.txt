#
# aegir-jpeg-datatype: the JPEG class service (specs/datatypes.md).
#
# A class spawned at runtime under the caller's badge; it serves
# `datatypes.class`, reads the caller's file through the session's namespace,
# and decodes with libs/hosted/aegir-jpeg (the vendored libjpeg-turbo). The
# artifact is named for what it reads -- `jpeg.datatype`, the name the class
# search finds in DataTypes:.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-jpeg-datatype CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-jpeg-datatype src/main.cc)

  set_target_properties(aegir-jpeg-datatype PROPERTIES OUTPUT_NAME jpeg.datatype)

  target_compile_options(aegir-jpeg-datatype PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-jpeg-datatype PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap
                                aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                                aegir-namespace aegir-mem aegir-datatypes aegir-jpeg
                                sel4 sel4runtime
  )
endif()
