// macOS: NSSpellChecker — the system's languages, and the words the user
// taught it in any app ("Learn Spelling"), which "Add to dictionary" adds to.
// Every call is an XPC round trip to the spelling service, hence the
// checker's per-word cache. Main thread only. Manual retain/release (no ARC),
// like plat's .mm files.
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#import <AppKit/AppKit.h>

#include <algorithm>

namespace spell {

namespace {

NSString *ns(std::string_view s) {
    return [[[NSString alloc] initWithBytes:s.data() length:s.size()
                                   encoding:NSUTF8StringEncoding] autorelease];
}

class MacBackend : public Backend {
public:
    MacBackend() : _tag([NSSpellChecker uniqueSpellDocumentTag]) {}
    ~MacBackend() override {
        [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag:_tag];
        for (NSString *l : _langs)
            [l release];
    }

    bool load(const std::vector<std::string> &codes) override {
        @autoreleasepool {
            NSArray<NSString *> *avail = [[NSSpellChecker sharedSpellChecker] availableLanguages];
            for (const std::string &code : codes)
                if (NSString *l = ns(code); l && [avail containsObject:l])
                    _langs.push_back([l retain]);
        }
        return !_langs.empty();
    }

    bool check(std::string_view word) override {
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = ns(word);
            if (!w)
                return true;
            for (NSString *lang : _langs) {
                const NSRange bad = [sc checkSpellingOfString:w
                                                   startingAt:0
                                                     language:lang
                                                         wrap:NO
                                       inSpellDocumentWithTag:_tag
                                                    wordCount:nullptr];
                if (bad.location == NSNotFound)
                    return true;
            }
        }
        return false;
    }

    // The words a line apiece in one string, so each language takes one round
    // trip plus one per misspelled word rather than one per word; the next
    // language sees only what this one didn't know.
    void checkAll(const std::vector<std::string> &words, std::vector<uint8_t> *right) override {
        right->assign(words.size(), 0);
        @autoreleasepool {
            NSSpellChecker         *sc = [NSSpellChecker sharedSpellChecker];
            std::vector<size_t>     todo;
            std::vector<NSString *> ws(words.size(), nil);
            for (size_t i = 0; i < words.size(); ++i) {
                ws[i] = ns(words[i]);
                if (ws[i])
                    todo.push_back(i);
                else
                    (*right)[i] = 1; // as check()
            }
            std::vector<NSRange> at;
            for (NSString *lang : _langs) {
                if (todo.empty())
                    break;
                NSMutableString *all = [NSMutableString string];
                at.clear();
                for (size_t i : todo) {
                    at.push_back(NSMakeRange(all.length, ws[i].length));
                    [all appendString:ws[i]];
                    [all appendString:@"\n"];
                }
                // Each answer is the next misspelled range from `from` on:
                // the words wholly before it are right in this language.
                std::vector<size_t> wrong;
                size_t              k    = 0;
                NSInteger           from = 0;
                while (k < todo.size()) {
                    const NSRange bad  = [sc checkSpellingOfString:all
                                                        startingAt:from
                                                          language:lang
                                                              wrap:NO
                                            inSpellDocumentWithTag:_tag
                                                         wordCount:nullptr];
                    const bool    none = bad.location == NSNotFound;
                    if (!none && (bad.length == 0 || NSInteger(bad.location) < from)) {
                        // Not an answer it should give: the rest word by word.
                        for (; k < todo.size(); ++k)
                            (*right)[todo[k]] = check(words[todo[k]]) ? 1 : 0;
                        break;
                    }
                    for (; k < todo.size() && (none || NSMaxRange(at[k]) <= bad.location); ++k)
                        (*right)[todo[k]] = 1;
                    if (none)
                        break;
                    for (; k < todo.size() && at[k].location < NSMaxRange(bad); ++k)
                        wrong.push_back(todo[k]);
                    from = NSInteger(NSMaxRange(bad));
                }
                todo = std::move(wrong);
            }
        }
    }

    std::vector<std::string> suggest(std::string_view word, int max) override {
        std::vector<std::string> out;
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = ns(word);
            if (!w)
                return out;
            for (NSString *lang : _langs) {
                NSArray<NSString *> *guesses = [sc guessesForWordRange:NSMakeRange(0, w.length)
                                                              inString:w
                                                              language:lang
                                                inSpellDocumentWithTag:_tag];
                for (NSString *g in guesses) {
                    std::string s(g.UTF8String ? g.UTF8String : "");
                    if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end())
                        out.push_back(std::move(s));
                    if (int(out.size()) >= max)
                        return out;
                }
            }
        }
        return out;
    }

    void addToDictionary(std::string_view word) override {
        @autoreleasepool {
            if (NSString *w = ns(word))
                [[NSSpellChecker sharedSpellChecker] learnWord:w];
        }
    }

private:
    NSInteger               _tag;
    std::vector<NSString *> _langs; // retained
};

} // namespace

std::unique_ptr<Backend> createPlatformBackend(plat::App &) {
    return std::make_unique<MacBackend>();
}

namespace detail {

bool listsOffThread() {
    return false;
}

std::vector<Language> availableLanguages() {
    std::vector<Language> out;
    @autoreleasepool {
        for (NSString *l in [[NSSpellChecker sharedSpellChecker] availableLanguages]) {
            const std::string code(l.UTF8String ? l.UTF8String : "");
            if (!code.empty())
                out.push_back({code, languageName(code)});
        }
    }
    return out;
}

std::string dictionaryPackageHint(const std::vector<std::string> &) {
    return {};
}

} // namespace detail

} // namespace spell
