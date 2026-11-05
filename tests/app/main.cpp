#include "app/model/jobs.h"
#include "support/test.h"

int main(int argc, char **argv) {
    const int failed = base::test::runAll(argc, argv);
    // As the app does on quit: background work still under way reports to
    // nobody, not to the App the suites' statics destroy after this returns.
    model::stopBackground();
    return failed;
}
