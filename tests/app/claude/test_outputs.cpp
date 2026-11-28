// Files an agent made: the paths an answer names, and the copies kept of the
// ones its turn made — against throwaway folders (never the user's cache).
#include "app/claude/common.h"
#include "app/claude/outputs.h"

#include "base/file.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"

#include <cstdlib>
#include <string>
#include <sys/time.h>
#include <unistd.h>

using namespace claude;

namespace {

struct TempDirs {
    std::string root;
    TempDirs() {
        root = base::test::makeTempDir("claude_outputs_test_");
        setDirs({root + "/data", root + "/cache"});
    }
    ~TempDirs() {
        file::removeTree(root);
        setDirs({});
    }
};

void writeFile(const std::string &path, std::string_view data) {
    file::writeAtomic(path, data);
}

void be32(std::string &out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out += char((v >> s) & 0xFF);
}

// A PNG's signature and header: all model::imageSize reads.
std::string pngBytes(uint32_t w, uint32_t h) {
    std::string out = "\x89PNG\r\n\x1a\n";
    be32(out, 13);
    out += "IHDR";
    be32(out, w);
    be32(out, h);
    out += std::string("\x08\x06\x00\x00\x00", 5);
    be32(out, 0); // crc, unchecked
    return out;
}

const char kSvg[] = R"(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="160">)"
                    R"(<rect x="10" y="10" width="60" height="60" fill="blue"/></svg>)";

void setModified(const std::string &path, int64_t micros) {
    base::test::setModifiedTime(path, micros / 1000000);
}

// An answer's attachments as the backend gets them: the copies made before,
// else made now.
std::vector<model::File> outputFiles(std::string_view text, const OutputContext &ctx) {
    std::vector<model::File> files;
    if (!cachedOutputs(ctx, &files)) {
        makeOutputs(text, ctx);
        cachedOutputs(ctx, &files);
    }
    return files;
}

} // namespace

TEST("outputs: an answer names its files by path, or by folder and name") {
    TempDirs          tmp;
    const std::string d = tmp.root + "/work";
    writeFile(d + "/out/test-image.svg", kSvg);
    writeFile(d + "/out/test-image.png", pngBytes(4, 4));
    writeFile(d + "/out/notes.txt", "not shown");
    writeFile(d + "/proj/mock.pdf", "%PDF-1.4");
    writeFile(d + "/abs.png", pngBytes(2, 2));

    // Seen live 2026-09-26: a folder, then the bare names under it.
    const std::string text = str::concat(
        {"Files are in `",
         d,
         "/out/`:\n- `test-image.svg`\n- `test-image.png`\n- notes.txt\nAlso **",
         d,
         "/abs.png**, docs/missing.png, mock.pdf and https://example.com/x.png."}
    );
    const std::vector<std::string> files = mentionedFiles(text, d + "/proj");
    const std::vector<std::string> want  = {
        d + "/abs.png", d + "/out/test-image.svg", d + "/out/test-image.png", d + "/proj/mock.pdf"
    };
    CHECK(files == want);
    for (size_t i = 0; i < files.size() && i < want.size(); ++i)
        CHECK_STR(files[i], want[i]);

    // A file:// link, a "~/" path, and a path cut at a sentence's end, each once.
    const std::vector<std::string> more =
        mentionedFiles(str::concat({"See file://", d, "/abs.png and ", d, "/./abs.png!"}), "");
    REQUIRE(more.size() == 1);
    CHECK_STR(more[0], d + "/abs.png");
}

