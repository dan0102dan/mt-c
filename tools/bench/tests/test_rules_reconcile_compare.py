#!/usr/bin/env python3
"""Pure tests of timing plans and result gates; never execute a benchmark."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "run_rules_reconcile_compare.py"
SPEC = importlib.util.spec_from_file_location("rules_compare", SCRIPT)
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


class RulesCompareTests(unittest.TestCase):
    def test_adjacent_manifest_rejects_changed_sources_and_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "fixture.c"; source.write_bytes(b"validated source")
            binaries = {}
            for variant in ("baseline", "candidate"):
                path = root / variant; path.write_bytes(variant.encode())
                binaries[variant] = dict(file=variant, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            manifest = dict(schema=1, baseline_commit=compare.BASELINE_COMMIT,
                            source_sha256={str(source): hashlib.sha256(source.read_bytes()).hexdigest()},
                            binaries=binaries)
            (root / "benchmark-manifest.json").write_text(json.dumps(manifest))
            got, paths = compare.validate_build_manifest(root)
            self.assertEqual(got, manifest)
            self.assertEqual(paths, {variant: root / variant for variant in binaries})
            source.write_bytes(b"unvalidated source")
            with self.assertRaisesRegex(ValueError, "source changed"):
                compare.validate_build_manifest(root)
            source.write_bytes(b"validated source")
            (root / "candidate").write_bytes(b"replaced executable")
            with self.assertRaisesRegex(ValueError, "binary changed"):
                compare.validate_build_manifest(root)

    def test_iteration_plan_targets_candidate_without_explosive_baseline(self):
        plan = compare.choose_iterations(1000, 1000, 5_000_000, 500_000)
        self.assertEqual(plan["iterations"], 500)
        self.assertGreaterEqual(plan["estimated_candidate_ns"], 250_000_000)
        self.assertLessEqual(plan["estimated_baseline_ns"], 5_000_000_000)
        capped = compare.choose_iterations(10000, 10000, 1_000_000_000, 1_000_000)
        self.assertEqual(capped["iterations"], 5)
        self.assertIn("budget", capped["reason"])
        largest = compare.choose_iterations(100000, 10000, 100_000_000, 100_000)
        self.assertEqual(largest["iterations"], 1)
        empty = compare.choose_iterations(100000, 0, 100_000, 100_000)
        self.assertEqual(empty["iterations"], 2500)
        self.assertEqual(compare.choose_iterations(1000, 1000, 1, 1)["iterations"], 4096)
        for baseline, candidate in ((0, 1), (1, 0), (-1, 1), (1, -1)):
            with self.subTest(baseline=baseline, candidate=candidate), self.assertRaises(ValueError):
                compare.choose_iterations(1000, 1000, baseline, candidate)

    def result(self, domains=1000, iterations=2):
        return dict(fixture="namespace_all_hit_v4", rules=1000, domains=domains, iterations=iterations,
                    validated_ipv4_entries=domains * iterations,
                    checksum=domains * (domains + 1) // 2 if iterations % 2 else 0,
                    build_verify_clear_cpu_ns=200_000_000, peak_rss_kib=4096)

    def test_correctness_gate_rejects_noop_partial_and_malformed_results(self):
        for domains, iterations in ((1000, 2), (1000, 3), (0, 2)):
            compare.validate_result(self.result(domains, iterations), 1000, domains, iterations)
        changes = dict(fixture="other", rules=999, domains=999, iterations=1,
                       validated_ipv4_entries=0, checksum=1, build_verify_clear_cpu_ns=0,
                       peak_rss_kib=-1)
        for key, value in changes.items():
            row = self.result(); row[key] = value
            with self.subTest(field=key), self.assertRaises(ValueError):
                compare.validate_result(row, 1000, 1000, 2)
        for key in self.result():
            row = self.result(); del row[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                compare.validate_result(row, 1000, 1000, 2)
        row = self.result(); row["iterations"] = True
        with self.assertRaises(ValueError): compare.validate_result(row, 1000, 1000, 1)

    def paired_rows(self):
        rows = []
        for rep in range(5):
            order = ("baseline", "candidate") if rep % 2 == 0 else ("candidate", "baseline")
            for variant in order:
                row = self.result()
                row.update(variant=variant, rep=rep, sequence=len(rows) + 1,
                           build_verify_clear_cpu_ns=(rep + 1) * (2_000_000 if variant == "baseline" else 1_000_000),
                           peak_rss_kib=(1000 + rep * 10 if variant == "baseline" else 1200 + rep * 20))
                rows.append(row)
        return rows

    def test_summary_keeps_all_pairs_and_reports_cpu_per_iteration(self):
        rows = self.paired_rows()
        plan = dict(rules=1000, domains=1000, iterations=2)
        result = compare.summarize(rows, [plan])
        self.assertEqual(result["measured_runs"], 10)
        case = result["cases"][0]
        self.assertEqual(case["variants"]["baseline"]["cpu_ms_per_reconcile"], dict(median=3.0, min=1.0, max=5.0))
        self.assertEqual(case["variants"]["candidate"]["cpu_ms_per_reconcile"], dict(median=1.5, min=0.5, max=2.5))
        self.assertEqual(case["baseline_over_candidate_median_cpu"], 2.0)
        self.assertEqual(case["candidate_minus_baseline_median_peak_rss_kib"], 220)
        self.assertEqual(case["variants"]["baseline"]["process_peak_rss_kib"], dict(median=1020, min=1000, max=1040))
        self.assertEqual(len(case["pairs"]), 5)
        self.assertEqual(case["pairs"][1]["order"], ["candidate", "baseline"])
        self.assertEqual([p["baseline_over_candidate_cpu"] for p in case["pairs"]], [2.0] * 5)

    def test_summary_refuses_missing_duplicate_or_different_length_runs(self):
        rows = self.paired_rows()
        variants = [rows[:-1], rows[:-1] + [rows[0]]]
        changed = copy.deepcopy(rows); changed[0]["iterations"] = 3; variants.append(changed)
        for bad in variants:
            with self.subTest(rows=bad), self.assertRaises(ValueError):
                compare.summarize(bad, [dict(rules=1000, domains=1000, iterations=2)])


if __name__ == "__main__":
    unittest.main()
