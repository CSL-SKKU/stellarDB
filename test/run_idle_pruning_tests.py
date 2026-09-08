#!/usr/bin/env python3
"""Correctness checks for the official CLI's post-request pruning phase."""
import csv
import math
import subprocess
import tempfile
from pathlib import Path

from run_report_tests import completed

BINARY = Path(__file__).resolve().parents[1] / "main"


def run(base, name, options=(), items=32, requests=17, writes=False, db=None,
        success=True):
    target = db or base / (name + "-db")
    command = [str(BINARY), "-D", str(target), "-n", str(items), "-q", str(requests),
               "-P", "1048576", "-k", "256", "-m", "16384", "-i", "ascend",
               "-M", "20", "--util-gate"]
    command += (["-a", "ycsb", "-b", "ycsb_churn", "--churn-mix", "100/0/0"]
                if writes else ["-a", "latprobe", "-b", "latprobe"])
    result = subprocess.run(command + list(map(str, options)) + ["1", "1", "1"],
                            cwd=base, capture_output=True, text=True, timeout=30)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return target, result


def read(path):
    with path.open() as f:
        data = list(csv.DictReader(f))
    assert data and all(None not in row and None not in row.values() for row in data)
    return data


def phases(path, requests):
    data = read(path)
    busy = [row for row in data if row["phase"] == "busy"]
    idle = [row for row in data if row["phase"] == "idle"]
    assert data == busy + idle and busy and idle
    assert abs(completed(busy) - requests) < 0.001
    previous = float(busy[-1]["time_s"])
    elapsed = pruning = attempts = successes = 0
    for row in busy:
        assert all(value == "" for key, value in row.items() if key.startswith("idle_"))
    for row in idle:
        assert float(row["time_s"]) > previous
        previous = float(row["time_s"])
        assert all(value == "" for key, value in row.items()
                   if key not in ("time_s", "phase") and not key.startswith("idle_"))
        assert float(row["idle_elapsed_s"]) >= elapsed
        assert float(row["idle_pruning_time_s"]) >= pruning
        assert int(row["idle_pruning_attempts"]) >= attempts
        assert int(row["idle_pruning_successes"]) >= successes
        elapsed = float(row["idle_elapsed_s"])
        pruning = float(row["idle_pruning_time_s"])
        attempts = int(row["idle_pruning_attempts"])
        successes = int(row["idle_pruning_successes"])
        assert 0 <= pruning <= elapsed and 0 <= successes <= attempts
        if row["idle_status"] == "invalidating":
            assert attempts == successes == 0 and pruning == 0
        ratio = float(row["idle_stale_ratio"])
        assert math.isfinite(ratio) and 0 <= ratio <= 1
    return busy, idle


