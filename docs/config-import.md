# Portable configuration import

The WebUI imports `.mtrickle` and `.mtrickle.json` files into an editable,
in-memory draft. Choosing **Import** does not apply routing, write the device
configuration or synchronize subscriptions. Switching tabs retains this draft;
reloading the page discards it. **Cancel** closes the preview without changing
either the existing draft or the saved configuration.

## Selection and conflicts

- **Replace** replaces only selected sections with the selected file entries.
  An explicitly selected empty collection clears that section. An absent or
  unselected section is preserved. Replace can overwrite local edits in the
  selected section because it is still a draft operation.
- **Append** keeps existing objects by ID. A differing object with the same ID
  can keep its current draft values, use the imported values, or become a copy.
  Matching names or subscription URLs are not object identities.
- Referenced routing profiles travel with imported groups/subscriptions.
  Copied profile IDs and all imported references are remapped together.
  If a partial file omits profile definitions, required definitions are resolved
  from the current editor, including local edits; unrelated profile drafts stay local.
  Missing references are rejected, and a profile used by retained consumers
  cannot be removed without changing those consumers.
- Rule IDs remain unique across the group editor. IDs released by replaced or
  removed groups can be reused, so repeating Replace does not regenerate IDs.
  Collection order and rule order are meaningful changes.
- Known optional defaults compare equally: an omitted rule name is empty,
  omitted subscription overrides are empty, and the omitted profile terminal
  action is `blackhole`. Empty direct profile references are equivalent to
  omitted references; colors compare using the C server's lowercase/white
  normalization. These comparisons do not remove explicit override resets or
  silently rewrite exported drafts.

Legacy group files remain supported. Missing legacy IDs receive distinct IDs;
explicit malformed IDs, unsupported formats/versions and unsafe settings are
rejected before staging. Existing valid hexadecimal IDs are normalized to the
backend's lowercase representation.

## Explicit Save

While an import is pending, Save and Ctrl/Cmd+S use the same configuration save
operation from Groups, Subscriptions or Settings. One request includes the
selected imported sections, their required definitions/consumer changes, and
the section whose Save action was used. It uses the original saved revision as
the server precondition. Ordinary edits made after importing enter this save.

Unrelated drafts in other sections stay local. A dependency imported with a
group does not authorize saving other unfinished profiles. Updating a profile's
primary interface updates the saved consumers' compatibility interface without
saving unrelated edits to those consumers.

During a pending import, subscription additions and deletions are also staged.
Synchronization requires Save first; it cannot implicitly persist a draft URL
or a subscription which does not yet exist on the server.

Save accepts the returned configuration only after a fresh snapshot confirms
the expected portable data. A rejection, transport failure or malformed/mismatched
acknowledgement leaves the draft available. If POST succeeded but confirmation
was lost, the next Save checks the server before writing again. Opening another
import also reconciles that attempt before creating a new preview.

A changed server revision is not overwritten automatically. The draft can be
exported before reloading and importing against the newer saved state. Edits
made after a submitted Save remain dirty when that Save is confirmed.

## Subscription caches and overrides

Portable files contain subscription definitions and user exceptions, not
downloaded rules. Caches are reused only for the same ID and URL. An explicit
empty override array resets old exceptions; absence in older editor responses
is handled separately.

Rule edits after import are compared with the staged effective rules. Comparing
them with the old saved cache would incorrectly recreate removed overrides or
lose an edit back to its previous saved value.

Removing a type override can leave the browser without the original source
type. In that case the draft cache is cleared until Save returns the canonical
rules. The save payload still preserves the server cache; no guessed source
type or stale downloaded rows are sent back to the device.

## Regression coverage

Pure tests cover decoding, selection, merge choices, ID allocation, ordering,
profile dependencies, draft projection/rebase, subscription exceptions and
saved-baseline tracking. Store tests check that staging has no network effects,
late GETs cannot overwrite it, and accepting a confirmed save clears only
confirmed changes.

Browser tests cover both viewport sizes, all import entry points, cancellation
and reload, repeated imports after editing/saving, multiple staged files,
keyboard Save, dependent profiles, unchanged imports, stale revisions and save
failures/retries. The live suite starts an isolated Deno API for each test;
route-mocked tests model specific failure and canonicalization responses.
Both suites reject uncaught browser errors.

The C `test_system`, `test_groups`, `test_subscriptions_api`,
`test_profiles_api`, `test_profiles_reload` and `test_config_json` suites cover
the production API/persistence contracts. Browser API mocks alone do not prove
kernel routing or persistence on a physical router. User configuration files
must not be committed as test fixtures.
