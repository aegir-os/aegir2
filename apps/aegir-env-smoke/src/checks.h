/*
 * aegir-env-smoke: the process environment checks, behind a header with no C++
 * includes (checks.cc's boundary, the cxx-smoke's shape).
 */

#ifndef AEGIR_ENV_SMOKE_CHECKS_H
#define AEGIR_ENV_SMOKE_CHECKS_H

namespace aegir::env_smoke {

/** Check argv, the environment and the current directory, writing each result
 *  to the debug console. Returns the number that failed. */
int run();

}  // namespace aegir::env_smoke

#endif  // AEGIR_ENV_SMOKE_CHECKS_H
