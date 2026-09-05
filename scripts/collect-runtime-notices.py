"""Collect notices and exact MSYS2 source-package links for shipped DLLs.

Only Python's standard library is required. No user data is collected.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import shutil
import urllib.error
import urllib.request
from pathlib import Path


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def desc_fields(path):
    text = path.read_text(encoding="utf-8")
    return dict(re.findall(r"%([A-Z]+)%\n(.*?)(?:\n\n|$)", text, re.S))


def verify_source(entry):
    base = "https://repo.msys2.org/mingw/sources/"
    stem = entry["sourcePackage"] + "-" + entry["version"].split(":")[-1] + ".src.tar."
    errors = []
    for extension in ("zst", "gz", "xz"):
        url = base + stem + extension
        try:
            request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "Nocturne-release"})
            with urllib.request.urlopen(request, timeout=30) as response:
                if response.status == 200:
                    entry["sourceArchive"] = url
                    entry["sourceBytes"] = int(response.headers.get("Content-Length", "0"))
                    return entry
        except (urllib.error.URLError, TimeoutError) as error:
            errors.append(str(error))
    raise RuntimeError("Source archive unavailable for " + entry["package"] + ": " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--msys-root", type=Path, default=Path("D:/msys64"))
    parser.add_argument("--verify-sources", action="store_true")
    args = parser.parse_args()
    package = args.package_dir.resolve()
    dlls = {path.name: path for path in package.rglob("*.dll")}
    found = set()
    entries = []
    for metadata in sorted((args.msys_root / "var/lib/pacman/local").glob("mingw-w64-ucrt-x86_64-*/desc")):
        files = (metadata.parent / "files").read_text(encoding="utf-8").splitlines()
        owned = [name for name in files if not name.endswith("/") and Path(name).name in dlls]
        if not owned:
            continue
        fields = desc_fields(metadata)
        package_name = fields["NAME"].strip()
        source_package = fields.get("BASE", package_name.replace("-ucrt-x86_64", "")).strip()
        version = fields["VERSION"].strip()
        binary_paths = []
        for name in owned:
            installed = args.msys_root / name
            shipped = dlls[installed.name]
            if sha256(installed) != sha256(shipped):
                raise RuntimeError("Shipped runtime does not match installed package: " + shipped.name)
            found.add(shipped.name)
            binary_paths.append(shipped.relative_to(package).as_posix())
        license_files = [
            name for name in files
            if not name.endswith("/")
            and "/share/" in name
            and re.search(r"(?:license|copying|copyright)", name, re.I)
        ]
        if not license_files:
            raise RuntimeError("Missing license files: " + package_name)
        notices = []
        for name in license_files:
            original = args.msys_root / name
            relative = name.split("/share/", 1)[1]
            destination = package / "licenses" / source_package / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, destination)
            notices.append(destination.relative_to(package).as_posix())
        entries.append({
            "package": package_name, "sourcePackage": source_package, "version": version,
            "homepage": fields.get("URL", "").strip(),
            "packageLicenseDeclaration": fields.get("LICENSE", "").strip().splitlines(),
            "binaries": sorted(binary_paths), "licenseFiles": sorted(notices),
            "sourceArchive": "https://repo.msys2.org/mingw/sources/" + source_package
                + "-" + version.split(":")[-1] + ".src.tar.zst"
        })
    missing = sorted(set(dlls) - found)
    if missing:
        raise RuntimeError("No installed-package attribution for: " + ", ".join(missing))
    if args.verify_sources:
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            entries = list(pool.map(verify_source, entries))
    manifest = {
        "distribution": "MSYS2 UCRT64", "sourceLinksVerified": args.verify_sources,
        "note": "Package license declarations cover full supplier packages, which may include tools not shipped here.",
        "components": entries
    }
    (package / "THIRD_PARTY_SOURCES.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = [
        "# Third-party runtime sources",
        "",
        "These exact-version MSYS2 source archives include upstream sources, PKGBUILD build recipes, and distribution patches.",
        "They are available from the designated source server at no charge. License texts are included in the licenses directory.",
        "The application loads DLLs dynamically; compatible modified versions can replace them.",
        "",
        "| Component | Version | Complete source archive |",
        "| --- | --- | --- |",
    ]
    for entry in entries:
        lines.append("| " + entry["sourcePackage"] + " | " + entry["version"]
                     + " | [Download corresponding source](" + entry["sourceArchive"] + ") |")
    (package / "THIRD_PARTY_SOURCES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"components": len(entries), "dlls": len(found), "sourcesVerified": args.verify_sources}))


if __name__ == "__main__":
    main()
