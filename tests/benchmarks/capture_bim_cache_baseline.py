"""Capture provider headers without modifying the active source checkout."""
import argparse
from pathlib import Path
import subprocess


HEADERS = (
    "Container/scene/SceneProvider.h",
    "Container/scene/MeshSceneProviderBuilder.h",
    "Container/scene/MeshSceneProviderAssetAdapter.h",
    "Container/renderer/scene/SceneProviderSynchronizer.h",
    "Container/renderer/core/RenderExtraction.h",
)


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="7fd895a8209ee718af85a9ea4cd0289a1c28a17a")
    parser.add_argument("--output", type=Path,
                        default=repo / "out/bim-frame-cache-validation/before-include")
    args = parser.parse_args()
    commit = subprocess.check_output(
        ["git", "rev-parse", "--verify", "--end-of-options", f"{args.ref}^{{commit}}"],
        cwd=repo, text=True).strip()
    # Read all inputs before publishing any files; preserve their exact bytes.
    headers = {name: subprocess.check_output(
        ["git", "show", f"{commit}:include/{name}"], cwd=repo) for name in HEADERS}
    for name, content in headers.items():
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    (args.output / "source-commit.txt").write_text(commit + "\n", encoding="utf-8")
    print(f"Captured {len(headers)} provider headers from {commit} to {args.output}")


if __name__ == "__main__":
    main()
