"""Regression for the non-destructive, idempotent profile consolidation utility."""
import importlib.util
import pathlib
import sqlite3
import tempfile
from contextlib import closing

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("consolidator", ROOT / "scripts/consolidate-profiles.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

SCHEMA = """
CREATE TABLE folders(id INTEGER PRIMARY KEY AUTOINCREMENT,parent_id INTEGER REFERENCES folders(id),name TEXT NOT NULL,sort_order INTEGER,created_at TEXT,updated_at TEXT);
CREATE TABLE notes(id INTEGER PRIMARY KEY AUTOINCREMENT,folder_id INTEGER REFERENCES folders(id),kind TEXT,title TEXT,html TEXT,plain_text TEXT,excerpt TEXT,body_revision INTEGER,content_hash BLOB,created_at TEXT,updated_at TEXT,deleted_at TEXT);
CREATE TABLE todos(id INTEGER PRIMARY KEY AUTOINCREMENT,text TEXT,done INTEGER,due_at TEXT,sort_order INTEGER);
CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT);
"""
def create(directory):
    directory.mkdir()
    db = sqlite3.connect(directory / "notebook.sqlite3")
    db.executescript(SCHEMA)
    return db

def add(db, title, text, markup=None, deleted=None):
    return db.execute("INSERT INTO notes(kind,title,html,plain_text,excerpt,body_revision,created_at,updated_at,deleted_at) VALUES('note',?,?,?,?,1,'2026-01-01','2026-01-01',?)",
                      (title, markup or "<p>" + text + "</p>", text, text, deleted)).lastrowid

with tempfile.TemporaryDirectory(prefix="NocturneMergeTest-") as temporary:
    root = pathlib.Path(temporary)
    target, source = root / "desktop", root / "old"
    dst, src = create(target), create(source)
    add(dst, "same", "desktop original")
    add(dst, "duplicate", "identical")
    add(src, "same", "old distinct")
    add(src, "duplicate", "identical")
    add(src, "deleted", "do not restore", deleted="2026-01-02")
    (source / "attachments").mkdir()
    image = source / "attachments" / "sample.png"
    image.write_bytes(b"attachment bytes retained exactly")
    add(src, "image", "image note", '<p><img src="' + image.resolve().as_uri() + '"></p>')
    src.execute("INSERT INTO todos(text,done,sort_order) VALUES('old task',0,0)")
    dst.commit(); src.commit()
    original_rows = dst.execute("SELECT * FROM notes ORDER BY id").fetchall()
    dst.close(); src.close()
    source_hash = module.file_digest(source / "notebook.sqlite3")
    report = module.consolidate(target, source, root / "backup1", True)
    assert len(report["added_notes"]) == 2 and len(report["skipped_notes"]) == 1
    with closing(sqlite3.connect(target / "notebook.sqlite3")) as db:
        assert db.execute("SELECT * FROM notes WHERE id IN (1,2) ORDER BY id").fetchall() == original_rows
        assert db.execute("SELECT count(*) FROM notes").fetchone()[0] == 4
        assert db.execute("SELECT count(*) FROM todos").fetchone()[0] == 1
        image_html = db.execute("SELECT html FROM notes WHERE title='image'").fetchone()[0]
        assert "merged-profile-assets" in image_html and image.resolve().as_uri() not in image_html
        changed_id = report["added_notes"][0]["target_id"]
        db.execute("UPDATE notes SET plain_text='locally edited' WHERE id=?", (changed_id,))
        db.commit()
    repeat = module.consolidate(target, source, root / "backup2", True)
    assert not repeat["added_notes"] and repeat["added_todos"] == 0
    with closing(sqlite3.connect(target / "notebook.sqlite3")) as db:
        assert db.execute("SELECT plain_text FROM notes WHERE id=?", (changed_id,)).fetchone()[0] == "locally edited"
        assert db.execute("SELECT count(*) FROM notes").fetchone()[0] == 4
    assert module.file_digest(source / "notebook.sqlite3") == source_hash
    assert image.read_bytes() == b"attachment bytes retained exactly"
    print("PASS: original preservation, conflict copies, exact deduplication, deleted-note exclusion, attachment rewrite, and idempotency")
