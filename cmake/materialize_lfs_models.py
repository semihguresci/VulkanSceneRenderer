"""Replace IFC/IFCX Git LFS archive pointers with verified model payloads."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
from pathlib import Path
import re
import tempfile
import urllib.parse
import urllib.request


def read_pointer(path):
    if path.stat().st_size > 1024:
        return None
    data = path.read_bytes()
    if not data.startswith(b"version https://git-lfs.github.com/spec/v1\n"):
        # Accept Windows line endings as well.
        if not data.startswith(b"version https://git-lfs.github.com/spec/v1\r\n"):
            return None
    oid = re.search(rb"^oid sha256:([0-9a-f]{64})\r?$", data, re.M)
    size = re.search(rb"^size ([0-9]+)\r?$", data, re.M)
    if not oid or not size:
        raise ValueError(f"Invalid Git LFS pointer: {path}")
    return oid[1].decode("ascii"), int(size[1])


def materialize(path, root, repository, revision):
    pointer = read_pointer(path)
    if pointer is None:
        return False
    oid, size = pointer
    relative = path.resolve().relative_to(root.resolve()).as_posix()
    url = (f"https://media.githubusercontent.com/media/{repository}/{revision}/"
           + urllib.parse.quote(relative, safe="/"))
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".",
                                     suffix=".lfs-download", delete=False) as file:
        temporary = Path(file.name)
    digest, received = hashlib.sha256(), 0
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "VulkanSceneRenderer-assets"})
        with urllib.request.urlopen(request, timeout=120) as source, temporary.open("wb") as target:
            while chunk := source.read(1024 * 1024):
                received += len(chunk)
                if received > size:
                    raise ValueError(f"LFS payload exceeds expected size: {relative}")
                digest.update(chunk)
                target.write(chunk)
        if received != size or digest.hexdigest() != oid:
            raise ValueError(f"LFS payload size/hash mismatch: {relative}")
        temporary.replace(path)
        return True
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    stamp = (root / ".fetched").read_text(encoding="utf-8")
    revision = re.search(r"downloaded commit ([0-9a-f]{40})", stamp)
    if not revision or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        raise ValueError("Expected a stamped Git revision and owner/repository")
    pointers = [path for path in root.rglob("*") if path.is_file()
                and path.suffix.lower() in {".ifc", ".ifcx"} and read_pointer(path)]
    errors = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        tasks = {pool.submit(materialize, path, root, args.repository, revision[1]): path
                 for path in pointers}
        for task in as_completed(tasks):
            path = tasks[task]
            try:
                task.result()
                print(f"Materialized IFC model: {path.relative_to(root)}", flush=True)
            except Exception as error:
                errors.append(str(error))
                print(f"Failed to materialize {path.relative_to(root)}: {error}", flush=True)
    print(f"IFC LFS payloads: {len(pointers) - len(errors)} resolved, {len(errors)} failed", flush=True)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
