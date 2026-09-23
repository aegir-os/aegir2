#
# The hosted libraries: they link aegir-cxx-policy-hosted, which is the rule.
#
# This directory is added only when AEGIR_HOSTED_CXX is on, and the toolkit's
# own libraries are gated again on AEGIR_TOOLKIT. Nothing here may be linked
# into a freestanding binary: these objects reference the exception
# personality, and the director links no unwinder.
#

add_subdirectory(aegir-heap)
add_subdirectory(aegir-environment)
add_subdirectory(aegir-cxxabi-shim)
add_subdirectory(aegir-filesystem)
if(AEGIR_TOOLKIT)
  add_subdirectory(aegir-trinket)
  add_subdirectory(aegir-bureau)
  add_subdirectory(aegir-datatypes)
  add_subdirectory(aegir-classes)
endif()
