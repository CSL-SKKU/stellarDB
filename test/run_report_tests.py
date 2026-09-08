#!/usr/bin/env python3
"""Correctness checks for the official CLI's report and completion contract."""
import csv
import math
import subprocess
import tempfile
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "main"


def run(base, name, options=(), api="ycsb", bench="ycsb_c_uniform", requests=17,
        db=None, success=True):
    target = db or base / (name + "-db")
    result = subprocess.run(
        [str(BINARY), "-D", str(target), "-a", api, "-b", bench,
         "-n", "32", "-q", str(requests), "-P", "1048576", "-k", "256",
         "-m", "16384", "-i", "ascend", *map(str, options), "1", "1", "1"],
        cwd=base, capture_output=True, text=True, timeout=60,
    )
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return target


def rows(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def completed(data):
    previous = 0
    count = 0
    for row in data:
        now = float(row["time_s"])
        assert now > previous
        count += (now - previous) * float(row["throughput_rps"])
        previous = now
        for key in ("latency_avg_ms", "latency_p99_ms"):
            if row.get(key):
                assert math.isfinite(float(row[key])) and float(row[key]) >= 0
    return count


def partition(row, normal, tombstones):
    assert int(row["entries_total"]) == normal + tombstones
    assert int(row["entries_stale"]) == 0
    assert int(row["entries_live_normal"]) == normal
    assert int(row["entries_live_tombstones"]) == tombstones


def main():
    with tempfile.TemporaryDirectory(prefix="stellar-report-") as tmp:
        base = Path(tmp)
        report = base / "aggregate.csv"
        run(base, "aggregate", ["--report-out", report, "-R"])
        data = rows(report)
        assert len(data) == 1
        assert abs(completed(data) - 17) < 0.001  # excludes all 32 initial inserts
        partition(data[0], 32, 0)
        assert int(data[0]["nodes"]) == 1
        assert int(data[0]["rebalance_attempts"]) == 0  # setup excluded
        for field in ("upward_hops_avg", "downward_hops_avg"):
            assert float(data[0][field]) == 0  # root is also the leaf

        # Client-level scans and RMWs, including work submitted after read completion.
        for bench in ("ycsb_e_uniform", "ycsb_f_uniform", "ycsb_d_latest"):
            report = base / (bench + ".csv")
            run(base, bench, ["--report-out", report], bench=bench, requests=43)
            assert abs(completed(rows(report)) - 43) < 0.001, bench

        report = base / "delete.csv"
        db = run(base, "delete", ["--report-out", report, "--churn-mix", "0/0/100"],
                 bench="ycsb_churn", requests=32)
        partition(rows(report)[0], 0, 32)
        assert rows(report)[0]["upward_hops_avg"] == ""  # writes are excluded
        assert rows(report)[0]["downward_hops_avg"] == ""
        report = base / "recovery.csv"
        run(base, "recovery", ["--report-out", report], api="latprobe", bench="latprobe", db=db)
        partition(rows(report)[0], 0, 32)
        assert abs(completed(rows(report)) - 17) < 0.001  # includes read misses
        assert float(rows(report)[0]["upward_hops_avg"]) == 0
        assert float(rows(report)[0]["downward_hops_avg"]) == 0
        report = base / "resurrection.csv"
        run(base, "resurrection", ["--report-out", report, "--churn-mix", "100/0/0"],
            bench="ycsb_churn", requests=17, db=db)
        row = rows(report)[0]
        assert 0 < int(row["entries_live_normal"]) <= 17
        assert int(row["entries_live_tombstones"]) + int(row["entries_live_normal"]) == 32

        config = base / "selected.config"
        config.write_text("# Only requested fields\nall=false\nthroughput_rps=1\nlatency_p99_ms=true\n")
        report = base / "series.csv"
        run(base, "series", ["--report-out", report, "--config-report", config,
                            "--timeseries", "0.001"], api="latprobe", bench="latprobe", requests=2000)
        data = rows(report)
        assert len(data) > 1
        assert set(data[0]) == {"time_s", "throughput_rps", "latency_p99_ms"}
        assert abs(completed(data) - 2000) < 0.001

        # Either hop field works alone, without throughput/latency collectors.
        for field in ("upward_hops_avg", "downward_hops_avg"):
            config.write_text(f"all=false\n{field}=true\n")
            report = base / (field + ".csv")
            run(base, field, ["--report-out", report, "--config-report", config])
            row = rows(report)[0]
            assert set(row) == {"time_s", field}
            assert float(row[field]) == 0

        config.write_text("all=false\nupward_hops_avg=true\ndownward_hops_avg=true\n")
        report = base / "hop-series.csv"
        run(base, "hop-series", ["--report-out", report, "--config-report", config,
                                 "--timeseries", "0.001"],
            api="latprobe", bench="latprobe", requests=2000)
        data = rows(report)
        assert len(data) > 1
        assert any(row["upward_hops_avg"] for row in data)
        for row in data:
            assert set(row) == {"time_s", "upward_hops_avg", "downward_hops_avg"}
            assert bool(row["upward_hops_avg"]) == bool(row["downward_hops_avg"])
            if row["upward_hops_avg"]:
                assert float(row["upward_hops_avg"]) == 0
                assert float(row["downward_hops_avg"]) == 0

        config.write_text("unknown_metric=true\n")
        run(base, "bad-config", ["--report-out", base / "bad.csv", "--config-report", config], success=False)
        assert not (base / "bad-config-db").exists()
        for interval in ("0", "-1", "nan", "inf", "abc"):
            run(base, "bad-interval", ["--timeseries", interval], success=False)
        run(base, "off", ["--report-out", "none", "--config-report", config, "--timeseries", "0.001"])
        run(base, "off-default", ["--config-report", config])
        assert not (base / "none").exists()
        before = report.read_bytes()
        run(base, "existing-output", ["--report-out", report], success=False)
        assert report.read_bytes() == before
    print("PASS report fields, aggregate/interval completion, scans, RMW, load exclusion, tombstones, recovery, off, validation")


if __name__ == "__main__":
    main()
