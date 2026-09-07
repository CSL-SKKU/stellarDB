#!/usr/bin/env python3
"""Exercise the real CLI, storage paths, and recovery in temporary directories.

Run `make` first, then `python3 test/run_directory_tests.py`. No mount namespace
is needed. Permission checks require an unprivileged user.
"""

import os
from pathlib import Path
import subprocess
import tempfile


BINARY = Path(__file__).resolve().parents[1] / "main"
SMALL_RUN = ["-a", "latprobe", "-b", "latprobe", "-n", "16", "-q", "16",
             "-P", "1048576", "-k", "256", "-m", "16384", "-i", "ascend",
             "1", "1", "1"]


def run(cwd, options, success=True, benchmark=True):
    result = subprocess.run(
        [str(BINARY), *options, *(SMALL_RUN if benchmark else [])],
        cwd=cwd, capture_output=True, text=True, timeout=60,
    )
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return result


def contents(directory):
    return {p.name: p.read_bytes() for p in directory.iterdir() if p.is_file()}


def main():
    os.umask(0o027)
    with tempfile.TemporaryDirectory(prefix="stellar-directory-") as tmp:
        base = Path(tmp)
        help_result = run(base, ["-D", "unused", "--help"], benchmark=False)
        assert "--directory" in help_result.stdout
        assert "/scratch0/kvell" in help_result.stdout
        assert not (base / "unused").exists()
        run(base, ["-D"], success=False, benchmark=False)
        assert "nonempty path" in run(base, ["--directory="], False).stderr

        relative = "data sets/nested/db"
        db = base / relative
        created = run(base, ["-D", relative + "/"])
        assert created.stdout.count("Created database directory:") == 3
        assert db.stat().st_mode & 0o777 == 0o750
        before = contents(db)
        assert "ROOT" in before and any(n.startswith("slab-") for n in before)

        reopened = run(base, ["--directory=" + str(db)])
        assert "Created database directory:" not in reopened.stdout
        assert "Init found 16 elements" in reopened.stdout
        assert contents(db) == before
        print("PASS short/long options, nested/relative/spaced paths, umask, recovery")

        long_db = base.joinpath(*[str(i) + "x" * 130 for i in range(4)])
        assert len(str(long_db)) > 512
        run(base, ["--directory", str(long_db)])
        orphan = long_db / "slab-9999999"
        orphan.write_bytes(b"incomplete slab")
        recovered = run(base, ["-D", str(long_db)])
        assert "Init found 16 elements" in recovered.stdout
        assert not orphan.exists()
        assert contents(db) == before
        print("PASS long paths and recovery cleanup in the selected directory")

        regular_file = base / "not-a-directory"
        regular_file.write_text("preserve me")
        for path in (regular_file, regular_file / "child"):
            failed = run(base, ["-D", str(path)], False)
            assert "Not a directory" in failed.stderr
        assert regular_file.read_text() == "preserve me"

        if os.geteuid() == 0:
            print("SKIP permission-denied cases (run as an unprivileged user)")
        else:
            denied = base / "denied"
            denied.mkdir()
            try:
                # Missing write, read, or search permission, respectively.
                for mode in (0o500, 0o300, 0o600):
                    denied.chmod(mode)
                    failed = run(base, ["-D", str(denied)], False)
                    assert "Permission denied" in failed.stderr
                    assert "Initializing random" not in failed.stdout
                denied.chmod(0o500)
                failed = run(base, ["-D", str(denied / "child")], False)
                assert "Permission denied" in failed.stderr
            finally:
                denied.chmod(0o700)
            assert not list(denied.iterdir())

            for name in ("ROOT", next(n for n in before if n.startswith("slab-"))):
                file = db / name
                mode = file.stat().st_mode & 0o777
                try:
                    file.chmod(0)
                    failed = run(base, ["-D", str(db)], False)
                    assert "Permission denied" in failed.stderr
                    assert file.exists()
                finally:
                    file.chmod(mode)
            assert contents(db) == before
            print("PASS permission errors, including unreadable ROOT/slabs without deletion")

        print("Directory tests passed")


if __name__ == "__main__":
    main()
