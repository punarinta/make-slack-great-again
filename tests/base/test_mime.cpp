#include "support/test.h"
#include "base/mime.h"

#include <string>

TEST("mime: types by extension, any case, from a name or a path") {
    CHECK(mime::fromName("a.PNG") == "image/png");
    CHECK(mime::fromName("/tmp/x.y/photo.JPeG") == "image/jpeg");
    CHECK(mime::fromName("clip.webm") == "video/webm");
    CHECK(mime::fromName("page.htm") == "text/html");
    CHECK(mime::fromName("notes.md") == "text/markdown");
    CHECK(mime::fromName("dir.png/README").empty()); // the folder's dot is not an extension
    CHECK(mime::fromName("noext").empty());
    CHECK(mime::fromName("x.unknown").empty());
    CHECK(mime::fromNameOr("x.unknown") == "application/octet-stream");
    CHECK(mime::fromNameOr("song.flac") == "audio/flac");
}

TEST("mime: magic bytes") {
    CHECK(mime::sniff(std::string("\x89PNG\r\n\x1a\n....", 12)) == "image/png");
    CHECK(mime::sniff("\x89PNG").empty()); // too short to be one
    CHECK(mime::sniff("\xFF\xD8\xFF\xE0") == "image/jpeg");
    CHECK(mime::sniff("GIF89a...") == "image/gif");
    CHECK(mime::sniff("GIF87a") == "image/gif");
    CHECK(mime::sniff("GIF8").empty());
    CHECK(mime::sniff("RIFF\x10\0\0\0WEBPVP8 ") == ""); // NUL cut it short
    CHECK(mime::sniff(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)) == "image/webp");
    CHECK(mime::sniff(std::string("RIFF\x10\0\0\0WAVEfmt ", 16)).empty()); // a WAV is not
    CHECK(mime::sniff("%PDF-1.7") == "application/pdf");
    CHECK(mime::sniff("ID3\x04") == "audio/mpeg");
    CHECK(mime::sniff("<!DOCTYPE html>").empty());
    CHECK(mime::sniff("").empty());
}

TEST("mime: file card labels") {
    CHECK(mime::label("image/png") == "PNG");
    CHECK(mime::label("text/plain") == "Plain text");
    CHECK(mime::label("video/webm") == "WebM");
    CHECK(mime::label("application/zip") == "Zip");
    CHECK(mime::label("application/x-unknown").empty());
}

TEST("mime: the extension of a type, and text by name") {
    CHECK(mime::extension("image/png") == "png");
    CHECK(mime::extension("image/jpeg") == "jpg"); // the first of jpg, jpeg
    CHECK(mime::extension("image/bmp") == "bmp");
    CHECK(mime::extension("image/x-unknown").empty());
    for (const char *n : {"a.txt", "b.LOG", "c.yaml", "d.cpp", "e.h", "f.ts", "g.sh", "h.html"})
        CHECK(mime::isTextName(n));
    for (const char *n : {"a.png", "b.htm", "c.svg", "d.pdf", "noext", "x.y/README", "e.markdown"})
        CHECK_FALSE(mime::isTextName(n));
}
