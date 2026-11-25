#!/usr/bin/env python3
"""Package Blender Dock for release.

    python tools/package.py            -> dist/blender_dock-<version>.zip  (addons/blender_dock/...)
    python tools/package.py --branch   -> also commits the same tree to the local `asset-library`
                                          branch (point the Godot Asset Library at that commit)

Run `scons` first: both DLLs must be built. Nothing is pushed.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "demo", "addons", "blender_dock")
REQUIRED = ["blender_dock.gdextension", "LICENSE.md", "VERSION",
            "bin/blender_dock.windows.editor.x86_64.dll", "bin/blender_focus_hook.dll",
            "blender/godot_link.py", "blender/LICENSE-GPL-3.0.txt"]
SKIP_EXT = {".lib", ".exp", ".pdb", ".ilk", ".pyc", ".import"}
EXTRA = {"README.md": "addons/blender_dock/README.md", "CHANGELOG.md": "addons/blender_dock/CHANGELOG.md"}
# (kept inside the addon folder so installing does not drop files into the project root)


def addon_files():
    out = []
    for dirpath, dirnames, filenames in os.walk(ADDON):
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        for f in filenames:
            if f.startswith("~") or os.path.splitext(f)[1].lower() in SKIP_EXT:
                continue
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, ADDON).replace(os.sep, "/")
            out.append((full, "addons/blender_dock/" + rel))
    return sorted(out, key=lambda x: x[1])


def git(*args, env=None, inp=None):
    r = subprocess.run(["git", *args], cwd=ROOT, env=env, input=inp, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{r.stderr}")
    return r.stdout.strip()


def commit_branch(files, version, branch="asset-library"):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=os.path.join(tmp, "index"))
        for src, dst in files:
            blob = git("hash-object", "-w", "--no-filters", src)
            git("update-index", "--add", "--cacheinfo", f"100644,{blob},{dst}", env=env)
        tree = git("write-tree", env=env)
    parent = subprocess.run(["git", "rev-parse", "-q", "--verify", f"refs/heads/{branch}"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip()
    if parent and git("rev-parse", f"{parent}^{{tree}}") == tree:
        print(f"{branch} already has this exact tree: {parent}")
        return parent
    src_head = git("rev-parse", "--short", "HEAD")
    args = ["commit-tree", tree, "-m", f"Blender Dock {version} (package of {src_head})"]
    if parent:
        args += ["-p", parent]
    commit = git(*args)
    git("update-ref", f"refs/heads/{branch}", commit)
    print(f"{branch} -> {commit}")
    return commit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--branch", action="store_true", help="also commit to the local asset-library branch")
    a = ap.parse_args()

    missing = [r for r in REQUIRED if not os.path.isfile(os.path.join(ADDON, r))]
    if missing:
        sys.exit("missing (run scons?): " + ", ".join(missing))
    version = open(os.path.join(ADDON, "VERSION"), encoding="utf-8").read().strip()

    files = addon_files() + [(os.path.join(ROOT, k), v) for k, v in EXTRA.items()]
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    zpath = os.path.join(ROOT, "dist", f"blender_dock-{version}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for src, dst in files:
            z.write(src, dst)
    print(f"{zpath}  ({len(files)} files)")
    for _, dst in files:
        print("  ", dst)
    if a.branch:
        commit_branch(files, version)


if __name__ == "__main__":
    main()
