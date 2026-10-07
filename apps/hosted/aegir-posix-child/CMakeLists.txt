#
# aegir-posix-child: a child for the POSIX process acceptance (specs/posix.md).
#
# The plainest hosted program: it stands the runtime up and returns from `main`,
# so its runtime reports the exit its parent's `wait4` completes on. It is its
# own program, with its own name, because the launcher announces every started
# command as `command started <argv[0]>` and the acceptance fires a step on that
# cue (scripts/run_target.py): a second command sharing a cue fires the step on
# the first of them. A child named `date` did exactly that to the acceptance's
# own `date` step.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-posix-child CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-posix-child src/main.cc)

  target_compile_options(aegir-posix-child PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-child PRIVATE aegir-cxx-policy-hosted aegir-cxxabi-shim aegir-command
                              aegir-heap aegir-runtime aegir-bootstrap aegir-mem sel4
                              sel4runtime
  )
else()
  add_executable(aegir-posix-child src/placeholder.cc)

  target_compile_options(aegir-posix-child PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-child PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                              sel4runtime
  )
endif()
