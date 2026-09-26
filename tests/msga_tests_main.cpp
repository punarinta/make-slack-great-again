// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// main() of msga_tests: picks the suite named by argv[1], then runs that
// suite's MSGA_TEST_MAIN (or plain Catch2) restricted to the suite's file.
// See test_main.h.
#include "test_main.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Same rule as Catch2's filename tag (-#): the file name without directories
// and without its last extension.
std::string suiteOfFile(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return {};
    const auto slash = path.find_last_of("/\\", dot);
    const auto start = slash == std::string_view::npos ? 0 : slash + 1;
    return std::string(path.substr(start, dot - start));
}

std::map<std::string, msga_test::SuiteMain> &suiteMains() {
    static std::map<std::string, msga_test::SuiteMain> mains;
    return mains;
}

std::string gSuite;

std::set<std::string> knownSuites() {
    std::set<std::string> suites;
    for (const auto *info : Catch::getRegistryHub().getTestCaseRegistry().getAllInfos())
        suites.insert(suiteOfFile(info->lineInfo.file));
    return suites;
}

// Splits one Catch2 test-spec argument at its top-level commas (the OR
// separator), leaving commas inside quoted names, tags and escapes alone.
std::vector<std::string> splitAlternatives(const std::string &spec) {
    std::vector<std::string> parts(1);
    bool                     quoted = false, inTag = false;
    for (std::size_t i = 0; i < spec.size(); ++i) {
        const char c = spec[i];
        if (c == '\\' && i + 1 < spec.size()) {
            parts.back() += c;
            parts.back() += spec[++i];
            continue;
        }
        if (c == '"' && !inTag)
            quoted = !quoted;
        else if (c == '[' && !quoted)
            inTag = true;
        else if (c == ']' && !quoted)
            inTag = false;
        else if (c == ',' && !quoted && !inTag) {
            parts.emplace_back();
            continue;
        }
        parts.back() += c;
    }
    return parts;
}

// Whether an alternative has a pattern that must match (anything not negated
// with '~'). An alternative of exclusions only must keep Catch2's default of
// skipping hidden ([.]) tests once the suite's file tag is added to it.
bool hasRequiredPattern(const std::string &alt) {
    bool negated = false;
    for (std::size_t i = 0; i < alt.size(); ++i) {
        const char c = alt[i];
        if (c == ' ' || c == '\t')
            continue;
        if (c == '~') {
            negated = true;
            continue;
        }
        if (!negated)
            return true;
        negated          = false;
        // Skip the negated pattern: a tag, a quoted name or a bare name.
        const char close = c == '[' ? ']' : c == '"' ? '"' : '[';
        for (++i; i < alt.size() && alt[i] != close; ++i) {
            if (alt[i] == '\\')
                ++i;
        }
        if (close == '[')
            --i; // a bare name ends where the next tag begins
    }
    return false;
}

// Restricts the command line's test specs to the suite's file: every OR
// alternative gets the file tag ANDed in (Catch2 ANDs consecutive arguments
// and ORs comma-separated alternatives, so tagging each alternative is exact).
std::vector<std::string>
restrictToSuite(const std::vector<std::string> &specs, const std::string &suite) {
    const std::string tag = "[#" + suite + "]";
    if (specs.empty())
        return {tag + "~[.]"};
    std::vector<std::string> out;
    for (const auto &spec : specs) {
        std::string joined;
        for (const auto &alt : splitAlternatives(spec)) {
            if (!joined.empty())
                joined += ',';
            joined += tag + alt + (hasRequiredPattern(alt) ? "" : "~[.]");
        }
        out.push_back(joined);
    }
    return out;
}

void printUsage(const char *argv0) {
    std::fprintf(
        stderr,
        "usage: %s <suite> [Catch2 options and test specs]\n"
        "A suite is one test file (test_session.cpp -> test_session). Suites:\n",
        argv0
    );
    for (const auto &suite : knownSuites())
        std::fprintf(stderr, "  %s\n", suite.c_str());
}

} // namespace

namespace msga_test {

SuiteMainRegistration::SuiteMainRegistration(const char *file, SuiteMain fn) {
    suiteMains()[suiteOfFile(file)] = fn;
}

int runCatch(int argc, char **argv) {
    Catch::Session session;
    if (const int rc = session.applyCommandLine(argc, argv); rc != 0)
        return rc;
    auto &config           = session.configData();
    config.filenamesAsTags = true;
    config.testsOrTags     = restrictToSuite(config.testsOrTags, gSuite);
    return session.run();
}

} // namespace msga_test

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == '-') {
        printUsage(argv[0]);
        return 2;
    }
    gSuite = argv[1];
    if (!knownSuites().count(gSuite)) {
        std::fprintf(stderr, "%s: no suite named '%s'\n", argv[0], argv[1]);
        printUsage(argv[0]);
        return 2;
    }

    // The suite sees the command line without its own name.
    std::vector<char *> args{argv[0]};
    args.insert(args.end(), argv + 2, argv + argc);
    args.push_back(nullptr);
    int suiteArgc = argc - 1;

    const auto it = suiteMains().find(gSuite);
    return it != suiteMains().end() ? it->second(suiteArgc, args.data())
                                    : msga_test::runCatch(suiteArgc, args.data());
}
