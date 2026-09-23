/*
 * The two formats auth speaks, and nothing else (specs/auth.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The user database is a binary table, packed at build time from the
 * descriptor-row source by scripts/make_users.py and carried in the initrd
 * under the flat name `users.db`: the text is for people, the table is for
 * the service. auth reads it through the namespace and serves the login
 * port from it. The login answer carries no capability in v1, so the port
 * needs no Grant -- the director's default rights fit.
 */

#pragma once

#include <stdint.h>

namespace aegir::auth {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "auth.login";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/* login: in, the name's words then the secret's words (the namespace
 * protocol's one string shape, aegir/nmspace.h). Answer, one word: 1
 * authenticated, 0 refused -- and an unknown name, a wrong secret and a
 * malformed call are the same 0, because an oracle that says which half
 * failed is a small gift to a guesser. */
constexpr uint32_t kMethodLogin = 1;

}  // namespace aegir::auth

namespace aegir::authdb {

/* The table: a 16-byte header, then fixed rows. The field widths are format
 * decisions, the way FAT's 8.3 is one; the version is how they grow. */
constexpr uint32_t kMagic = 0x42445541; /* "AUDB", little-endian */
constexpr uint32_t kVersion = 2;
constexpr uint32_t kHeaderBytes = 16;

constexpr uint32_t kNameBytes = 24;    /* the system's name bound, kNameMax */
constexpr uint32_t kAccountBytes = 24; /* names what it says */
constexpr uint32_t kSecretBytes = 32;  /* a v1 plain secret fits; v1's bound */
constexpr uint32_t kHomeBytes = 48;    /* a Volume:rest path, Sys:Homes/<name> + room */

struct Row {
    char name[kNameBytes];       /* NUL-terminated within the field */
    char account[kAccountBytes]; /* NUL-terminated within the field */
    char secret[kSecretBytes];   /* NUL-terminated within the field */
    char home[kHomeBytes];       /* the path the session's Home: stands for */
};
static_assert(sizeof(Row) == 128, "the user row is a storage format");

/** The flat name the packed table travels under in the initrd. */
constexpr char kFileName[] = "users.db";
constexpr uint32_t kFileNameLength = sizeof(kFileName) - 1;

}  // namespace aegir::authdb
