"""Bundle the exact vcpkg source inputs, verified against the installed packages.

Uses the pinned recipes in tools/dependency-recipes and verified upstream archives.
The source ZIP contains extracted source trees, never nested download archives.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request


def digest(path: Path, algorithm: str = "sha256") -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def contained(root: Path, relative: str) -> Path:
    parts = PurePosixPath(relative)
    if parts.is_absolute() or ".." in parts.parts or "\\" in relative or ":" in relative:
        raise ValueError(f"Unsafe source path: {relative}")
    result = root.joinpath(*parts.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Source path escapes destination: {relative}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installed", type=Path, required=True)
    parser.add_argument("--vcpkg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    spec = json.loads((repo / "tools/dependency-sources.json").read_text(encoding="utf-8"))
    project = json.loads((repo / "vcpkg.json").read_text(encoding="utf-8"))
    if spec["baseline"] != project["builtin-baseline"]:
        raise ValueError("Dependency baseline changed; refresh the source inventory and recipes.")
    if args.output.exists():
        raise ValueError("Dependency source destination must be new.")
    args.output.mkdir(parents=True)
    prior = repo / "dependency-sources"
    prior_manifest = json.loads((prior / "manifest.json").read_text()) if (prior / "manifest.json").is_file() else None
    records = []
    for dependency in spec["dependencies"]:
        name = dependency["name"]
        share = args.installed / "x64-windows-static-md/share" / name
        sbom = json.loads((share / "vcpkg.spdx.json").read_text(encoding="utf-8"))
        port = next(p for p in sbom["packages"] if p["SPDXID"] == "SPDXRef-port")
        resource = next(p for p in sbom["packages"] if p["SPDXID"] == "SPDXRef-resource-0")
        sha = next(c["checksumValue"] for c in resource["checksums"] if c["algorithm"] == "SHA512")
        if port["versionInfo"] != dependency["version"] or sha != dependency["sha512"]:
            raise ValueError(f"Installed {name} differs from the source inventory.")
        dest = args.output / name
        upstream = dest / "upstream"
        upstream.mkdir(parents=True)
        for record in dependency["recipeFiles"]:
            recipe = contained(repo / "tools/dependency-recipes" / name, record["path"])
            actual = next(f for f in sbom["files"] if f["SPDXID"].startswith("SPDXRef-port-file") and f["fileName"].removeprefix("./") == record["path"])
            actual_sha = next(c["checksumValue"] for c in actual["checksums"] if c["algorithm"] == "SHA256")
            if digest(recipe) != record["sha256"] or actual_sha != record["sha256"]:
                raise ValueError(f"Recipe differs from installed {name}: {record['path']}")
        # Repackaging a published source tree can reuse its verified source inputs.
        if prior_manifest and prior_manifest["dependencies"] == spec["dependencies"]:
            matches = [r for r in prior_manifest["files"] if r["path"].startswith(name + "/upstream/")]
            if not matches:
                raise ValueError(f"Missing bundled sources: {name}")
            for record in matches:
                source = contained(prior, record["path"])
                if digest(source) != record["sha256"]:
                    raise ValueError(f"Bundled source was changed: {record['path']}")
                target = contained(args.output, record["path"])
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        else:
            cache = args.vcpkg / "downloads"
            cache.mkdir(parents=True, exist_ok=True)
            archive = cache / dependency["archive"]
            if not archive.is_file():
                with urllib.request.urlopen(dependency["url"], timeout=60) as response, archive.open("wb") as output:
                    shutil.copyfileobj(response, output)
            if digest(archive, "sha512") != sha:
                raise ValueError(f"Upstream source checksum failed: {name}")
            with tarfile.open(archive) as source:
                for member in source:
                    # Strip the archive's single versioned root directory.
                    parts = PurePosixPath(member.name).parts
                    if len(parts) < 2 or member.isdir():
                        continue
                    if not member.isfile():
                        raise ValueError(f"Unexpected source archive member: {member.name}")
                    target = contained(upstream, "/".join(parts[1:]))
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as data, target.open("wb") as output:
                        shutil.copyfileobj(data, output)
        shutil.copy2(share / "copyright", dest / "copyright.txt")
        for path in sorted(dest.rglob("*")):
            if path.is_file():
                records.append({"path": path.relative_to(args.output).as_posix(), "sha256": digest(path)})
        print(f"Verified dependency sources: {name} {dependency['version']}")
    (args.output / "manifest.json").write_text(json.dumps({"dependencies": spec["dependencies"], "files": records}, indent=2) + "\n", encoding="utf-8")
    (args.output / "README.md").write_text(
        "# Dependency source inputs\n\n"
        "These upstream sources match the source hashes recorded by the installed vcpkg packages.\n"
        "The exact vcpkg recipes and patches are in tools/dependency-recipes at the source root.\n"
        "Apply a recipe's patches in its declared order when building a library manually.\n"
        "The standard build uses the pinned vcpkg baseline in vcpkg.json; see the root README.md.\n"
        "Original license notices are preserved. No dependency plugin DLLs are included.\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
