import { strict as assert } from "node:assert";

import { cleanProfiles } from "../../src/modules/settings/profiles-data";
import type { Profile } from "../../src/types";
import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
const { fetchProfiles, profileDraftIds, profiles, profilesDirty } =
  await import("../../src/data/profiles.svelte");
const { fetcher } = await import("../../src/utils/fetcher");

const profile = (id: string, name = id): Profile => ({
  id,
  name,
  interfaces: ["wg0", "wg1"],
  on_unavailable: "blackhole",
  usage: { groups: 0, subscriptions: 0 },
});
function seed(list: Profile[]) {
  Object.assign(profiles, {
    list: structuredClone(list),
    draft: structuredClone(list),
    baseline: JSON.stringify(cleanProfiles(list)),
    loaded: true,
    loading: false,
    failed: false,
    saving: false,
    persistencePending: false,
  });
}
async function withGet(response: () => Promise<unknown>, run: () => Promise<void>) {
  const original = fetcher.get;
  fetcher.get = <T>() => response() as Promise<T>;
  try {
    await run();
  } finally {
    fetcher.get = original;
  }
}

Deno.test(
  "profile draft IDs include field edits, additions and deletions but not list order",
  () => {
    const original = [profile("a"), profile("b"), profile("c")];
    seed(original);
    profiles.draft.reverse();
    profiles.draft[0].name = " c ";
    profiles.draft[0].usage = { groups: 9, subscriptions: 2 };
    assert.deepEqual(profileDraftIds(), new Set());
    assert.equal(profilesDirty(), true);

    profiles.draft[0].interfaces.reverse();
    profiles.draft = profiles.draft.filter((item) => item.id !== "b");
    profiles.draft.push({ ...profile("new"), name: "", interfaces: [] });
    assert.deepEqual(profileDraftIds(), new Set(["c", "new", "b"]));
  },
);

Deno.test(
  "import refresh rebases server profiles while retaining unrelated unfinished drafts",
  async () => {
    const original = [profile("a"), profile("b"), profile("c"), profile("d")];
    const fresh = [
      profile("a"),
      profile("b"),
      { ...profile("d", "Imported D"), usage: { groups: 2, subscriptions: 1 } },
      profile("imported", "Imported new profile"),
    ];
    seed(original);
    profiles.draft = [
      structuredClone(original[3]),
      { ...profile("a"), name: "  Local edit  ", interfaces: [] },
      structuredClone(original[2]),
      { ...profile("local"), name: "", interfaces: [] },
    ];
    profiles.persistencePending = true;

    await withGet(
      async () => ({ profiles: fresh }),
      async () => {
        await fetchProfiles({ fresh: true, rebaseDrafts: true });
      },
    );

    assert.deepEqual(profiles.list, fresh);
    assert.equal(profiles.baseline, JSON.stringify(cleanProfiles(fresh)));
    assert.deepEqual(
      profiles.draft.map((item) => item.id),
      ["d", "a", "local", "imported"],
    );
    assert.equal(profiles.draft[0].name, "Imported D");
    assert.equal(profiles.draft[1].name, "  Local edit  ");
    assert.deepEqual(profiles.draft[1].interfaces, []);
    assert.equal(profiles.draft[2].name, "");
    assert.deepEqual(profileDraftIds(), new Set(["a", "local", "b"]));
    assert.equal(profiles.persistencePending, true);
    assert.equal(profilesDirty(), true);
    assert.equal(profiles.failed, false);

    profiles.draft[0].interfaces.reverse();
    profiles.draft[0].usage!.groups = 99;
    assert.deepEqual(profiles.list[2].interfaces, ["wg0", "wg1"]);
    assert.equal(profiles.list[2].usage?.groups, 2);
    assert.equal(profiles.baseline, JSON.stringify(cleanProfiles(fresh)));
  },
);

Deno.test(
  "import refresh preserves local reorder without marking profiles individually dirty",
  async () => {
    seed([profile("a"), profile("b")]);
    profiles.draft.reverse();
    const fresh = [profile("a"), profile("new"), profile("b")];
    await withGet(
      async () => ({ profiles: fresh }),
      async () => {
        await fetchProfiles({ fresh: true, rebaseDrafts: true });
      },
    );
    assert.deepEqual(
      profiles.draft.map((item) => item.id),
      ["b", "a", "new"],
    );
    assert.equal(profiles.baseline, JSON.stringify(cleanProfiles(fresh)));
    assert.deepEqual(profileDraftIds(), new Set());
    assert.equal(profilesDirty(), true);
  },
);

