// local_import.h where the build has no importer.
#include "app/slack/local_import.h"

namespace slack {

bool localImportSupported() {
    return false;
}

LocalImport importLocalSession() {
    LocalImport r;
    r.error = "unsupported_platform";
    return r;
}

} // namespace slack
