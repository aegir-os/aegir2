#
# aegir-posix-env-test: the environment-and-time sub-arc's client (specs/posix.md).
#
# A plain program in the POSIX sense -- getenv, setenv, unsetenv, environ, uname,
# sysconf, getcwd, chdir, clock_gettime, nanosleep and printf, and no Aegir call
# of its own. It links aegir-crt0, the object library whose constructor stands a
# hosted program up before main, so main opens with what it does rather than how
# it came up. Its evidence is the marker it prints and the exit status behind it,
# which the acceptance's step reads off the terminal (scripts/targets.py).
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-posix-env-test CXX)

if(AEGIR_HOSTED_CXX)
  add_executable(aegir-posix-env-test src/main.cc)

  target_compile_options(aegir-posix-env-test PRIVATE -Wall -Wextra -Werror)

  # aegir-crt0 first, and it is an object library on purpose: a static archive
  # is searched only for symbols something already references, and this program
  # references none of the runtime's -- which is the point of it.
  target_link_libraries(
    aegir-posix-env-test PRIVATE aegir-crt0 aegir-cxx-policy-hosted
                                aegir-cxxabi-shim aegir-posix aegir-command aegir-heap
                                aegir-runtime aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-posix-env-test src/placeholder.cc)

  target_compile_options(aegir-posix-env-test PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-posix-env-test PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4
                                muslc sel4runtime
  )
endif()
