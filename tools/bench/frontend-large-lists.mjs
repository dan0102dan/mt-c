#!/usr/bin/env node
// Reproducible frontend diagnostics. Install src/frontend dependencies and the
// chosen Playwright browser, then start the same Vite/build variant before and
// after a change. This uses API fixtures, not a router or production data.
// node tools/bench/frontend-large-lists.mjs --url http://localhost:5173 --out /tmp/ui.json
// Optional: --config /path/to/config.mtrickle
// Without --config, use .build/test-data/config-100000-rules.mtrickle if present;
// otherwise generate the same deterministic 100 groups × 1,000 rules in memory.
// --generate forces that generated fixture even when the default file exists.
// Optional: --browser webkit --executable /path/to/webkit-launcher
import { createRequire } from "node:module";
import { readFile, writeFile } from "node:fs/promises";
import { basename, resolve } from "node:path";
import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("../../", import.meta.url));
const require = createRequire(`${root}src/frontend/package.json`);
const { chromium, webkit } = require("@playwright/test");
const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i < 0 ? fallback : process.argv[i + 1];
};
const url = arg("--url", "http://localhost:5173");
const out = arg("--out", "/tmp/mt-frontend-bench.json");
const engine = arg("--browser", "chromium");
if (!["chromium", "webkit"].includes(engine))
  throw new Error("--browser must be chromium or webkit");
const executablePath = arg("--executable", undefined);
const configArgument = arg("--config", undefined);
const generate = process.argv.includes("--generate");
if (configArgument && generate)
  throw new Error("Use either --config or --generate");
if (process.argv.includes("--config") && !configArgument)
  throw new Error("--config requires a file path");
const configPath = configArgument
  ? resolve(configArgument)
  : `${root}.build/test-data/config-100000-rules.mtrickle`;
function generateConfig() {
  return {
    format: "mt-c-config",
    schemaVersion: 1,
    exportedAt: "2026-11-13T18:25:12.443Z",
    source: { application: "mt-c" },
    groups: Array.from({ length: 100 }, (_, groupIndex) => ({
      id: (0x10000000 + groupIndex).toString(16),
      name: `Load test ${String(groupIndex + 1).padStart(3, "0")}`,
      color: "#42bdf9",
      interface: "blackhole",
      enable: true,
      priority: 300,
      rules: Array.from({ length: 1000 }, (_, ruleIndex) => {
        const index = groupIndex * 1000 + ruleIndex;
        return {
          id: (0x20000000 + index).toString(16),
          name: "",
          rule: `rule${String(index + 1).padStart(6, "0")}.example.test`,
          type: "namespace",
          enable: true,
        };
      }),
    })),
  };
}
let source;
let configSource;
if (!generate) {
  try {
    source = JSON.parse(await readFile(configPath, "utf8"));
    configSource = { kind: "file", path: configPath };
  } catch (error) {
    if (configArgument || error.code !== "ENOENT") throw error;
  }
}
if (source === undefined) {
  source = generateConfig();
  configSource = { kind: "generated", groups: 100, rulesPerGroup: 1000 };
}
if (!source || !Array.isArray(source.groups) || !source.groups.length)
  throw new Error("Benchmark configuration must contain at least one group");
const importFile =
  configSource.kind === "file"
    ? configPath
    : {
        name: basename(configPath),
        mimeType: "application/json",
        buffer: Buffer.from(JSON.stringify(source)),
      };