Deno.test(
  "import refresh adopts server order when only profile fields were edited locally",
  async () => {
    seed([profile("a"), profile("b")]);
    profiles.draft[0].name = "Local A";
    const fresh = [profile("b"), profile("new"), profile("a")];
    await withGet(
      async () => ({ profiles: fresh }),
      async () => {
        await fetchProfiles({ rebaseDrafts: true });
      },
    );
    assert.deepEqual(
      profiles.draft.map((item) => item.id),
      ["b", "new", "a"],
    );
    assert.equal(profiles.draft[2].name, "Local A");
    assert.deepEqual(profileDraftIds(), new Set(["a"]));
  },
);

Deno.test(
  "queued import refresh ignores a pre-import response and retains its rebase intent",
  async () => {
    seed([profile("a"), profile("b")]);
    profiles.draft[0].name = "Local A";
    profiles.persistencePending = true;
    const previousList = structuredClone(profiles.list);
    const previousDraft = structuredClone(profiles.draft);
    const previousBaseline = profiles.baseline;
    const stale = Promise.withResolvers<unknown>();
    const fresh = Promise.withResolvers<unknown>();
    let calls = 0;
    await withGet(
      () => (++calls === 1 ? stale.promise : fresh.promise),
      async () => {
        const pending = fetchProfiles();
        const refresh = fetchProfiles({ fresh: true, rebaseDrafts: true });
        stale.resolve({ profiles: [profile("stale")] });
        await pending;
        assert.equal(calls, 2);
        assert.deepEqual(profiles.list, previousList);
        assert.deepEqual(profiles.draft, previousDraft);
        assert.equal(profiles.baseline, previousBaseline);

        fresh.resolve({ profiles: [profile("a"), profile("b"), profile("imported")] });
        await refresh;
      },
    );
    assert.deepEqual(
      profiles.draft.map((item) => item.id),
      ["a", "b", "imported"],
    );
    assert.equal(profiles.draft[0].name, "Local A");
    assert.deepEqual(profileDraftIds(), new Set(["a"]));
    assert.equal(profiles.persistencePending, true);
    assert.equal(profiles.loading, false);
  },
);

Deno.test("edits made during the import refresh survive its response", async () => {
  seed([profile("a")]);
  const response = Promise.withResolvers<unknown>();
  await withGet(
    () => response.promise,
    async () => {
      const pending = fetchProfiles({ fresh: true, rebaseDrafts: true });
      profiles.draft[0].name = "Edited during refresh";
      response.resolve({ profiles: [profile("a"), profile("new")] });
      await pending;
    },
  );
  assert.equal(profiles.draft[0].name, "Edited during refresh");
  assert.equal(profiles.draft[1].id, "new");
  assert.deepEqual(profileDraftIds(), new Set(["a"]));
});

Deno.test(
  "failed or invalid import refresh leaves profiles, drafts and baseline intact",
  async () => {
    for (const response of [
      async () => {
        throw new Error("Connection lost");
      },
      async () => ({ profiles: [{ id: "broken", name: "", interfaces: [] }] }),
    ]) {
      seed([profile("a")]);
      profiles.draft[0].name = "Local A";
      profiles.persistencePending = true;
      const previousList = structuredClone(profiles.list);
      const previousDraft = structuredClone(profiles.draft);
      const previousBaseline = profiles.baseline;
      await withGet(response, async () => {
        await fetchProfiles({ fresh: true, rebaseDrafts: true });
      });
      assert.deepEqual(profiles.list, previousList);
      assert.deepEqual(profiles.draft, previousDraft);
      assert.equal(profiles.baseline, previousBaseline);
      assert.equal(profiles.persistencePending, true);
      assert.equal(profiles.loaded, true);
      assert.equal(profiles.failed, true);
      assert.equal(profiles.loading, false);
    }
  },
);

Deno.test(
  "ordinary refresh keeps the existing draft and baseline acceptance behavior",
  async () => {
    const original = [profile("a")];
    const fresh = [profile("a", "Server A"), profile("new")];
    await withGet(
      async () => ({ profiles: fresh }),
      async () => {
        seed(original);
        profiles.draft[0].name = "Local A";
        profiles.persistencePending = true;
        const previousBaseline = profiles.baseline;
        await fetchProfiles({ fresh: true });
        assert.deepEqual(profiles.list, fresh);
        assert.equal(profiles.draft[0].name, "Local A");
        assert.equal(profiles.baseline, previousBaseline);
        assert.equal(profiles.persistencePending, true);

        seed(original);
        await fetchProfiles();
        assert.deepEqual(profiles.list, fresh);
        assert.deepEqual(profiles.draft, fresh);
        assert.equal(profiles.baseline, JSON.stringify(cleanProfiles(fresh)));
        assert.equal(profilesDirty(), false);
      },
    );
  },
);
