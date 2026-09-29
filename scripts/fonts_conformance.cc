/*
 * Host conformance for the font catalog (specs/fonts.md).
 *
 * The two halves that need no filesystem are asserted here: reading a BDF
 * header for the face's own name, and choosing a face from a list. The scan
 * itself -- walking `Sys:Fonts` and reading a header off the volume -- is the
 * boot's to prove (the toolkit logs the face it loaded).
 */

#include <aegir/trinket/fonts.h>

#include <cstdio>

namespace {

using aegir::trinket::FontFace;
using aegir::trinket::probe_bdf;
using aegir::trinket::select_face;

unsigned g_checks = 0;
unsigned g_failures = 0;

void expect(bool ok, char const *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

FontFace face(char const *family, char const *weight, char const *slant, int size)
{
    FontFace f;
    f.family = family;
    f.weight = weight;
    f.slant = slant;
    f.pixel_size = size;
    return f;
}

}  // namespace

int main()
{
    /* A BDF header names its own face: the family, the weight, the slant and
     * the pixel size, quoted or bare, and nothing after the properties. */
    {
        char const *header =
            "STARTFONT 2.1\n"
            "FONT -xos4-Terminus-Medium-R-Normal--12-120-72-72-C-60-ISO10646-1\n"
            "SIZE 12 72 72\n"
            "STARTPROPERTIES 20\n"
            "FAMILY_NAME \"Terminus\"\n"
            "WEIGHT_NAME \"Medium\"\n"
            "SLANT \"R\"\n"
            "PIXEL_SIZE 12\n"
            "CHARSET_REGISTRY \"ISO10646\"\n"
            "ENDPROPERTIES\n"
            "CHARS 1356\n";
        FontFace out;
        expect(probe_bdf(header, &out), "a BDF header probes");
        expect(out.family == "Terminus", "the family is read");
        expect(out.weight == "Medium", "the weight is read");
        expect(out.slant == "R", "the slant is read");
        expect(out.pixel_size == 12, "the pixel size is read");
    }

    /* Bare values and a CRLF file are the same header. */
    {
        FontFace out;
        expect(probe_bdf("FAMILY_NAME terminus\r\nWEIGHT_NAME Bold\r\nSLANT I\r\n"
                         "PIXEL_SIZE 14\r\nCHARS 1\r\n",
                         &out),
               "a bare, CRLF header probes");
        expect(out.family == "terminus" && out.weight == "Bold" && out.slant == "I" &&
                   out.pixel_size == 14,
               "the bare fields are read");
    }

    /* A header with no family is not a face, and the properties end at CHARS:
     * a FAMILY_NAME after it belongs to nothing. */
    {
        FontFace out;
        expect(!probe_bdf("STARTFONT 2.1\nPIXEL_SIZE 12\nCHARS 3\n", &out),
               "a header with no family is refused");
        expect(!probe_bdf("FAMILY_NAME\nPIXEL_SIZE 12\nENDPROPERTIES\n", &out),
               "an empty family is refused");
        expect(probe_bdf("FAMILY_NAME \"A\"\nENDPROPERTIES\nFAMILY_NAME \"B\"\n", &out) &&
                   out.family == "A",
               "the properties end at ENDPROPERTIES");
    }

    /* The style is the weight and the slant the header carried. */
    {
        FontFace bold = face("Terminus", "Bold", "R", 12);
        FontFace italic = face("Terminus", "Medium", "I", 12);
        expect(bold.bold() && !bold.italic(), "Bold is bold");
        expect(italic.italic() && !italic.bold(), "I is italic");
    }

    std::vector<FontFace> const faces = {
        face("Terminus", "Medium", "R", 12),
        face("Terminus", "Bold", "R", 12),
        face("Terminus", "Medium", "I", 12),
        face("Terminus", "Medium", "R", 16),
        face("Noto Sans", "Medium", "R", 12),
    };

    /* Exact wins; the family is matched case-blind. */
    {
        FontFace const *f = select_face(faces, "Terminus", 16, false, false);
        expect(f != nullptr && f->pixel_size == 16 && f->family == "Terminus",
               "the exact size wins");
        expect(select_face(faces, "terminus", 16, false, false) == f,
               "the family matches case-blind");
        expect(select_face(faces, "TERMINUS", 16, false, false) == f,
               "the family matches case-blind, upper");
    }

    /* The size nearest the ask, then the style nearest it. */
    {
        FontFace const *up = select_face(faces, "Terminus", 15, false, false);
        expect(up != nullptr && up->pixel_size == 16, "a nearer larger size wins");
        FontFace const *down = select_face(faces, "Terminus", 13, false, false);
        expect(down != nullptr && down->pixel_size == 12, "a nearer smaller size wins");
        FontFace const *bold = select_face(faces, "Terminus", 12, true, false);
        expect(bold != nullptr && bold->bold(), "the style breaks a size tie");
        FontFace const *italic = select_face(faces, "Terminus", 12, false, true);
        expect(italic != nullptr && italic->italic(), "the slant breaks a size tie");
    }

    /* A family that is absent -- and an empty list -- is nothing to fall back
     * from; a different family is never chosen. */
    {
        expect(select_face(faces, "Missing", 12, false, false) == nullptr,
               "a missing family finds nothing");
        expect(select_face({}, "Terminus", 12, false, false) == nullptr,
               "an empty catalog finds nothing");
    }

    std::printf("fonts: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
