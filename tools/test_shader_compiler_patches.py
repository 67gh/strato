#!/usr/bin/env python3
"""Exercise the real CMake shader patch hook without Android or network access.
Requires Python 3, git, CMake >= 3.22 and an initialized shader-compiler submodule.
Usage: python tools/test_shader_compiler_patches.py --cmake /path/to/cmake
All source resets and conflict injections happen only in a temporary clone.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILES = ["shader_info.h", "backend/spirv/spirv_emit_context.h",
         "backend/spirv/spirv_emit_context.cpp", "backend/spirv/emit_spirv_image.cpp",
         "backend/spirv/emit_spirv_image_atomic.cpp"]
PINNED = "8717c8f6d7deab58032e042b027ed84838833824"


def run(*args, cwd=None, ok=True):
    result = subprocess.run([str(x) for x in args], cwd=cwd, capture_output=True, text=True)
    if ok and result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result


def snapshot(source):
    return {name: (source / name).read_bytes() for name in FILES}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    helper = ROOT / "app/cmake/ApplyShaderCompilerPatches.cmake"
    origin = ROOT / "app/libraries/shader-compiler"
    revision = run("git", "-C", origin, "rev-parse", "HEAD").stdout.strip()
    if revision != PINNED:
        raise RuntimeError("This regression test expects the pinned shader-compiler revision: " + PINNED)
    with tempfile.TemporaryDirectory(prefix="strato shader patches ") as folder:
        temp = Path(folder)
        parent = temp / "parent"
        run("git", "init", "-q", parent)
        run("git", "-c", "protocol.file.allow=always", "submodule", "add", "-q",
            str(origin), "compiler", cwd=parent)
        run("git", "-c", "user.name=Regression Test", "-c", "user.email=test@example.invalid",
            "commit", "-qm", "Temporary submodule fixture", cwd=parent)
        source = parent / "compiler"
        baseline = snapshot(source)
        assert b"ImageNumericType" not in baseline["shader_info.h"]

        def apply(target=source, success=True):
            result = run(args.cmake, "-DSTRATO_SHADER_COMPILER_DIR=" + str(target),
                         "-P", helper, ok=False)
            if success and result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if not success:
                assert result.returncode != 0, "A conflict or missing source must fail"
                assert "Cannot apply shader compiler patch" in result.stdout + result.stderr
            return result

        first = apply()
        expected = snapshot(source)
        assert all(expected[name] != baseline[name] for name in FILES)
        assert b"enum class ImageNumericType" in expected["shader_info.h"]
        assert first.stdout.count("Applied shader compiler patch:") == 5
        print("PASS: clean pinned submodule; all five compiler files patched")
        apply()
        assert snapshot(source) == expected
        print("PASS: already-applied patch is idempotent")
        upgrades = sorted((ROOT / "app/patches/shader-compiler/upgrades").glob("*.patch"))
        assert len(upgrades)==4
        for upgrade in upgrades:
            run("git", "apply", "--reverse", upgrade, cwd=source)
        assert snapshot(source)!=expected
        # This is the exact prior release, not a clean upstream submodule.
        migrated = apply()
        assert snapshot(source)==expected
        assert migrated.stdout.count("Applied shader compiler patch:")==4
        apply()
        assert snapshot(source)==expected
        print("PASS: previous release migrates without submodule reset; migration is idempotent")


        run("git", "restore", "--", FILES[0], FILES[3], cwd=source)
        apply()
        assert snapshot(source) == expected
        print("PASS: partial previous overlay repaired")

        # Reproduce the project's actual CI command against the temporary submodule.
        run("git", "submodule", "update", "--init", "--recursive", "--force", cwd=parent)
        assert snapshot(source) == baseline
        apply()
        assert snapshot(source) == expected
        print("PASS: recursive submodule update --force followed by CMake restores all fixes")

        extra = b"\n// Unrelated local edit preserved by the integration test.\n"
        path = source / FILES[0]
        path.write_bytes(path.read_bytes() + extra)
        apply()
        assert path.read_bytes() == expected[FILES[0]] + extra
        print("PASS: unrelated local edits preserved")

        run("git", "reset", "--hard", "-q", "HEAD", cwd=source)
        conflict = source / FILES[-1]
        conflict.write_text("// Conflicting user version. Do not overwrite.\n")
        before = snapshot(source)
        apply(success=False)
        assert snapshot(source) == before
        print("PASS: late conflicting file stops preflight without changing any file")

        # An exported archive can be inside another Git repo, with no .git of its own.
        exported = parent / "exported sources"
        for name, data in baseline.items():
            path = exported / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        apply(exported)
        assert snapshot(exported) == expected
        apply(exported)
        assert snapshot(exported) == expected
        print("PASS: exported sources inside parent Git repo; no silently skipped patches")

        missing = temp / "missing compiler"
        missing.mkdir()
        apply(missing, success=False)
        assert not list(missing.iterdir())
        print("PASS: missing submodule produces explicit failure")
        if shutil.which("ninja"):
            run("git", "reset", "--hard", "-q", "HEAD", cwd=source)
            mini = temp / "cmake project"
            mini.mkdir()
            (mini / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(PatchTest NONE)\n'
                + 'set(STRATO_SHADER_COMPILER_DIR "' + source.as_posix() + '")\n'
                + 'include("' + helper.as_posix() + '")\n')
            build = temp / "build"
            run(args.cmake, "-S", mini, "-B", build, "-G", "Ninja")
            assert snapshot(source) == expected
            run("git", "submodule", "update", "--init", "--recursive", "--force", cwd=parent)
            assert snapshot(source) == baseline
            run(args.cmake, "--build", build)
            assert snapshot(source) == expected
            again = run(args.cmake, "--build", build)
            assert "Applied shader compiler patch:" not in again.stdout
            print("PASS: cached Ninja build reruns CMake after submodule reset; next build stable")
        else:
            print("SKIP: Ninja unavailable; cached build reconfiguration not tested")
        print("All patch integration checks passed. No Android APK or game execution tested.")


if __name__ == "__main__":
    main()
