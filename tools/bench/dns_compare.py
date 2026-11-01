"""Shared fixture config and summaries for host and real-kernel guest runs."""
import hashlib
import statistics


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def config(upstream="127.0.0.1", upstream_port=15399, port=13553,
           rules=1000, groups=1, disable_drop_aaaa=False):
    if rules < 1 or groups < 1:
        raise ValueError("at least one enabled rule and group required")
    text = f'''configVersion: 0.8.2
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: 0.0.0.0
      port: 8080
    skin: default
  dnsProxy:
    host:
      address: 0.0.0.0
      port: {port}
    upstream:
      address: {upstream}
      port: {upstream_port}
    disableRemap53: true
    disableFakePTR: false
    disableDropAAAA: {str(disable_drop_aaaa).lower()}
    maxIdleConns: 10
    maxConcurrent: 100
    timeout: 1s
  netfilter:
    iptables:
      chainPrefix: MT_
    ipset:
      tablePrefix: mt_
      additionalTTL: 1h0m0s
    disableIPv4: false
    disableIPv6: false
    startMarkTableIndex: 1298229097
  link: []
  showAllInterfaces: false
  logLevel: error
groups:
'''
    for group in range(groups):
        text += f'''  - id: aa{group+1:06x}
    name: Bench{group}
    color: '#ffffff'
    interface: blackhole
    enable: true
    rules:
'''
        for i in range(rules):
            text += f'''      - id: {0x10000000+group*rules+i:08x}
        name: r{i}
        type: namespace
        rule: 'd{i:06d}.bench.example.com'
        enable: true
'''
    return text + "subscriptions: []\n"


def validate_load(row):
    if row["ok"] <= 0 or any(row[key] for key in ("errors", "timeouts", "invalid")) or row["ok"] != row["sent"]:
        raise ValueError(f"load failed correctness gate: {row}")


def summary(rows):
    output = []
    for case in sorted({(r["proto"], r["cnames"], r["addresses"]) for r in rows}):
        item = dict(proto=case[0], cnames=case[1], addresses=case[2])
        for variant in ("baseline", "candidate"):
            selected = [r for r in rows if (r["proto"],r["cnames"],r["addresses"]) == case and r["variant"] == variant]
            if not selected:
                raise ValueError("unpaired comparison")
            item[variant] = {}
            for metric in ("rps", "p95_ms", "p99_ms", "cpu_ticks_per_query"):
                values = [r[metric] for r in selected]
                item[variant][metric] = dict(median=statistics.median(values), min=min(values), max=max(values))
        item["change_pct"] = {m: (item["candidate"][m]["median"] / v["median"] - 1) * 100
                              if v["median"] else None for m, v in item["baseline"].items()}
        output.append(item)
    return dict(measured_runs=len(rows), successful_queries=sum(r["ok"] for r in rows),
                errors=sum(r["errors"] for r in rows), timeouts=sum(r["timeouts"] for r in rows),
                invalid=sum(r["invalid"] for r in rows), workloads=output)
