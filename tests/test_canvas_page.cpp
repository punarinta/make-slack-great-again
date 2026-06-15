// SPDX-License-Identifier: GPL-3.0-or-later
// Integration coverage for CanvasPage against a canvas that contains a top-level
// image. Guards two regressions:
//   1. The body renders (not just the title) when content loads.
//   2. Editing a text section produces a surgical ReplaceSection — NOT a
//      whole-document ReplaceAll that would round-trip the image's relative blob
//      URL back to Slack (which breaks the save and, via the retry loop, used to
//      leave the body blank).
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "backend/fake_backend/fake_backend.h"
#include "session/session.h"
#include "ui/canvas_page/canvas_page.h"

#include <QApplication>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>

namespace {

// FakeBackend (implements every pure virtual) serving rich canvas HTML with an
// embedded blob image, and recording the changes editCanvas would send.
class CanvasBackend : public FakeBackend {
public:
    std::vector<CanvasChange> lastChanges;

    void editCanvas(
        const QString &,
        const std::vector<CanvasChange>   &changes,
        std::function<void(bool, QString)> done
    ) override {
        lastChanges = changes;
        if (done)
            done(true, {});
    }
    void loadCanvasMeta(
        const QString &, std::function<void(QString, QString, CanvasMetaState)> done
    ) override {
        if (done)
            done(
                QStringLiteral("Development process"),
                QStringLiteral("http://x"),
                CanvasMetaState::Ok
            );
    }
    void loadCanvasContent(
        const QString &, std::function<void(QString)> onHtml, std::function<void(QString)>
    ) override {
        if (onHtml)
            onHtml(QStringLiteral(
                "<div class=\"quip-canvas-content\">"
                "<h1 id=\"t\">Development process</h1>"
                "<h1 id=\"a\">The stages in the Development process</h1>"
                "<img src='/collab-slack-blob/B/F1?size_name=thumb_1024' id='img1' "
                "alt='diagram' width='64' height='23'>"
                "<h1 id=\"b\">Below is the description</h1>"
                "<p id=\"c\" class=\"line\">Engineer pings "
                "<lnk href='https://x.com/p'>Petter</lnk> and Jonas</p>"
                "<p id=\"d\" class=\"line\">Ready for sprint plain text</p>"
                "</div>"
            ));
    }
};

void pump() {
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents();
    }
}

} // namespace

TEST_CASE("CanvasPage renders body and saves image canvases surgically") {
    auto       backend = std::make_unique<CanvasBackend>();
    auto      *be      = backend.get();
    Session    session(std::move(backend), QStringLiteral("T0"));
    CanvasPage page;
    page.setSession(&session);
    page.resize(900, 700);
    page.open(
        ConversationId{QStringLiteral("C1")},
        QStringLiteral("F1"),
        QStringLiteral("Development process")
    );
    pump();

    auto *body = page.findChild<QTextBrowser *>();
    REQUIRE(body != nullptr);

    // Body rendered (title h1 stripped, the rest present).
    QString plain;
    for (QTextBlock b = body->document()->begin(); b.isValid(); b = b.next())
        plain += b.text() + "\n";
    REQUIRE(plain.contains("The stages in the Development process"));
    REQUIRE(plain.contains("Below is the description"));
    REQUIRE(plain.contains("Ready for sprint plain text"));

    // Edit the last paragraph and force the autosave.
    QTextCursor c(body->document());
    c.movePosition(QTextCursor::End);
    c.insertText(" EDIT");
    page.flushPendingSave();
    pump();

    // The image canvas must stay on the section-diff path: exactly one
    // ReplaceSection, no whole-document ReplaceAll, and no relative blob URL.
    REQUIRE(be->lastChanges.size() == 1);
    CHECK(be->lastChanges[0].op == CanvasChange::Op::ReplaceSection);
    CHECK(be->lastChanges[0].sectionId == QStringLiteral("d"));
    CHECK(be->lastChanges[0].markdown.contains("EDIT"));
    for (const auto &ch : be->lastChanges) {
        CHECK(ch.op != CanvasChange::Op::ReplaceAll);
        CHECK_FALSE(ch.markdown.contains("collab-slack-blob"));
    }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-canvas-page");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}