def main():
    with tempfile.TemporaryDirectory(prefix="stellar-idle-tests-") as tmp:
        base = Path(tmp)
        for value in ("-1", "nan", "inf", "abc", "", "31536001"):
            db, result = run(base, "invalid", ["--wait-for-pruning-s", value], success=False)
            assert "--wait-for-pruning-s requires seconds" in result.stderr
            assert not db.exists()
        db, result = run(base, "missing-target", ["--wait-for-pruning-s", "1"], success=False)
        assert "requires -p/--with-prune" in result.stderr and not db.exists()

        # Omitted and explicitly disabled options preserve the existing schema.
        schemas = []
        for name, options in (("off", []), ("zero", ["--wait-for-pruning-s", "0"])):
            path = base / (name + ".csv")
            _, result = run(base, name, [*options, "--report-out", path])
            data = read(path)
            assert len(data) == 1 and abs(completed(data) - 17) < 0.001
            assert "# Idle pruning:" not in result.stdout
            assert "phase" not in data[0]
            schemas.append(set(data[0]))
        assert schemas[0] == schemas[1]

        # Equality is success even for a zero target; no gratuitous prune/wait.
        path = base / "clean.csv"
        _, result = run(base, "clean", ["-p", "0", "--wait-for-pruning-s", "2",
                                       "--report-out", path])
        _, idle = phases(path, 17)
        assert len(idle) == 1
        assert idle[0]["idle_status"] == "target_reached"
        assert int(idle[0]["idle_pruning_attempts"]) == 0
        assert float(idle[0]["idle_stale_ratio"]) == 0
        assert float(idle[0]["idle_elapsed_s"]) < 2
        assert "# Idle invalidation: status=complete" in result.stdout

        # With no pending hints or copies, the first sweep callback is due
        # immediately, even when the entire sweep fits in one sample interval.
        path = base / "clean-timeseries.csv"
        run(base, "clean-timeseries", ["-p", "0", "--wait-for-pruning-s", "2",
                                       "--timeseries", "0.001", "--report-out", path])
        _, idle = phases(path, 17)
        assert any(row["idle_status"] == "invalidating" for row in idle)
        assert idle[-1]["idle_status"] == "target_reached"

        # Expiring before the sweep cannot report success merely because the
        # old estimate was already below target.
        path = base / "no-sweep-time.csv"
        _, result = run(base, "no-sweep-time", ["-p", "0", "--wait-for-pruning-s", "0.000000001",
                                               "--report-out", path])
        _, idle = phases(path, 17)
        assert idle[-1]["idle_status"] == "timeout"
        assert "# Idle invalidation: status=not_started" in result.stdout

        # Three nodes cannot form an ILI triple. Stale data persists, but the
        # fail-safe exits normally and emits the final (partial interval) row.
        path = base / "unreachable.csv"
        db, result = run(base, "unreachable", ["-p", "0", "--wait-for-pruning-s", "0.23",
                                               "--report-out", path, "--timeseries", "0.05"],
                         items=64, requests=17, writes=True)
        busy, idle = phases(path, 17)
        final = idle[-1]
        assert len(idle) >= 3 and all(row["idle_status"] != "timeout" for row in idle[:-1])
        assert final["idle_status"] == "timeout"
        assert 0.23 <= float(final["idle_elapsed_s"]) < 5
        assert float(final["idle_stale_ratio"]) > 0
        assert int(final["idle_pruning_successes"]) == 0
        assert int(final["idle_pruning_attempts"]) >= 2
        assert int(final["idle_last_prune_status"]) == 1
        assert all(int(row["pruning_successes"]) == 0 for row in busy)
        assert "status=timeout" in result.stdout

        # Idle results remain available with a selective config and with CSV off.
        config = base / "selected.config"
        config.write_text("all=false\nthroughput_rps=true\n")
        path = base / "selected.csv"
        run(base, "selected", ["-p", "0", "--wait-for-pruning-s", "0.03",
                               "-M", "600000", "--report-out", path, "--config-report", config],
            items=64, db=db)
        _, idle = phases(path, 17)
        assert len(idle) == 1 and idle[-1]["idle_status"] == "timeout"
        assert int(idle[-1]["idle_pruning_attempts"]) == 1
        _, result = run(base, "stdout", ["-p", "0", "--wait-for-pruning-s", "0.03",
                                         "--report-out", "none"], items=64, db=db)
        assert "# Idle pruning: status=timeout" in result.stdout
        assert "final_stale_ratio=" in result.stdout

        # Exercise actual pruning with the utilization gate closed to idle
        # clients; a reachable target stops before the generous fail-safe.
        path = base / "reclaim.csv"
        db, result = run(base, "reclaim", ["-p", "0.6", "--wait-for-pruning-s", "5",
                                          "--report-out", path, "--timeseries", "0.001"],
                         items=1024, requests=4096, writes=True)
        busy, idle = phases(path, 4096)
        final = idle[-1]
        assert final["idle_status"] == "target_reached", result.stdout
        assert float(final["idle_stale_ratio"]) <= 0.6
        # The handoff estimate can still be below target: pending hints and
        # the complete sweep raise it before the first idle target check.
        assert 0 <= float(final["idle_initial_stale_ratio"]) <= 1
        assert int(final["idle_pruning_successes"]) > 0
        assert float(final["idle_pruning_time_s"]) > 0
        assert float(final["idle_elapsed_s"]) < 5
        assert all(int(row["pruning_successes"]) == 0 for row in busy)
        assert int(final["idle_last_prune_status"]) == 0
        assert "# Idle invalidation: status=complete" in result.stdout

        # Reopen the pruned database: all authoritative keys remain indexed.
        path = base / "recovered.csv"
        run(base, "recovered", ["--report-out", path], items=1024, requests=1024, db=db)
        data = read(path)
        assert abs(completed(data) - 1024) < 0.001
        assert int(data[-1]["entries_live_normal"]) == 1024
        assert int(data[-1]["entries_live_tombstones"]) == 0
    print("PASS idle pruning: validation, disabled schema, busy accounting, equality, timeout, "
          "timeseries, selective/off reports, gate bypass, reclaim, recovery")


if __name__ == "__main__":
    main()
