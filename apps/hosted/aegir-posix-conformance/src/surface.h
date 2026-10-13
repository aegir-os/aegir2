/*
 * aegir-posix-conformance: the area entry points, one per source file.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_POSIX_CONFORMANCE_SURFACE_H
#define AEGIR_POSIX_CONFORMANCE_SURFACE_H

#include "scratch.h"

namespace aegir_posix_conformance {

void memory_surface();
void files_surface(Volume const &volume);
void directories_surface(Volume const &volume);
void process_surface();
void environment_surface();

}  // namespace aegir_posix_conformance

#endif
