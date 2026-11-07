#!/usr/bin/env python3
"""Isolate what makes Slack revoke an Enterprise Grid session.

msga's Enterprise workspace dies after a handful of API calls while ordinary
workspaces live indefinitely on the same client. Each phase removes one
explanation, so a failure tells you exactly which change to make.

    scripts/slack-session-probe.py --cookie 'xoxd-...' --url https://<workspace>.slack.com
    scripts/slack-session-probe.py --cookie 'xoxd-...' --url ... --phases 0,4

Phases
  0  Does auth.test redirect us to a different host (the org)?
  1  Does an idle session survive? (auth.test only)
  2  Does any single call kill it? (bare POST, no arguments)
  3  Does replaying Slack's d-s/uc cookies kill it?
  4  Does msga's EXACT startup sequence kill it? (GET + real parameters)
  5  Does PAGINATION kill it? (msga chases next_cursor to exhaustion)
  6  Does a session that NEVER enumerates survive? Only small calls
     (auth.test + one channel's conversations.history), for --soak-minutes.
     Survives -> enumeration volume is the trigger. Dies -> the org caps
     session lifetime for this client and no request pattern will help.
     Run this on a FRESH session, before anything else touches it.
"""
import argparse, json, re, sys, time, urllib.error, urllib.parse, urllib.request

UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/131.0.0.0 Safari/537.36")

CANDIDATES = ["commands.list", "conversations.list", "client.counts",
              "client.userBoot", "users.list", "stars.list",
              "subscriptions.thread.getView", "emoji.list", "saved.list"]

EXTRA_COOKIES = {}          # d-s / uc harvested from Set-Cookie (phase 3 only)