const current = structuredClone(source.groups);
current[0].name = "Existing changed group";
const scrollGroupId = current[Math.min(4, current.length - 1)].id;
const browser = await (engine === "webkit" ? webkit : chromium).launch({
  headless: true,
  ...(executablePath ? { executablePath } : {}),
});
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
await page.addInitScript(() => {
  const metrics = {
    recording: false,
    gaps: [],
    tasks: [],
    peakRows: 0,
    newRows: 0,
    epoch: 0,
    blank: 0,
    samples: 0,
    longTasksSupported:
      globalThis.PerformanceObserver?.supportedEntryTypes?.includes(
        "longtask",
      ) ?? false,
  };
  globalThis.__uiMetrics = metrics;
  let last;
  let epoch = -1;
  let seenRows = new WeakSet();
  function frame(time) {
    if (metrics.recording) {
      if (last) metrics.gaps.push(time - last);
      if (epoch !== metrics.epoch) {
        seenRows = new WeakSet();
        epoch = metrics.epoch;
      }
      const rows = document.querySelectorAll(".rule");
      metrics.peakRows = Math.max(metrics.peakRows, rows.length);
      for (const row of rows)
        if (!seenRows.has(row)) {
          seenRows.add(row);
          metrics.newRows++;
        }
      const area = [...document.querySelectorAll(".group-rules")].find((el) => {
        const box = el.getBoundingClientRect();
        return box.top < innerHeight / 2 && box.bottom > innerHeight / 2;
      });
      const box = area?.getBoundingClientRect();
      if (box && box.top < innerHeight / 2 && box.bottom > innerHeight / 2) {
        metrics.samples++;
        if (
          !document
            .elementFromPoint(innerWidth / 2, innerHeight / 2)
            ?.closest(".rule")
        )
          metrics.blank++;
      }
    }
    last = time;
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  try {
    if (metrics.longTasksSupported)
      new PerformanceObserver((list) => {
        if (metrics.recording)
          metrics.tasks.push(...list.getEntries().map((x) => x.duration));
      }).observe({ type: "longtask" });
  } catch {
    metrics.longTasksSupported = false;
  }
});
await page.route("**/api/v1/**", async (route) => {
  const path = new URL(route.request().url()).pathname.replace("/api/v1", "");
  const fixtures = {
    "/auth": { enabled: false },
    "/groups": { groups: current },
    "/subscriptions": { subscriptions: [] },
    "/profiles": { profiles: [] },
    "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
    "/system/update/status": { stage: "idle" },
    "/system/update": { installed_version: "test", can_install: false },
    "/system/config/snapshot": {
      revision: "a".repeat(64),
      config: {
        configVersion: "test",
        app: {},
        groups: current,
        profiles: [],
        subscriptions: [],
      },
    },
  };
  await route.fulfill({ json: fixtures[path] ?? {} });
});
await page.route("https://api.github.com/**", (route) =>
  route.fulfill({ json: [] }),
);
const session =
  engine === "chromium" ? await page.context().newCDPSession(page) : undefined;
await session?.send("Profiler.enable");
const report = {
  url,
  engine,
  browser: browser.version(),
  cpuProfilingSupported: Boolean(session),
  configSource,
  groupCount: source.groups.length,
  ruleCount: source.groups.reduce(
    (count, group) => count + group.rules.length,
    0,
  ),
  phases: {},
  errors,
};
async function phase(name, action) {
  await page.evaluate(() =>
    Object.assign(globalThis.__uiMetrics, {
      recording: true,
      gaps: [],
      tasks: [],
      peakRows: 0,
      newRows: 0,
      epoch: globalThis.__uiMetrics.epoch + 1,
      blank: 0,
      samples: 0,
    }),
  );
  await session?.send("Profiler.start");
  const start = performance.now();
  await action();
  await page.waitForTimeout(120);
  const elapsedMs = performance.now() - start;
  const profile = session
    ? (await session.send("Profiler.stop")).profile
    : undefined;
  if (profile)
    await writeFile(`${out}.${name}.cpuprofile`, JSON.stringify(profile));
  const metrics = await page.evaluate(() => {
    globalThis.__uiMetrics.recording = false;
    return globalThis.__uiMetrics;
  });
  const gaps = metrics.gaps.sort((a, b) => a - b);
  const nodes = (profile?.nodes ?? []).sort(
    (a, b) => (b.hitCount ?? 0) - (a.hitCount ?? 0),
  );
  report.phases[name] = {
    elapsedMs,
    frameCount: gaps.length,
    maxFrameGapMs: gaps.at(-1) ?? 0,
    p95FrameGapMs: gaps[Math.floor(gaps.length * 0.95)] ?? 0,
    longTasksSupported: metrics.longTasksSupported,
    longTaskCount: metrics.longTasksSupported ? metrics.tasks.length : null,
    maxLongTaskMs: metrics.longTasksSupported
      ? Math.max(0, ...metrics.tasks)
      : null,
    peakRows: metrics.peakRows,
    newRows: metrics.newRows,
    blankSamples: metrics.blank,
    viewportSamples: metrics.samples,
    hotFunctions: nodes.slice(0, 12).map((n) => ({
      name: n.callFrame.functionName,
      url: n.callFrame.url,
      line: n.callFrame.lineNumber + 1,
      samples: n.hitCount,
    })),
  };
  console.log(name, JSON.stringify(report.phases[name]));
}
try {
  await page.goto(url);
  await page.locator("input.group-name").first().waitFor();
  await page
    .locator(`.group[data-uuid="${scrollGroupId}"] [data-collapsible-trigger]`)
    .click();
  await page.locator(".rule").first().waitFor();
  await phase("scroll", async () => {
    await page.mouse.move(900, 600);
    for (let i = 0; i < 25; i++) {
      await page.mouse.wheel(0, 1100);
      await page.waitForTimeout(45);
    }
  });
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.waitForTimeout(200);
  await phase("importOpen", async () => {
    const chooser = page.waitForEvent("filechooser");
    await page
      .getByRole("button", { name: "Import Config", exact: true })
      .click();
    await (await chooser).setFiles(importFile);
    await page
      .getByRole("dialog", { name: "Import Config", exact: true })
      .waitFor();
    await page.getByRole("dialog").getByRole("switch").waitFor();
  });
  await phase("importToggle", async () => {
    const toggle = page.getByRole("dialog").getByRole("switch");
    await toggle.click();
    await toggle.click();
    await toggle.click();
    const apply = page
      .getByRole("dialog")
      .getByRole("button", { name: "Import", exact: true });
    await apply.waitFor({ state: "visible" });
    await page.waitForFunction(
      () =>
        ![...document.querySelectorAll('[role="dialog"] button')].find(
          (b) => b.textContent.trim() === "Import",
        )?.disabled,
    );
  });
  await phase("importApply", async () => {
    await page
      .getByRole("dialog")
      .getByRole("button", { name: "Import", exact: true })
      .click();
    await page
      .getByRole("dialog", { name: "Import Config", exact: true })
      .waitFor({ state: "hidden" });
    await page.locator(".group-list .group").first().waitFor();
  });
} finally {
  await writeFile(out, JSON.stringify(report, null, 2));
  await browser.close();
}
