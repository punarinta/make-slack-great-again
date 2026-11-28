// The signed-in workspaces of every service: one record
// per workspace, keyed "service:id" ("slack:T0123"), in display order, plus
// which one is active and which are muted. The store never interprets a
// record's `auth` blob — each service encodes its own credentials there
// (slack::Credentials).
//
// On disk: <configDir>/workspaces.json (app/identity.h), owner-only. A
// switch writes only workspaces.json.active, in place and without fsync (a
// torn or lost one opens the file's own "active" instead). The first start
// imports the list kept by earlier versions (app/legacy). The
// auth blobs live where earlier versions kept them, under the keys
// "workspace/<key>/auth" (base/secret.h: the macOS Keychain, the old
// settings store elsewhere); only when that refuses do they stay in the file.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace auth {

struct WorkspaceRecord {
    std::string service, id;   // "slack", "T0123"
    std::string displayName;   // the workspace name
    std::string iconUrl;       // the server's icon; may be empty
    std::string auth;          // opaque per-service blob (JSON)
    bool        muted = false; // no notifications / tray tint from it

    std::string key() const { return service + ":" + id; }
    // key() == k, without building the key.
    bool        hasKey(std::string_view k) const;
};

class WorkspaceStore {
public:
    // Loads `path` (a missing or broken file is an empty store).
    explicit WorkspaceStore(std::string path);

    const std::vector<WorkspaceRecord> &all() const { return _records; }
    const WorkspaceRecord              *find(std::string_view key) const;
    bool                                empty() const { return _records.empty(); }

    // Inserts (at the end) or replaces the record with the same key, and saves
    // (an unchanged record is not written again).
    void save(WorkspaceRecord rec);
    // Drops the record (and its keychain item); the active one moves to the
    // first remaining workspace.
    void remove(std::string_view key);
    void setMuted(std::string_view key, bool muted);
    // Reorders to `keys`; unknown keys are ignored, missing ones keep their
    // place after the listed ones (no workspace is ever lost).
    void setOrder(const std::vector<std::string> &keys);

    // The first start's import (app/legacy): these records,
    // in this order, replace the store. An empty auth is already in the
    // secret store under the old key: it is read from there, not rewritten.
    void importRecords(std::vector<WorkspaceRecord> records, std::string_view active);

    // The workspace the app opens; "" when none (or it was removed).
    const std::string &active() const { return _active; }
    void               setActive(std::string_view key);

    // <configDir>/workspaces.json; "" when the OS gives no config dir.
    static std::string defaultPath(plat::App &app);

private:
    bool flush();
    // The active file in step with _active (cheap: no fsync, no rename);
    // false when it can't be written (then it is gone).
    bool writeActive();
    // Takes a record in, its auth from the keychain when the file has none;
    // one without a service or id, or already there, is dropped.
    void adopt(WorkspaceRecord r);
    // `key` active, or the first record when it names none.
    void setActiveOrFirst(std::string_view key);

    std::string                                  _path;
    std::vector<WorkspaceRecord>                 _records;
    std::string                                  _active;
    std::string                                  _activeOnDisk; // the active file's text
    // What the keychain holds per key (secret.h platforms): unchanged blobs
    // are not written again.
    std::unordered_map<std::string, std::string> _inKeychain;
};

} // namespace auth
