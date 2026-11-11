#!/usr/bin/env python3
"""
End-to-end test of the AniList integration against a mock AniList GraphQL server.

Drives the real backend over the AppLoad IPC protocol and exercises: phone setup page,
auto-linking (list match / alt titles / slug ids), offline queueing, both-sides-moved-forward
merges, conflicts (reread, removed entry), remote-ahead mirroring into local read marks,
explicit edits, manual linking, and the "unsure" path.

Usage: scripts/build.sh --with-host /tmp/rmb && python3 scripts/test_anilist.py /tmp/rmb
"""

import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GOOD_TOKEN = "good-token-" + "x" * 40

MEDIA = {
    1: {"id": 1, "chapters": 139, "status": "FINISHED", "format": "MANGA", "startDate": {"year": 2009},
        "title": {"romaji": "Shingeki no Kyojin", "english": "Attack on Titan", "native": "進撃の巨人",
                  "userPreferred": "Shingeki no Kyojin"},
        "synonyms": ["AoT", "Атака титанов"], "coverImage": {"medium": ""}},
    2: {"id": 2, "chapters": None, "status": "FINISHED", "format": "MANGA", "startDate": {"year": 2018},
        "title": {"romaji": "Chainsaw Man", "english": "Chainsaw Man", "native": "チェンソーマン",
                  "userPreferred": "Chainsaw Man"}, "synonyms": [], "coverImage": {"medium": ""}},
    3: {"id": 3, "chapters": 200, "status": "FINISHED", "format": "MANGA", "startDate": {"year": 2018},
        "title": {"romaji": "Na Honjaman Level Up", "english": "Solo Leveling", "native": "나 혼자만 레벨업",
                  "userPreferred": "Na Honjaman Level Up"}, "synonyms": [], "coverImage": {"medium": ""}},
    4: {"id": 4, "chapters": None, "status": "RELEASING", "format": "MANGA", "startDate": {"year": 2020},
        "title": {"romaji": "Akyeogui Ending-eun Jugeumppun", "english": "Villains Are Destined to Die",
                  "native": "악역의 엔딩은 죽음뿐", "userPreferred": "Akyeogui Ending-eun Jugeumppun"},
        "synonyms": [], "coverImage": {"medium": ""}},
    5: {"id": 5, "chapters": None, "status": "FINISHED", "format": "NOVEL", "startDate": {"year": 2011},
        "title": {"romaji": "Shingeki no Kyojin", "english": "Attack on Titan (Novel)", "native": "進撃の巨人",
                  "userPreferred": "Shingeki no Kyojin"}, "synonyms": [], "coverImage": {"medium": ""}},
}


class Mock:
    def __init__(self):
        self.lock = threading.Lock()
        self.clock = 1000
        self.entries = {}  # mediaId -> {id, status, progress, repeat, updatedAt}
        self.offline = False
        self.calls = []

    def set_entry(self, mid, progress=None, status=None):
        with self.lock:
            e = self.entries.setdefault(mid, {"id": 900 + mid, "status": "CURRENT", "progress": 0, "repeat": 0})
            if progress is not None:
                e["progress"] = progress
            if status is not None:
                e["status"] = status
            self.clock += 10
            e["updatedAt"] = self.clock

    def entry_json(self, mid):
        e = self.entries[mid]
        return {"id": e["id"], "status": e["status"], "progress": e["progress"], "repeat": e["repeat"],
                "updatedAt": e["updatedAt"]}


MOCK = Mock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        raw = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        if MOCK.offline:
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        q = body.get("query", "")
        v = body.get("variables", {})
        auth = self.headers.get("Authorization", "")
        MOCK.calls.append(q.split("(")[0][:40] + " " + json.dumps(v))
        if "Viewer" in q:
            if auth != "Bearer " + GOOD_TOKEN:
                return self.reply(400, {"errors": [{"message": "Invalid token", "status": 400}], "data": None})
            return self.reply(200, {"data": {"Viewer": {"id": 42, "name": "tester", "avatar": {"medium": ""}}}})
        if "MediaListCollection" in q:
            if auth != "Bearer " + GOOD_TOKEN:
                return self.reply(401, {"errors": [{"message": "Invalid token", "status": 401}], "data": None})
            with MOCK.lock:
                entries = []
                for mid in MOCK.entries:
                    e = MOCK.entry_json(mid)
                    e["media"] = MEDIA[mid]
                    entries.append(e)
            return self.reply(200, {"data": {"MediaListCollection": {"lists": [{"entries": entries}]}}})
        if "SaveMediaListEntry" in q:
            mid = v["m"]
            MOCK.set_entry(mid, v.get("p"), v.get("s"))
            with MOCK.lock:
                return self.reply(200, {"data": {"SaveMediaListEntry": MOCK.entry_json(mid)}})
        if "Page(" in q:
            needle = v.get("q", "").lower()
            res = []
            for m in MEDIA.values():
                names = [m["title"][k] or "" for k in ("romaji", "english", "native")] + m["synonyms"]
                if any(needle in n.lower() or n.lower() in needle for n in names if n):
                    mm = dict(m)
                    with MOCK.lock:
                        mm["mediaListEntry"] = MOCK.entry_json(m["id"]) if m["id"] in MOCK.entries else None
                    res.append(mm)
            return self.reply(200, {"data": {"Page": {"media": res}}})
        if "Media(id" in q:
            m = dict(MEDIA[v["id"]])
            m["mediaListEntry"] = MOCK.entry_json(v["id"]) if v["id"] in MOCK.entries else None
            return self.reply(200, {"data": {"Media": m}})
        return self.reply(400, {"errors": [{"message": "unknown query"}]})


