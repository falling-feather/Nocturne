"""Merge an older standalone profile into the desktop library without replacing notes.

Sources are read-only. The caller must close applications before applying.
Run without --apply to inspect the plan; backups and an idempotent source ledger
are mandatory for writes. Linked-task/collection sources require a separate
migration and are rejected rather than losing their relationships.
"""
import argparse
import datetime
import hashlib
import html
import json
import re
import shutil
import sqlite3
from contextlib import closing
from pathlib import Path
from urllib.parse import unquote, urlparse

def connect_readonly(path):
    db = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    return db

def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()

def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def backup_profile(profile, destination):
    destination.mkdir(parents=True, exist_ok=True)
    source = connect_readonly(profile / "notebook.sqlite3")
    with closing(sqlite3.connect(destination / "notebook.sqlite3")) as snapshot:
        source.backup(snapshot)
        if snapshot.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise RuntimeError("Backup integrity check failed")
    source.close()
    attachments = profile / "attachments"
    if attachments.exists():
        for original in attachments.rglob("*"):
            if original.is_symlink():
                raise RuntimeError("Refusing to follow an attachment symlink")
            if original.is_file():
                copied = destination / "attachments" / original.relative_to(attachments)
                copied.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, copied)
                if file_digest(original) != file_digest(copied):
                    raise RuntimeError("Attachment backup hash mismatch")