def api(base, method, token, cookie, replay=False, harvest=False, query=None):
    """POST with an empty body by default; pass `query` to issue a GET with a
    query string — the shape msga's WebApiClient uses for every read call."""
    jar = "d=" + cookie
    if replay:
        jar += "".join(f"; {k}={v}" for k, v in EXTRA_COOKIES.items())
    url, body = base.rstrip("/") + "/api/" + method, b""
    if query is not None:
        url += "?" + urllib.parse.urlencode(query)
        body = None
    req = urllib.request.Request(url, data=body, headers={
        "Authorization": "Bearer " + token, "Cookie": jar,
        "Content-Type": "application/x-www-form-urlencoded", "User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            if harvest:
                for sc in (r.headers.get_all("Set-Cookie") or []):
                    m = re.match(r"\s*([^=]+)=([^;]*)", sc)
                    if m and m.group(1).strip() in ("d-s", "uc", "lc"):
                        EXTRA_COOKIES[m.group(1).strip()] = m.group(2)
            return json.loads(r.read().decode())
    except Exception as e:
        return {"ok": False, "error": f"transport:{e}"}


def scrape_token(url, cookie):
    req = urllib.request.Request(url, headers={"Cookie": "d=" + cookie, "User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            body = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:          # logged-in page is served as 403
        body = e.read().decode("utf-8", "replace")
    m = re.search(r'"api_token":"(xoxc-[^"]+)"', body)
    if not m:
        t = re.search(r"<title>(.*?)</title>", body, re.S)
        sys.exit(f"no api_token on boot page (title: {t.group(1).strip() if t else '?'}) "
                 f"— the cookie is probably stale")
    return m.group(1)


def alive(base, token, cookie):
    r = api(base, "auth.test", token, cookie)
    return bool(r.get("ok")), r.get("error")


def host_of(u):
    return re.sub(r"^https?://([^/]+).*", r"\1", u)


def phase0(a, token, info):
    print("\n=== phase 0: where does auth.test send us? ===")
    reported, given = info.get("url") or "", host_of(a.url)
    rep = host_of(reported) if reported else ""
    print(f"  team={info.get('team')} team_id={info.get('team_id')} "
          f"enterprise_id={info.get('enterprise_id')}")
    print(f"  probing host : {given}")
    print(f"  auth.test url: {reported or '(none)'}")
    if rep and rep != given:
        print(f"  !! auth.test names a DIFFERENT host ({rep}); msga would repoint there:")
        for m_ in ("conversations.list", "users.list"):
            rr = api("https://" + rep, m_, token, a.cookie)
            print(f"       {m_:20} @ {rep} -> ok={rr.get('ok')} err={rr.get('error')}")
    else:
        print("  same host — repointing is a no-op here, not the bug")


def phase1(a, token):
    print(f"\n=== phase 1: auth.test only, every {a.interval:g}s "
          f"for {a.warm_minutes:g} min ===")
    deadline, n = time.time() + a.warm_minutes * 60, 0
    while time.time() < deadline:
        time.sleep(a.interval)
        n += 1
        ok, err = alive(a.url, token, a.cookie)
        print(f"  [{n:2d}] auth.test ok={ok} err={err}")
        if not ok:
            sys.exit(f"\nRESULT: died after {n} idle auth.test calls — no workspace "
                     f"call involved. The org revokes this client regardless of what "
                     f"it does; no change to which endpoints we call will help.")
    print("  survived — the session is stable when left alone.")


def phase2(a, token):
    print("\n=== phase 2: one candidate call at a time (bare POST) ===")
    for m_ in CANDIDATES:
        r = api(a.url, m_, token, a.cookie)
        ok, err = alive(a.url, token, a.cookie)
        print(f"  {m_:30} -> ok={r.get('ok')} err={r.get('error')} | alive={ok} {err or ''}")
        if not ok:
            sys.exit(f"\nRESULT: the session died immediately after {m_}. "
                     f"msga must stop making that call on Enterprise Grid.")
        time.sleep(2)
    print("  session survived every candidate call.")


def phase3(a, token):
    print("\n=== phase 3: replaying Slack's d-s/uc cookies ===")
    api(a.url, "auth.test", token, a.cookie, harvest=True)
    print(f"  harvested: {list(EXTRA_COOKIES) or 'nothing'}")
    if not EXTRA_COOKIES:
        print("  server set no companion cookies — nothing to test")
        return
    for i in range(1, 9):
        r = api(a.url, "conversations.list", token, a.cookie, replay=True, harvest=True)
        ok, err = alive(a.url, token, a.cookie)
        print(f"  [{i}] conversations.list(+cookies) ok={r.get('ok')} "
              f"err={r.get('error')} | alive={ok} {err or ''}")
        if not ok:
            sys.exit(f"\nRESULT: session died after {i} call(s) replaying d-s/uc. "
                     f"msga must send only `d`.")
        time.sleep(2)
    print("  cookie replay is not the trigger.")


def phase4(a, token, team_id):
    """msga's exact startup sequence. Phase 2 made bare POSTs with no arguments
    and everything lived, so if this kills the session the difference is in HOW
    msga asks (verb, parameters), not WHAT it asks."""
    print("\n=== phase 4: msga's exact startup calls (GET + real params) ===")
    seq = [
        ("auth.test",          {}),
        ("users.getPresence",  {}),
        ("saved.list",         {}),
        ("conversations.list", {"types": "public_channel,private_channel,im,mpim",
                                "exclude_archived": "true", "limit": "1000",
                                "team_id": team_id}),
        ("stars.list",         {"limit": "1000"}),
        ("users.list",         {"team_id": team_id, "limit": "1000"}),
        ("commands.list",      {}),
        ("emoji.list",         {}),
        ("client.counts",      {}),
    ]
    for method, q in seq:
        r = api(a.url, method, token, a.cookie, query=q)
        ok, err = alive(a.url, token, a.cookie)
        shown = ",".join(f"{k}={v}" for k, v in q.items())[:54]
        print(f"  {method:20} {shown:56} -> ok={r.get('ok')} err={r.get('error')}"
              f" | alive={ok} {err or ''}")
        if not ok:
            sys.exit(f"\nRESULT: session died right after {method} "
                     f"({shown or 'no params'}). That exact request is the trigger.")
        time.sleep(2)
    print("\nRESULT: msga's exact call sequence did NOT kill the session — the "
          "difference is concurrency/timing, not any single request.")


def phase5(a, token, team_id):
    """msga does not make one users.list call, it chases next_cursor until the
    cursor runs out. On an Enterprise Grid org that directory is the whole
    company, so a "single" logical fetch becomes a long burst of rapid
    sequential requests — the shape an abuse detector reads as scraping.
    Phases 2 and 4 only ever asked for the first page."""
    print("\n=== phase 5: cursor pagination, the way msga actually fetches ===")
    for method, base_q, key in (
        ("users.list", {"team_id": team_id, "limit": "1000"}, "members"),
        ("conversations.list", {"types": "public_channel,private_channel,im,mpim",
                                "exclude_archived": "true", "limit": "1000",
                                "team_id": team_id}, "channels"),
    ):
        cursor, page, total = "", 0, 0
        print(f"  --- {method} ---")
        while True:
            q = dict(base_q)
            if cursor:
                q["cursor"] = cursor
            r = api(a.url, method, token, a.cookie, query=q)
            page += 1
            if not r.get("ok"):
                ok, err = alive(a.url, token, a.cookie)
                print(f"    page {page:3d}: ok=False err={r.get('error')} | alive={ok} {err or ''}")
                if not ok:
                    sys.exit(f"\nRESULT: the session died during {method} pagination, "
                             f"on page {page} after {total} records. msga must stop "
                             f"walking the full directory on Enterprise Grid.")
                break
            total += len(r.get(key) or [])
            cursor = (r.get("response_metadata") or {}).get("next_cursor") or ""
            ok, err = alive(a.url, token, a.cookie)
            print(f"    page {page:3d}: +{len(r.get(key) or []):5d} (total {total:6d}) "
                  f"cursor={'yes' if cursor else 'end'} | alive={ok} {err or ''}")
            if not ok:
                sys.exit(f"\nRESULT: the session died during {method} pagination, "
                         f"on page {page} after {total} records. msga must stop "
                         f"walking the full directory on Enterprise Grid.")
            if not cursor:
                break
            if page >= 60:
                print("    (stopping at 60 pages)")
                break
        print(f"    {method}: {page} page(s), {total} records, session alive")
    print("\nRESULT: pagination did not kill the session either.")


def phase6(a, token):
    print(f"\n=== phase 6: no enumeration — small calls only, "
          f"{a.soak_minutes:g} min ===")
    channel = a.channel
    if not channel:
        sys.exit("phase 6 needs --channel <id> (any channel you're in, e.g. C0123…). "
                 "Listing conversations to find one would defeat the test.")
    deadline, n, start = time.time() + a.soak_minutes * 60, 0, time.time()
    while time.time() < deadline:
        n += 1
        h = api(a.url, "conversations.history", token, a.cookie,
                query={"channel": channel, "limit": "20"})
        ok, err = alive(a.url, token, a.cookie)
        mins = (time.time() - start) / 60
        print(f"  [{n:3d}] t+{mins:5.1f}m history ok={h.get('ok')} err={h.get('error')} "
              f"msgs={len(h.get('messages') or [])} | alive={ok} {err or ''}")
        if not ok:
            sys.exit(f"\nRESULT: died after {mins:.1f} min with NO enumeration at all "
                     f"({n} history polls). The org caps this client's session "
                     f"regardless of request volume — a client-side fix can't help.")
        time.sleep(a.interval)
    print(f"\nRESULT: survived {a.soak_minutes:g} min of polling with no enumeration. "
          f"Enumeration volume is the trigger — msga must not bulk-list per session.")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cookie", required=True, help="the `d` cookie value (xoxd-...)")
    p.add_argument("--url", required=True, help="WORKSPACE url, e.g. https://<workspace>.slack.com")
    p.add_argument("--warm-minutes", type=float, default=5.0)
    p.add_argument("--interval", type=float, default=30.0)
    p.add_argument("--phases", default="0,1,2,3,4,5", help="e.g. --phases 0,5 or --phases 6")
    p.add_argument("--channel", default="", help="channel id for phase 6")
    p.add_argument("--soak-minutes", type=float, default=15.0)
    a = p.parse_args()
    want = {int(x) for x in a.phases.split(",") if x.strip()}

    print(f"deriving token from {a.url} …")
    token = scrape_token(a.url, a.cookie)
    print(f"  token {token[:16]}…")
    info = api(a.url, "auth.test", token, a.cookie)
    print(f"  auth.test: ok={info.get('ok')} err={info.get('error')}")
    if not info.get("ok"):
        sys.exit("token dead on arrival — nothing to measure")
    team_id = info.get("team_id") or ""

    if 6 in want:                  # alone, on a fresh session
        phase6(a, token)
        return
    if 0 in want: phase0(a, token, info)
    if 1 in want: phase1(a, token)
    if 2 in want: phase2(a, token)
    if 4 in want: phase4(a, token, team_id)
    if 5 in want: phase5(a, token, team_id)
    if 3 in want: phase3(a, token)   # last: destructive


if __name__ == "__main__":
    main()
