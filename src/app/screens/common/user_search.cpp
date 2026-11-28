#include "app/screens/common/user_search.h"

#include "base/str.h"
#include "base/utf8.h"

namespace screens {

std::string userSearchKey(const model::User &u, bool withTitle) {
    // Whichever names the label isn't (Settings → Names) match as well.
    std::string key(u.label());
    for (const std::string *n : {&u.realName, &u.profileName})
        if (!n->empty() && *n != u.label())
            key = str::concat({key, " ", *n});
    key =
        withTitle ? str::concat({key, " ", u.name, " ", u.title}) : str::concat({key, " ", u.name});
    return utf8::foldCase(key);
}

} // namespace screens
