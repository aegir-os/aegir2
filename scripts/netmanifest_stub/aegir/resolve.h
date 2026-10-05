/*
 * A stub of aegir/resolve.h for the netmanifest host conformance check.
 *
 * The parser needs one function from the resolver -- the dotted-quad reader --
 * and nothing else. The check compiles the parser against this declaration; the
 * driver beside it provides the definition, so the parser's own logic is what
 * the cases exercise, not the resolver's file and port machinery.
 */

#ifndef AEGIR_RESOLVE_H
#define AEGIR_RESOLVE_H

#include <stdint.h>

namespace aegir::resolve {

uint32_t parse_ipv4(char const *text) noexcept;

}  // namespace aegir::resolve

#endif  // AEGIR_RESOLVE_H
