/*
 * Host conformance for the font service's probe (specs/fonts.md).
 *
 * The probe reads a face's own name without a rasterizer: a BDF's properties
 * are text at the top of the file, and a TrueType/OpenType face's is in its
 * `name` table, reached through the table directory, a collection's being a
 * directory per face. It is asserted here over the real faces the vendored
 * trees hold -- the faces the system volume ships -- because that is what the
 * index is for, and the alternative (a synthetic sfnt) would test the test.
 *
 * Usage: probe_conformance <repo root>
 */

#include "probe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using aegir::font::FaceInfo;
using aegir::font::Reader;

bool read_at(void *context, uint64_t offset, uint64_t length, void *out)
{
    int const fd = *static_cast<int *>(context);
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return false;
    }
    auto *bytes = static_cast<char *>(out);
    uint64_t total = 0;
    while (total < length) {
        ssize_t const have = ::read(fd, bytes + total, length - total);
        if (have <= 0) {
            return false;
        }
        total += static_cast<uint64_t>(have);
    }
    return true;
}

std::vector<FaceInfo> probe_path(std::string const &path)
{
    std::vector<FaceInfo> faces;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return faces;
    }
    long const size = ::lseek(fd, 0, SEEK_END);
    if (size > 0) {
        Reader reader;
        reader.context = &fd;
        reader.size = static_cast<uint64_t>(size);
        reader.at = read_at;
        faces = aegir::font::probe_file(reader);
    }
    ::close(fd);
    return faces;
}

unsigned g_checks = 0;
unsigned g_failures = 0;

void expect(bool ok, std::string const &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string describe(FaceInfo const &face)
{
    return face.family + " (" + face.style + ") index " + std::to_string(face.index);
}

/* One named face, at one index, with one family and style. */
void expect_face(std::string const &root, std::string const &relative, unsigned count,
                 unsigned index, char const *family, char const *style)
{
    std::vector<FaceInfo> const faces = probe_path(root + "/" + relative);
    std::string const where = relative + " face " + std::to_string(index);
    if (faces.size() != count) {
        expect(false, where + ": expected " + std::to_string(count) + " faces, got " +
                          std::to_string(faces.size()));
        return;
    }
    expect(faces[index].family == family, where + ": family is " + describe(faces[index]) +
                                              ", wanted " + family);
    expect(faces[index].style == style, where + ": style is " + describe(faces[index]) +
                                            ", wanted " + style);
    expect(faces[index].index == index, where + ": index is " + std::to_string(faces[index].index));
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: probe_conformance <repo root>\n");
        return 2;
    }
    std::string const root = argv[1];

    /* A BDF's properties are at the top of the file. */
    expect_face(root, "projects/terminus-font/ter-u12n.bdf", 1, 0, "Terminus", "Medium");
    expect_face(root, "projects/terminus-font/ter-u12b.bdf", 1, 0, "Terminus", "Bold");
    {
        std::vector<FaceInfo> const faces =
            probe_path(root + "/projects/terminus-font/ter-u12n.bdf");
        expect(!faces.empty() && !faces[0].scalable, "a BDF is not scalable");
        expect(!faces.empty() && faces[0].pixel_size == 12, "a BDF keeps its pixel size");
    }

    /* An OpenType face's name is in its `name` table, past its table
     * directory -- hundreds of kilobytes in, which is why a prefix will not
     * do. */
    expect_face(root, "projects/noto-fonts/hinted/ttf/NotoSans/NotoSans-Regular.ttf", 1, 0,
                "Noto Sans", "Regular");
    expect_face(root, "projects/noto-fonts/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf", 1, 0,
                "Noto Sans Mono", "Regular");
    expect_face(root, "projects/noto-fonts/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf",
                1, 0, "Noto Sans Arabic", "Regular");
    expect_face(root, "projects/noto-fonts/hinted/ttf/NotoSansHebrew/NotoSansHebrew-Regular.ttf",
                1, 0, "Noto Sans Hebrew", "Regular");
    {
        std::vector<FaceInfo> const faces =
            probe_path(root + "/projects/noto-fonts/hinted/ttf/NotoSans/NotoSans-Regular.ttf");
        expect(!faces.empty() && faces[0].scalable, "an OpenType face is scalable");
    }

    /* A collection is a directory per face: ten of them, in order. */
    char const *const ttc = "projects/noto-fonts/archive/unhinted/NotoSansCJK/"
                            "NotoSansCJK-Regular.ttc";
    expect_face(root, ttc, 10, 0, "Noto Sans CJK JP", "Regular");
    expect_face(root, ttc, 10, 2, "Noto Sans CJK SC", "Regular");
    expect_face(root, ttc, 10, 9, "Noto Sans Mono CJK HK", "Regular");

    /* A file that is not a face is not an error: it is skipped. */
    expect(probe_path(root + "/manifests/services.manifest").empty(),
           "a file that is not a face probes to nothing");
    expect(probe_path(root + "/no-such-face.ttf").empty(),
           "a file that is not there probes to nothing");

    /* The BDF probe itself, over headers a file may spell differently. */
    {
        FaceInfo info;
        expect(aegir::font::probe_bdf("FAMILY_NAME \"Terminus\"\nWEIGHT_NAME Bold\nSLANT I\n"
                                      "PIXEL_SIZE 14\nENDPROPERTIES\n",
                                      &info) &&
                   info.family == "Terminus" && info.style == "Bold Italic" &&
                   info.pixel_size == 14,
               "the BDF probe reads bare fields and joins weight and slant");
        expect(!aegir::font::probe_bdf("PIXEL_SIZE 14\nCHARS 1\n", &info),
               "a BDF header with no family is refused");
        expect(aegir::font::probe_bdf("FAMILY_NAME \"A\"\r\nENDPROPERTIES\r\n"
                                      "FAMILY_NAME \"B\"\r\n",
                                      &info) &&
                   info.family == "A",
               "the BDF properties end at ENDPROPERTIES");
    }

    std::printf("font probe: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