# ------------------------------------------------------------------ IPC helpers

class Ipc:
    def __init__(self, conn):
        self.c = conn
        self.backlog = []

    def send(self, t, o=None):
        raw = b"" if o is None else json.dumps(o).encode()
        self.c.send(struct.pack("<ii", t, len(raw)))
        self.c.send(raw)

    def recv(self, timeout):
        self.c.settimeout(timeout)
        hdr = self.c.recv(64)
        t, length = struct.unpack("<ii", hdr[:8])
        body = self.c.recv(16 * 1024 * 1024) if length else b""
        return t, (json.loads(body) if length else None)

    def wait(self, pred, timeout=30, what="event"):
        end = time.time() + timeout
        while time.time() < end:
            try:
                t, d = self.recv(max(0.1, end - time.time()))
            except socket.timeout:
                break
            if t == 109:
                print("   toast:", d.get("message"))
            if pred(t, d):
                return d
        raise TimeoutError("timed out waiting for " + what)

    def sync(self):
        self.send(21, {})
        saw_syncing = [False]

        def p(t, d):
            if t != 120:
                return False
            if d.get("syncing"):
                saw_syncing[0] = True
                return False
            return saw_syncing[0]
        return self.wait(p, 40, "sync")


def entry(state, mid):
    for e in state["entries"]:
        if e["media_id"] == mid:
            return e
    return None


def chapters(n, read_upto=0):
    return [{"id": "c%d" % i, "chapter_num": i, "title": "", "read": i <= read_upto, "downloaded": False}
            for i in range(1, n + 1)]


