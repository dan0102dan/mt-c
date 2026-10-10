#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "magitrickle/update.h"

static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)

static cJSON *fixture(const char *tag, const char *suffix, const char *date, bool pre) {
    cJSON *release = cJSON_CreateObject();
    cJSON_AddNumberToObject(release, "id", 100);
    cJSON_AddStringToObject(release, "tag_name", tag);
    cJSON_AddBoolToObject(release, "draft", false);
    cJSON_AddBoolToObject(release, "prerelease", pre);
    cJSON_AddStringToObject(release, "published_at", date);
    cJSON *assets = cJSON_CreateArray();
    cJSON_AddItemToObject(release, "assets", assets);
    cJSON *asset = cJSON_CreateObject();
    char name[256], url[1024];
    snprintf(name, sizeof(name), "mt-c_%s-1_%s", tag, suffix);
    snprintf(url, sizeof(url), MT_UPDATE_WEB "/download/%s/%s", tag, name);
    cJSON_AddStringToObject(asset, "name", name);
    cJSON_AddStringToObject(asset, "browser_download_url", url);
    cJSON_AddStringToObject(asset, "state", "uploaded");
    cJSON_AddNumberToObject(asset, "size", 123);
    cJSON_AddStringToObject(asset, "digest", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    cJSON_AddItemToArray(assets, asset);
    return release;
}

static const char *resolve(cJSON *root, bool preview, const char *tag, const char *suffix, mt_update_asset_t *out) {
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    const char *error = mt_update_resolve(json, strlen(json), preview, tag, 100, suffix, "0.8.2.2", 1, out);
    free(json);
    return error;
}

int main(void) {
    mt_update_version_t a, b;
    CHECK(mt_update_version_parse("0.8.10", 1, &a));
    CHECK(mt_update_version_parse("0.8.9", 1, &b));
    CHECK(mt_update_version_compare(&a, &b) > 0);
    CHECK(mt_update_version_parse("0.8.3", 1, &a));
    CHECK(mt_update_version_parse("0.8.2.2", 1, &b));
    CHECK(mt_update_version_compare(&a, &b) > 0);
    CHECK(mt_update_version_parse("0.8.3~git20261010000000.abcdef0", 1, &b));
    CHECK(mt_update_version_compare(&a, &b) > 0);
    CHECK(mt_update_version_parse("0.8.4~git20261010000000.abcdef0", 1, &b));
    CHECK(mt_update_version_compare(&a, &b) < 0);
    CHECK(mt_update_version_parse("v0.8.3-rev2", 1, &a));
    CHECK(mt_update_version_parse("0.8.3", 1, &b));
    CHECK(mt_update_version_compare(&a, &b) > 0);
    CHECK(mt_update_version_parse("0.8.3", 2, &b));
    CHECK(mt_update_version_compare(&a, &b) == 0);
    const char *bad[] = {"", "unattached", "1", "1.2.3.4.5", "1.2.", "1.2-rev0", "1.2-rc1", "1.2;reboot", "1.2/../../x", "9999999999.1", "1.2~git123.abcdef0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) { CHECK(!mt_update_version_parse(bad[i], 1, &a)); }
    CHECK(mt_update_download_host_allowed("https://github.com/dan0102dan/mt-c/releases/download/0.8.3/test.ipk"));
    CHECK(mt_update_download_host_allowed("https://release-assets.githubusercontent.com/asset?signature=1"));
    CHECK(!mt_update_download_host_allowed("http://github.com/asset"));
    CHECK(!mt_update_download_host_allowed("file:///etc/passwd"));
    CHECK(!mt_update_download_host_allowed("https://github.com.attacker.test/asset"));
    CHECK(!mt_update_download_host_allowed("https://github.com@127.0.0.1/asset"));
    CHECK(!mt_update_download_host_allowed("https://127.0.0.1/asset"));
    CHECK(!mt_update_download_host_allowed("https://github.com/asset\r\nX: y"));

    const char *suffix = "entware_aarch64-3.10_kn.ipk";
    mt_update_asset_t out;
    cJSON *release = fixture("0.8.3", suffix, "2026-10-09T18:26:19Z", false);
    CHECK(resolve(release, false, "0.8.3", suffix, &out) == NULL);
    CHECK(out.size == 123 && out.revision == 1 && strcmp(out.version, "0.8.3") == 0);
    CHECK(resolve(release, false, "0.8.3", "entware_aarch64-3.10.ipk", &out) != NULL);
    CHECK(resolve(release, false, "0.8.4", suffix, &out) != NULL);
    cJSON *asset = cJSON_GetObjectItemCaseSensitive(release, "assets")->child;
    cJSON_DeleteItemFromObjectCaseSensitive(asset, "digest");
    CHECK(resolve(release, false, "0.8.3", suffix, &out) != NULL);
    cJSON_AddStringToObject(asset, "digest", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    cJSON_DeleteItemFromObjectCaseSensitive(asset, "browser_download_url");
    cJSON_AddStringToObject(asset, "browser_download_url", "https://example.org/package.ipk");
    CHECK(resolve(release, false, "0.8.3", suffix, &out) != NULL);
    cJSON_Delete(release);

    release = fixture("0.8.3", suffix, "2026-10-09T18:26:19Z", true);
    CHECK(resolve(release, false, "0.8.3", suffix, &out) != NULL);
    cJSON *list = cJSON_CreateArray();
    cJSON_AddItemToArray(list, release);
    cJSON_AddItemToArray(list, fixture("0.8.2.2", suffix, "2026-10-01T18:26:19Z", false));
    CHECK(resolve(list, true, "0.8.3", suffix, &out) == NULL);
    cJSON_AddItemToArray(list, fixture("0.8.4", suffix, "2026-10-10T18:26:19Z", false));
    CHECK(resolve(list, true, "0.8.3", suffix, &out) != NULL);
    CHECK(resolve(list, true, "0.8.4", suffix, &out) == NULL);
    cJSON_Delete(list);

    release = fixture("0.8.3", "openwrt-24.10.4_aarch64_cortex-a53.ipk", "2026-10-09T18:26:19Z", false);
    CHECK(resolve(release, false, "0.8.3", "openwrt-24.10.4_aarch64_cortex-a53.ipk", &out) == NULL);
    CHECK(resolve(release, false, "0.8.3", "openwrt-25.12.5_aarch64_cortex-a53.apk", &out) != NULL);
    cJSON_Delete(release);
    CHECK(mt_update_resolve("{", 1, false, "0.8.3", 100, suffix, "0.8.2.2", 1, &out) != NULL);
    CHECK(mt_update_resolve("{}", 2, false, "0.8.3", 100, suffix, "0.8.4", 1, &out) != NULL);
    CHECK(mt_update_resolve("{}", 2, false, "0.8.3", 100, suffix, "unattached", 1, &out) != NULL);
    printf("%u update release checks passed\n", checks);
    return 0;
}
