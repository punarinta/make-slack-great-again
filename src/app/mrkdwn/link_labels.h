// Link labels: Slack's composer stores pasted-URL labels
// already truncated ("host/path/…/…") — only the link keeps the full URL.
// These helpers spot such labels so the message list can show a longer one
// and "Copy message" can put the full URL on the clipboard; and GIPHY media
// links, which the list draws as a "GIF" badge.
#pragma once

#include <string>
#include <string_view>

namespace mrkdwn {

// A GIPHY media asset: giphy.com or *.giphy.com, the asset under /media/ or
// a bare .gif / .webp.
bool        isGiphyMediaUrl(std::string_view url);
// The label says nothing the URL doesn't: empty, the URL (with or without
// its scheme, or a trailing slash), or Slack's truncated rendering of it.
bool        isUrlLabel(std::string_view label, std::string_view url);
// `label` contains an ellipsis ("…" or "...") and its pieces appear, in
// order, in the scheme-less URL (the first one at its start).
bool        isShortenedUrlLabel(std::string_view label, std::string_view url);
// The URL without its scheme, cut to maxChars characters with "…".
std::string expandedLabel(std::string_view url, size_t maxChars);

} // namespace mrkdwn