def main():
    backend = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rmb"
    tmp = tempfile.mkdtemp(prefix="rm2manga-al-")
    os.makedirs(tmp + "/data", exist_ok=True)

    aot = "v2.ru.mangalib::247--shingeki-no-kyojin"
    vil = "v2.ru.mangalib::29916--ag-yeog-uiending-eunjug-eumppun"
    unk = "v2.x::foo"
    state = {
        "settings": {"readingMode": "rtl", "fitMode": "contain", "invertSwipe": False},
        "mangas": {
            aot: {"key": aot, "source_id": "v2.ru.mangalib", "source_name": "MangaLib",
                  "manga_id": "247--shingeki-no-kyojin", "title": "Атака титанов", "in_library": True,
                  "read_chapters": {"c1": True, "c2": True, "c3": True}, "chapter_pages": {}},
            vil: {"key": vil, "source_id": "v2.ru.mangalib", "source_name": "MangaLib",
                  "manga_id": "29916--ag-yeog-uiending-eunjug-eumppun",
                  "title": "Единственный исход злодейки — смерть", "in_library": True,
                  "description": "★ 9.78\n\nАльтернативные названия:<br/>Villains Are Destined to Die<br/>악역의 엔딩은 죽음뿐",
                  "read_chapters": {}, "chapter_pages": {}},
            unk: {"key": unk, "source_id": "v2.x", "source_name": "X", "manga_id": "foo",
                  "title": "Totally Unknown Manga", "in_library": True, "read_chapters": {}, "chapter_pages": {}},
            "v2.ru.mangalib::7965--chainsaw-man": {"key": "v2.ru.mangalib::7965--chainsaw-man",
                  "source_id": "v2.ru.mangalib", "source_name": "MangaLib", "manga_id": "7965--chainsaw-man",
                  "title": "Человек-бензопила", "in_library": False, "read_chapters": {}, "chapter_pages": {}},
        },
        "chapters_cache": {aot: chapters(12, 3), vil: chapters(5), unk: chapters(3),
                           "v2.ru.mangalib::7965--chainsaw-man": chapters(10, 8)},
        "downloads": {},
    }
    json.dump(state, open(tmp + "/data/state.json", "w"))

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    MOCK.set_entry(1, 2, "CURRENT")
    MOCK.set_entry(4, 0, "PLANNING")

    sp = tmp + "/ipc.sock"
    lsock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    lsock.bind(sp)
    lsock.listen(1)
    env = dict(os.environ, RM2MANGA_DATA_ROOT=tmp, RM2MANGA_DISABLE_COVERS="1",
               RM2MANGA_ANILIST_URL="http://127.0.0.1:%d/" % srv.server_port,
               RM2MANGA_AIDOKU_RUN="/nonexistent", RM2MANGA_PAGE_CACHE_DIR=tmp + "/pages")
    proc = subprocess.Popen([backend, sp], env=env, cwd=tmp)
    conn, _ = lsock.accept()
    ipc = Ipc(conn)
    ok = False
    try:
        ipc.send(-2)
        ipc.wait(lambda t, d: t == 100 and d.get("installed_sources") is not None and not d.get("aidoku_online", True) is None)
        st = ipc.wait(lambda t, d: t == 120, what="anilist state")
        assert st["connected"] is False

        print("1. phone setup page")
        ipc.send(22, {"start": True})
        ev = ipc.wait(lambda t, d: t == 121 and d.get("running"), what="setup server")
        assert ev["qr"] and len(ev["qr"]) > 20, "QR code missing"
        url = [u for u in ev["urls"]][0]
        local_url = re.sub(r"http://[^:/]+", "http://127.0.0.1", url)
        html = urllib.request.urlopen(local_url).read().decode()
        assert "Client ID" in html, "expected client id step"
        urllib.request.urlopen(local_url + "/client", data=b"client_id=12345").read()
        html = urllib.request.urlopen(local_url).read().decode()
        assert "client_id=12345" in html and "response_type=token" in html
        html = urllib.request.urlopen(local_url + "/token", data=b"token=garbage-token-garbage-token").read().decode()
        assert "Invalid token" in html or "rejected" in html, html[-300:]
        html = urllib.request.urlopen(local_url + "/token",
                                      data=("token=" + urllib.parse.quote(GOOD_TOKEN)).encode()).read().decode()
        assert "Connected as tester" in html
        ipc.wait(lambda t, d: t == 121 and d.get("connected"), what="connected event")

        print("2. first sync: auto-link + push local progress")
        st = ipc.wait(lambda t, d: t == 120 and not d.get("syncing") and d.get("last_sync_at", 0) > 0, 40, "first sync")
        assert MOCK.entries[1]["progress"] == 3, MOCK.entries[1]
        assert MOCK.entries[4]["progress"] == 0 and MOCK.entries[4]["status"] == "PLANNING"
        al = json.load(open(tmp + "/data/anilist.json"))
        assert al["links"][aot]["media_id"] == 1, "AoT not linked"
        assert al["links"][vil]["media_id"] == 4, "Villains not linked via alt title"
        assert unk not in al["links"] and unk in al["unsure"], "unknown should be unsure"

        print("3. read ch 4 online -> pushed")
        ipc.send(7, {"source_id": "v2.ru.mangalib", "manga_id": "247--shingeki-no-kyojin", "chapter_id": "c4",
                     "chapter_num": 4, "page_index": 9, "page_count": 10, "completed": True})
        ipc.sync()
        assert MOCK.entries[1]["progress"] == 4

        print("4. offline: read ch 5, 6 -> queued")
        MOCK.offline = True
        for c in (5, 6):
            ipc.send(7, {"source_id": "v2.ru.mangalib", "manga_id": "247--shingeki-no-kyojin",
                         "chapter_id": "c%d" % c, "chapter_num": c, "page_index": 9, "page_count": 10,
                         "completed": True})
        st = ipc.sync()
        assert st["online"] == 0 and st["pending_count"] == 1, st
        assert entry(st, 1)["progress"] == 6 and entry(st, 1)["pending"]

        print("5. phone moved forward meanwhile (ch 5) -> merge pushes 6, no conflict")
        MOCK.set_entry(1, 5)
        MOCK.offline = False
        st = ipc.sync()
        assert MOCK.entries[1]["progress"] == 6 and st["conflict_count"] == 0 and st["pending_count"] == 0

        print("6. reread started on phone while device offline read ch 7 -> conflict, keep device")
        MOCK.offline = True
        ipc.send(7, {"source_id": "v2.ru.mangalib", "manga_id": "247--shingeki-no-kyojin", "chapter_id": "c7",
                     "chapter_num": 7, "page_index": 9, "page_count": 10, "completed": True})
        MOCK.set_entry(1, 1, "REPEATING")
        MOCK.offline = False
        st = ipc.sync()
        assert st["conflict_count"] == 1, st
        c = st["conflicts"][0]
        assert c["reason"] == "went_back" and c["local_progress"] == 7 and c["remote_progress"] == 1, c
        assert MOCK.entries[1]["progress"] == 1, "must not push while unsure"
        ipc.send(27, {"media_id": 1, "choice": "local"})
        ipc.wait(lambda t, d: t == 120 and d.get("conflict_count") == 0)
        st = ipc.sync()
        assert MOCK.entries[1]["progress"] == 7 and MOCK.entries[1]["status"] == "REPEATING", MOCK.entries[1]

        print("7. AniList ahead (ch 10 read on phone) -> local chapters marked read")
        MOCK.set_entry(1, 10, "CURRENT")
        st = ipc.sync()
        assert st.get("local_changed") is True
        s = json.load(open(tmp + "/data/state.json"))
        rc = s["mangas"][aot]["read_chapters"]
        assert all(rc.get("c%d" % i) for i in range(1, 11)) and not rc.get("c11"), rc
        assert s["mangas"][aot]["unread_count"] == 2

        print("8. explicit edit: mark completed")
        ipc.send(26, {"media_id": 1, "progress": 139, "status": "COMPLETED"})
        st = ipc.sync()
        assert MOCK.entries[1]["status"] == "COMPLETED" and MOCK.entries[1]["progress"] == 139

        print("9. first chapter of a PLANNING title -> becomes Reading")
        ipc.send(8, {"source_id": "v2.ru.mangalib", "manga_id": "29916--ag-yeog-uiending-eunjug-eumppun",
                     "chapter_id": "c1", "read": True})
        st = ipc.sync()
        assert MOCK.entries[4]["status"] == "CURRENT" and MOCK.entries[4]["progress"] == 1, MOCK.entries[4]

        print("10. manual search + link for an unknown title")
        ipc.send(24, {"source_id": "v2.x", "manga_id": "foo", "query": "solo leveling"})
        ev = ipc.wait(lambda t, d: t == 122, what="search results")
        assert ev["results"] and ev["results"][0]["media_id"] == 3
        ipc.send(25, {"source_id": "v2.x", "manga_id": "foo", "media": ev["results"][0]})
        ev = ipc.wait(lambda t, d: t == 123, what="manga anilist info")
        assert ev["anilist"]["linked"] and ev["anilist"]["on_list"] is False
        ipc.send(8, {"source_id": "v2.x", "manga_id": "foo", "chapter_id": "c1", "read": True})
        st = ipc.sync()
        assert MOCK.entries[3]["progress"] == 1 and MOCK.entries[3]["status"] == "CURRENT"

        print("11. entry deleted on phone while device offline read ch 2 -> conflict, keep AniList")
        MOCK.offline = True
        ipc.send(8, {"source_id": "v2.x", "manga_id": "foo", "chapter_id": "c2", "read": True})
        with MOCK.lock:
            del MOCK.entries[3]
        MOCK.offline = False
        st = ipc.sync()
        assert st["conflict_count"] == 1 and st["conflicts"][0]["reason"] == "removed", st["conflicts"]
        ipc.send(27, {"media_id": 3, "choice": "remote"})
        ipc.wait(lambda t, d: t == 120 and d.get("conflict_count") == 0)
        st = ipc.sync()
        assert 3 not in MOCK.entries and st["pending_count"] == 0

        print("12. linking a title that is Dropped on AniList but read further here -> ask")
        MOCK.set_entry(2, 5, "DROPPED")
        ipc.sync()
        ipc.send(25, {"source_id": "v2.ru.mangalib", "manga_id": "7965--chainsaw-man", "manga_title": "Человек-бензопила",
                      "source_name": "MangaLib", "media": {"media_id": 2, "title": "Chainsaw Man", "chapters": 0}})
        st = ipc.sync()
        assert st["conflict_count"] == 1 and st["conflicts"][0]["reason"] == "status", st["conflicts"]
        assert MOCK.entries[2]["status"] == "DROPPED" and MOCK.entries[2]["progress"] == 5
        ipc.send(27, {"media_id": 2, "choice": "local"})
        ipc.wait(lambda t, d: t == 120 and d.get("conflict_count") == 0)
        ipc.sync()
        assert MOCK.entries[2]["status"] == "CURRENT" and MOCK.entries[2]["progress"] == 8, MOCK.entries[2]

        print("13. state survives restart (offline queue persisted)")
        MOCK.offline = True
        ipc.send(8, {"source_id": "v2.ru.mangalib", "manga_id": "29916--ag-yeog-uiending-eunjug-eumppun",
                     "chapter_id": "c2", "read": True})
        time.sleep(0.5)
        al = json.load(open(tmp + "/data/anilist.json"))
        assert al["pending"]["4"]["progress"] == 2
        ok = True
        print("ALL ANILIST TESTS PASSED")
    finally:
        try:
            ipc.send(-1)
            proc.wait(5)
        except Exception:
            proc.kill()
        srv.shutdown()
        if ok:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