def consolidate(target, source, backup_root=None, apply=False):
    if target.resolve() == source.resolve():
        raise ValueError("Source and target must differ")
    src = connect_readonly(source / "notebook.sqlite3")
    tables = {r[0] for r in src.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    for name in ("todo_links", "note_sources"):
        if name in tables and src.execute("SELECT count(*) FROM " + name).fetchone()[0]:
            raise ValueError("Source has linked records requiring a relationship-aware migration")
    notes = [dict(r) for r in src.execute("SELECT * FROM notes WHERE deleted_at IS NULL")]
    folders = [dict(r) for r in src.execute("SELECT * FROM folders")] if "folders" in tables else []
    todos = [dict(r) for r in src.execute("SELECT * FROM todos")] if "todos" in tables else []
    if not apply:
        src.close()
        return {"source": str(source), "active_notes": len(notes), "folders": len(folders),
                "todos": len(todos), "titles": [n["title"] for n in notes]}
    if backup_root is None:
        raise ValueError("An explicit backup root is required")
    backup_profile(target, backup_root / "desktop-before")
    backup_profile(source, backup_root / "source-before")
    src.close()

    db = sqlite3.connect(target / "notebook.sqlite3")
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    before = [tuple(r) for r in db.execute("SELECT * FROM notes ORDER BY id")]
    key = "profile_merge/" + digest(str(source.resolve()).casefold())
    row = db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
    ledger = json.loads(row[0]) if row else {"notes": {}, "folders": {}, "todos": {}}
    result = {"added_notes": [], "skipped_notes": [], "renamed_notes": [], "added_todos": 0}
    now = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    try:
        db.execute("BEGIN IMMEDIATE")
        def folder(name, parent):
            existing = db.execute("SELECT id FROM folders WHERE COALESCE(parent_id,0)=? AND name=? COLLATE NOCASE",
                                  (parent or 0, name)).fetchone()
            if existing:
                return existing[0]
            return db.execute(
                "INSERT INTO folders(parent_id,name,sort_order,created_at,updated_at) VALUES(?,?,0,?,?)",
                (parent, name, now, now)).lastrowid
        group = folder("旧版资料", None)
        folder_map = {None: group, 0: group}
        remaining = folders[:]
        while remaining:
            progressed = False
            for value in remaining[:]:
                if value.get("parent_id") not in folder_map:
                    continue
                mapped = ledger["folders"].get(str(value["id"]))
                if not mapped or not db.execute("SELECT 1 FROM folders WHERE id=?", (mapped,)).fetchone():
                    mapped = folder(value["name"], folder_map[value.get("parent_id")])
                folder_map[value["id"]] = mapped
                ledger["folders"][str(value["id"])] = mapped
                remaining.remove(value)
                progressed = True
            if not progressed:
                raise ValueError("Source directory hierarchy is invalid")

        attachment_map = {}
        source_assets = source / "attachments"
        if source_assets.exists():
            for original in source_assets.rglob("*"):
                if not original.is_file():
                    continue
                if original.is_symlink():
                    raise ValueError("Attachment symlinks are not supported")
                hashed = file_digest(original)
                destination = target / "attachments" / "merged-profile-assets" / (hashed + original.suffix.lower())
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not destination.exists():
                    shutil.copy2(original, destination)
                if file_digest(destination) != hashed:
                    raise ValueError("Copied attachment mismatch")
                attachment_map[str(original.resolve()).casefold()] = destination.resolve().as_uri()

        def rewrite_images(markup):
            def replace(match):
                value = html.unescape(match.group(2))
                url = urlparse(value)
                if url.scheme not in ("", "file"):
                    return match.group(0)
                raw = unquote(url.path)
                if re.match(r"^/[A-Za-z]:/", raw):
                    raw = raw[1:]
                path = Path(raw) if url.scheme else source / raw
                changed = attachment_map.get(str(path.resolve()).casefold())
                return match.group(1) + html.escape(changed, quote=True) + match.group(3) if changed else match.group(0)
            return re.sub(r"""(<img\b[^>]*\bsrc\s*=\s*["'])([^"']+)(["'])""", replace, markup, flags=re.I)

        for note in notes:
            markup = rewrite_images(note.get("html") or "")
            plain = note.get("plain_text") or ""
            kind = note.get("kind") or "note"
            fingerprint = str(note["id"]) + ":" + digest(json.dumps([note["title"], kind, markup, plain], ensure_ascii=False))
            previous = ledger["notes"].get(fingerprint)
            if previous and db.execute("SELECT 1 FROM notes WHERE id=?", (previous,)).fetchone():
                result["skipped_notes"].append(note["id"])
                continue
            duplicate = db.execute("SELECT id FROM notes WHERE deleted_at IS NULL AND title=? AND kind=? AND html=? AND plain_text=?",
                                   (note["title"], kind, markup, plain)).fetchone()
            if duplicate:
                ledger["notes"][fingerprint] = duplicate[0]
                result["skipped_notes"].append(note["id"])
                continue
            title = note["title"]
            if db.execute("SELECT 1 FROM notes WHERE deleted_at IS NULL AND title=?", (title,)).fetchone():
                title = note["title"] + "（旧版合并）"
                suffix = 2
                while db.execute("SELECT 1 FROM notes WHERE deleted_at IS NULL AND title=?", (title,)).fetchone():
                    title = note["title"] + "（旧版合并 " + str(suffix) + "）"
                    suffix += 1
                result["renamed_notes"].append({"original": note["title"], "saved": title})
            new_id = db.execute(
                "INSERT INTO notes(folder_id,kind,title,html,plain_text,excerpt,body_revision,content_hash,created_at,updated_at,deleted_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,NULL)",
                (folder_map.get(note.get("folder_id"), group), kind, title, markup, plain,
                 note.get("excerpt") or " ".join(plain.split())[:180], max(1, note.get("body_revision") or 1),
                 hashlib.sha256(markup.encode("utf-8")).digest(), note.get("created_at") or now,
                 note.get("updated_at") or now)).lastrowid
            ledger["notes"][fingerprint] = new_id
            result["added_notes"].append({"source_id": note["id"], "target_id": new_id, "title": title})
        for todo in todos:
            signature = str(todo["id"]) + ":" + digest(json.dumps([todo["text"], todo["done"], todo.get("due_at")]))
            if signature in ledger["todos"]:
                continue
            duplicate = db.execute("SELECT id FROM todos WHERE text=? AND done=? AND due_at IS ?", (todo["text"], todo["done"], todo.get("due_at"))).fetchone()
            new_id = duplicate[0] if duplicate else db.execute(
                "INSERT INTO todos(text,done,due_at,sort_order) VALUES(?,?,?,?)",
                (todo["text"], todo["done"], todo.get("due_at"), todo.get("sort_order", 0))).lastrowid
            ledger["todos"][signature] = new_id
            result["added_todos"] += int(not duplicate)
        db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)", (key, json.dumps(ledger, ensure_ascii=False)))
        after_originals = [tuple(db.execute("SELECT * FROM notes WHERE id=?", (r[0],)).fetchone()) for r in before]
        if before != after_originals:
            raise RuntimeError("An existing desktop note was modified")
        if db.execute("PRAGMA foreign_key_check").fetchall():
            raise RuntimeError("Merged relationships failed integrity check")
        db.commit()
        if db.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise RuntimeError("Merged database integrity check failed")
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()
    (backup_root / "merge-report.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--backup-root", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    print(json.dumps(consolidate(args.target, args.source, args.backup_root, args.apply), ensure_ascii=True))
