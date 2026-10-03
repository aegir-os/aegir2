#
# aegir-ilbm-datatype: the ILBM class service (specs/datatypes.md).
#
# A class spawned at runtime under the caller's badge; it serves
# `datatypes.class`, reads the caller's file through the session's namespace,
# and decodes with libs/hosted/aegir-ilbm. The artifact is named for what it
# reads -- `ilbm.datatype`, the name the class search finds in DataTypes:.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-ilbm-datatype CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-ilbm-datatype src/main.cc)

  set_target_properties(aegir-ilbm-datatype PROPERTIES OUTPUT_NAME ilbm.datatype)

  target_compile_options(aegir-ilbm-datatype PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-ilbm-datatype PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-heap
                                aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                                aegir-namespace aegir-mem aegir-datatypes aegir-ilbm
                                sel4 sel4runtime
  )
endif()
