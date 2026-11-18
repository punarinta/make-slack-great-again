#include "app/model/jobs.h"
#include "support/test.h"

#ifdef MSGA_TEST_RELAY
#include "app/claude/relay_protocol.h"
#include "app/claude/relay_server.h"

#include <string_view>
#endif

int main(int argc, char **argv) {
#ifdef MSGA_TEST_RELAY
    // test_relay starts this binary as the relay a remote msga runs.
    if (argc >= 2 && std::string_view(argv[1]) == claude::relay::kFlag)
        return claude::runRelay(argc, argv);
#endif
    const int failed = base::test::runAll(argc, argv);
    // As the app does on quit: background work still under way reports to
    // nobody, not to the App the suites' statics destroy after this returns.
    model::stopBackground();
    return failed;
}
