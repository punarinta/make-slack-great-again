// The conversation's Files tab: the files shared in it, newest first, one
// row each — a 40 px tile (the picture for an image or a PDF's page, else
// the file type's square), the name in bold over "who · date · type ·
// size". A click opens the file: a picture in the file viewer, a canvas in
// the canvas viewer, anything else on its Slack page in the browser. The
// list comes a page at a time (Backend::loadConversationFiles); the next
// page loads as the end scrolls into view.
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shell {

class FilesPage : public ui::View {
public:
    explicit FilesPage(screens::Context &ctx);
    ~FilesPage() override;

    // Shows conv's files, asking for the first page again (what was shown
    // stays until it answers when it is the same conversation).
    void open(model::ConvRef conv);
    void clear();

    // Tests: what the page shows.
    size_t             rowCount() const { return _files.size(); }
    const model::File &file(size_t i) const { return _files[i].file; }
    std::string        rowText(size_t i) const; // "name\nwho · date · type · size"
    const std::string &statusText() const { return _statusText; }
    bool               loading() const { return _loading; }
    void               activate(size_t i); // a click on row i
    void               loadMore();         // the next page, if there is one

    void layout() override;

private:
    class Row;
    void        load(int page);
    void        addRows(size_t from);
    void        setStatus(std::string text);
    std::string subline(const model::SharedFile &f) const;
    void        loadMoreSoon(); // near the end: the next page, on the next loop turn

    screens::Context              &_ctx;
    ui::ScrollView                *_scroll = nullptr;
    ui::View                      *_list   = nullptr;
    ui::Label                     *_status = nullptr;
    std::vector<model::SharedFile> _files;
    std::vector<Row *>             _rows;
    std::string                    _statusText;
    model::ConvRef                 _conv     = model::kNoConv;
    int                            _nextPage = 0; // 0 = no more
    uint64_t                       _seq      = 0; // invalidates answers for an earlier open
    bool                           _loading  = false;
    bool                           _queued   = false; // loadMoreSoon posted
    std::shared_ptr<bool>          _alive    = std::make_shared<bool>(true);
};

} // namespace shell
