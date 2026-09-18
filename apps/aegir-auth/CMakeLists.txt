#
# aegir-auth: the user database and the login port (specs/auth.md).
#
# Reads the packed user table from Initrd: through the namespace, serves
# auth.login from it: a name and a secret in, one word out.
#
# It is packed into director's initrd under the flat name `aegir-auth`,
# which is how it is addressed now.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-auth CXX)

add_executable(aegir-auth src/main.cc)

target_compile_options(aegir-auth PRIVATE -Wall -Wextra -Werror)

# aegir-cxx-policy carries the freestanding C++ flags; aegir-runtime provides
# the pieces every binary needs (see libs/aegir-runtime). aegir-authdb is the
# two formats it speaks; aegir-namespace and aegir-volume are how the
# database reaches it (specs/auth.md). aegir-mem is how it adopts the spawn
# kit, and aegir-spawn is how a login becomes a session (specs/auth.md,
# specs/authority.md).
target_link_libraries(
  aegir-auth PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap aegir-ipc aegir-log
                     aegir-authdb aegir-console-protocol aegir-mem aegir-namespace
                     aegir-spawn aegir-volume sel4 muslc sel4runtime
)
