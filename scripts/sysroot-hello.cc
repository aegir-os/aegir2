/*
 * The program the on-device acceptance builds (specs/development.md,
 * specs/clang-on-aegir.md's Phase 3).
 *
 * A *hosted* C++ Aegir program, which is what makes it a fair test: main is a
 * plain main, and the runtime's crt stands the process up before it -- so what
 * the device compiles here is exactly the shape every program in this tree has.
 *
 * It reaches the sysroot the way any program does: musl's <unistd.h>, libc++'s
 * <string> and <vector>, and the Trinket toolkit's <aegir/trinket/unicode.h>.
 * The toolkit call is deliberately a *defined* symbol rather than a header-only
 * type, so the link pulls a member of libaegir-trinket.a and the toolkit archive
 * is proven linked, not merely included (specs/development.md).
 *
 * What it says is the acceptance's marker: the run reads it off the console, so
 * a program the device compiled, linked and spawned is what printed it.
 */

#include <bits/stdint.h>
#include <aegir/trinket/unicode.h>

#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

int main()
{
    std::string const marker = "AEGIR_HELLO_OK\n";
    std::vector<std::string_view> const pieces{"AEGIR_", "HELLO_OK"};

    if (pieces.size() != 2 || !aegir::trinket::is_valid_utf8(marker)) {
        return 1;
    }

    long const written = write(1, marker.data(), marker.size());
    return written == static_cast<long>(marker.size()) ? 0 : 1;
}