TEST("outputs: an answer's files are copied, and only the ones made in its turn") {
    TempDirs          tmp;
    const std::string d   = tmp.root + "/work";
    const int64_t     now = base::nowMicros();
    writeFile(d + "/new.svg", kSvg);
    writeFile(d + "/new.png", pngBytes(30, 20));
    writeFile(d + "/old.png", pngBytes(8, 6));
    setModified(d + "/old.png", now - 86'400'000'000LL); // made the day before: only referred to
    OutputContext ctx;
    ctx.convId     = "outputs-test";
    ctx.messageKey = "u1";
    ctx.turnStart  = now - 60'000'000;
    ctx.date       = now;

    const std::string text =
        str::concat({"Drew ", d, "/new.svg and ", d, "/new.png, next to ", d, "/old.png."});
    const auto files = outputFiles(text, ctx);
    REQUIRE(files.size() == 2);
    const model::File &svg = files[0];
    CHECK_STR(svg.name, "new.svg");
    CHECK_STR(svg.mime, "image/svg+xml");
    CHECK_STR(svg.prettyType, "SVG");
    // A picture at its own size.
    CHECK(svg.isImage());
    CHECK(svg.width == 240 && svg.height == 160);
    CHECK(str::endsWith(svg.path, ".svg"));
    CHECK(str::startsWith(svg.path, outputsDir(ctx.convId) + "/"));
    CHECK(svg.size == int64_t(sizeof(kSvg) - 1));
    CHECK_STR(svg.id, "out-u1-0");
    const model::File &png = files[1];
    CHECK(png.isImage());
    CHECK(png.width == 30);
    CHECK(png.height == 20);
    CHECK_STR(png.prettyType, "PNG");

    // The copy stays what the agent made: the file changing or going is no matter.
    file::remove(d + "/new.svg");
    const auto again = outputFiles(text, ctx);
    REQUIRE(again.size() == 2);
    CHECK_STR(again[0].path, svg.path);
    CHECK_STR(again[0].mime, svg.mime);
    CHECK(again[0].size == svg.size);
    CHECK(again[1].width == 30);

    // Another session's copies survive a prune that keeps only them.
    OutputContext other = ctx;
    other.convId        = "other";
    REQUIRE(outputFiles(text, other).size() == 1); // new.svg is gone by now
    pruneOutputs({"other"});
    CHECK_FALSE(file::exists(outputsDir(ctx.convId)));
    CHECK(file::exists(outputsDir("other")));

    clearOutputs(other.convId);
    CHECK_FALSE(file::exists(outputsDir(other.convId)));
    file::remove(d + "/new.png");
    CHECK(outputFiles(text, ctx).empty()); // gone, and not there to copy again
    // …and that is written down: the answer isn't looked through again,
    // even once a file it names turns up.
    std::vector<model::File> cached;
    CHECK(cachedOutputs(ctx, &cached));
    CHECK(cached.empty());
    writeFile(d + "/new.png", pngBytes(30, 20));
    CHECK(outputFiles(text, ctx).empty());

    // Not looked for yet: nothing cached until makeOutputs ran.
    OutputContext fresh = ctx;
    fresh.messageKey    = "u9";
    CHECK_FALSE(cachedOutputs(fresh, &cached));
    makeOutputs(text, fresh);
    CHECK(cachedOutputs(fresh, &cached));
    REQUIRE(cached.size() == 1);
    CHECK_STR(cached[0].name, "new.png");
    CHECK(str::startsWith(cached[0].path, outputsFolder(fresh) + "/"));
}

TEST("outputs: a picture this build can't decode is a file card") {
    TempDirs          tmp;
    const std::string d   = tmp.root + "/work";
    const int64_t     now = base::nowMicros();
    // An animated WebP's header (VP8X, animation flag, 4x4): a size, but no
    // frame decodeImage could draw. Seen 2026-10-02 as two empty previews.
    writeFile(
        d + "/rec.webp",
        std::string_view("RIFF\x1e\0\0\0WEBPVP8X\x0a\0\0\0\x02\0\0\0\x03\0\0\x03\0\0", 30)
    );
    writeFile(d + "/shot.png", pngBytes(5, 3));
    OutputContext ctx;
    ctx.convId     = "outputs-test";
    ctx.messageKey = "u2";
    ctx.turnStart  = now - 60'000'000;
    ctx.date       = now;

    const auto files =
        outputFiles(str::concat({"Recorded ", d, "/rec.webp and ", d, "/shot.png."}), ctx);
    REQUIRE(files.size() == 2);
    CHECK_STR(files[0].name, "rec.webp");
    CHECK_STR(files[0].prettyType, "WebP");
    CHECK_FALSE(files[0].isImage());
    CHECK_FALSE(files[0].hasPreview());
    CHECK(files[0].size == 30);
    CHECK(files[1].isImage());
    CHECK(files[1].width == 5 && files[1].height == 3);
}

TEST("outputs: paths are cleaned (separators, empty parts, . and ..)") {
    CHECK_STR(cleanPath("/r/w/b/"), "/r/w/b");
    CHECK_STR(cleanPath("/r//w/./b/../c"), "/r/w/c");
    CHECK_STR(cleanPath("/"), "/");
    CHECK_STR(cleanPath("/.."), "/");
    CHECK_STR(cleanPath("a/../../b"), "../b");
    CHECK_STR(cleanPath("./"), ".");
    CHECK_STR(str::simplified("  fix \t the\nbuild "), "fix the build");
}

TEST("outputs: an answer that names no file writes nothing down") {
    TempDirs      tmp;
    OutputContext ctx;
    ctx.convId     = "outputs-test";
    ctx.messageKey = "u3";
    ctx.date       = base::nowMicros();
    ctx.turnStart  = ctx.date - 60'000'000;
    makeOutputs("Done: the build passes now.", ctx);
    std::vector<model::File> cached;
    CHECK_FALSE(cachedOutputs(ctx, &cached)); // looked for again, cheaply, next time
    CHECK_FALSE(file::exists(outputsFolder(ctx)));
}
