#include "app/auth/workspaces.h"

#include "app/identity.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/secret.h"
#include "plat/plat.h"

namespace auth {

namespace {

std::string secretKey(const WorkspaceRecord &r) {
    return "workspace/" + r.key() + "/auth";
}

bool sameRecord(const WorkspaceRecord &a, const WorkspaceRecord &b) {
    return a.service == b.service && a.id == b.id && a.displayName == b.displayName &&
           a.iconUrl == b.iconUrl && a.auth == b.auth && a.muted == b.muted;
}

} // namespace

bool WorkspaceRecord::hasKey(std::string_view k) const {
    return k.size() == service.size() + 1 + id.size() && k.substr(0, service.size()) == service &&
           k[service.size()] == ':' && k.substr(service.size() + 1) == id;
}

WorkspaceStore::WorkspaceStore(std::string path) : _path(std::move(path)) {
    if (_path.empty())
        return;
    // Read even without the store: a flush corrects a stale one.
    file::readAll(_path + ".active", &_activeOnDisk);
    json::Document doc;
    std::string    err;
    if (!doc.parseFile(_path, &err)) {
        if (!err.empty())
            LOG_WARN("auth", "%s: %s", _path.c_str(), err.c_str());
        return;
    }
    const json::Value root = doc.root();
    for (const json::Value w : root["workspaces"]) {
        WorkspaceRecord r;
        r.service     = std::string(w["service"].str());
        r.id          = std::string(w["id"].str());
        r.displayName = std::string(w["displayName"].str());
        r.iconUrl     = std::string(w["iconUrl"].str());
        r.auth        = std::string(w["auth"].str());
        r.muted       = w["muted"].boolean();
        adopt(std::move(r));
    }
    // The active file is newer than the store's own "active" when it names
    // a workspace there (a switch writes only it).
    setActiveOrFirst(find(_activeOnDisk) ? std::string_view(_activeOnDisk) : root["active"].str());
}

void WorkspaceStore::adopt(WorkspaceRecord r) {
    if (r.service.empty() || r.id.empty() || find(r.key()))
        return;
    if (r.auth.empty() && secret::available()) {
        r.auth               = secret::read(secretKey(r));
        _inKeychain[r.key()] = r.auth;
    }
    _records.push_back(std::move(r));
}

void WorkspaceStore::setActiveOrFirst(std::string_view key) {
    _active = std::string(key);
    if (!find(_active))
        _active = _records.empty() ? std::string() : _records.front().key();
}

const WorkspaceRecord *WorkspaceStore::find(std::string_view key) const {
    for (const auto &r : _records)
        if (r.hasKey(key))
            return &r;
    return nullptr;
}

void WorkspaceStore::save(WorkspaceRecord rec) {
    for (auto &r : _records)
        if (r.service == rec.service && r.id == rec.id) {
            rec.muted = rec.muted || r.muted;
            if (sameRecord(r, rec))
                return;
            r = std::move(rec);
            flush();
            return;
        }
    _records.push_back(std::move(rec));
    if (_active.empty())
        _active = _records.back().key();
    flush();
}

void WorkspaceStore::remove(std::string_view key) {
    for (size_t i = 0; i < _records.size(); ++i)
        if (_records[i].hasKey(key)) {
            if (secret::available()) {
                secret::remove(secretKey(_records[i]));
                _inKeychain.erase(_records[i].key());
            }
            _records.erase(_records.begin() + i);
            break;
        }
    setActiveOrFirst(_active);
    flush();
}

void WorkspaceStore::setMuted(std::string_view key, bool muted) {
    for (auto &r : _records)
        if (r.hasKey(key) && r.muted != muted) {
            r.muted = muted;
            flush();
        }
}

void WorkspaceStore::setOrder(const std::vector<std::string> &keys) {
    std::vector<WorkspaceRecord> next;
    next.reserve(_records.size());
    for (const auto &k : keys)
        for (auto &r : _records)
            if (!r.service.empty() && r.hasKey(k)) {
                next.push_back(std::move(r));
                r.service.clear(); // taken
                break;
            }
    for (auto &r : _records)
        if (!r.service.empty())
            next.push_back(std::move(r));
    _records = std::move(next);
    flush();
}

void WorkspaceStore::importRecords(std::vector<WorkspaceRecord> records, std::string_view active) {
    _records.clear();
    _inKeychain.clear();
    for (WorkspaceRecord &r : records)
        adopt(std::move(r));
    setActiveOrFirst(active);
    flush();
}

void WorkspaceStore::setActive(std::string_view key) {
    if (!find(key) || _active == key)
        return;
    _active = std::string(key);
    if (!writeActive())
        flush(); // the store's own "active" then
}

bool WorkspaceStore::writeActive() {
    if (_path.empty() || _activeOnDisk == _active)
        return true;
    // In place: one small write, no fsync. The first one creates the file.
    const std::string path = _path + ".active";
    if (_active.empty()
            ? file::remove(path) || !file::exists(path)
            : file::overwrite(path, _active) || file::writeAtomic(path, _active, 0600)) {
        _activeOnDisk = _active;
        return true;
    }
    LOG_WARN("auth", "could not write %s", path.c_str());
    // Whatever it holds must not override the store's "active".
    file::remove(path);
    _activeOnDisk.clear();
    return false;
}

bool WorkspaceStore::flush() {
    if (_path.empty())
        return false;
    // A stale active file would override the "active" written below.
    if (!_activeOnDisk.empty())
        writeActive();
    json::Writer w(true);
    w.beginObject().key("active").value(_active).key("workspaces").beginArray();
    for (const auto &r : _records) {
        w.beginObject();
        w.key("service").value(r.service).key("id").value(r.id);
        w.key("displayName").value(r.displayName).key("iconUrl").value(r.iconUrl);
        if (r.muted)
            w.key("muted").value(true);
        // The keychain where there is one; the file when it refuses. Only a
        // changed blob is written (each keychain write may raise an OS prompt).
        bool inKeychain = false;
        if (secret::available()) {
            auto it    = _inKeychain.find(r.key());
            inKeychain = it != _inKeychain.end() && it->second == r.auth;
            if (!inKeychain && secret::write(secretKey(r), r.auth)) {
                _inKeychain[r.key()] = r.auth;
                inKeychain           = true;
            }
        }
        if (!inKeychain)
            w.key("auth").value(r.auth);
        w.endObject();
    }
    w.endArray().endObject();
    if (!file::writeAtomic(_path, w.str(), 0600)) {
        LOG_WARN("auth", "could not write %s", _path.c_str());
        return false;
    }
    return true;
}

std::string WorkspaceStore::defaultPath(plat::App &app) {
    const std::string dir = identity::configDir(app);
    return dir.empty() ? std::string() : file::join(dir, "workspaces.json");
}

} // namespace auth
