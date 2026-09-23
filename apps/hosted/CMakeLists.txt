#
# The hosted apps. They link aegir-cxx-policy-hosted; the toolkit's own sit
# behind AEGIR_TOOLKIT inside their own CMakeLists.
#
# This directory is added regardless of the flags, because director's manifest
# refuses to boot when a declared binary is absent from its initrd: each app
# here builds a freestanding placeholder when its runtime is off.
#

add_subdirectory(aegir-cxx-smoke)
add_subdirectory(aegir-env-smoke)
add_subdirectory(aegir-greeter)
add_subdirectory(aegir-bureau)
add_subdirectory(aegir-gui-demo)
