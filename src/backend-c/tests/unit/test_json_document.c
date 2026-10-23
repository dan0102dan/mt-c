#include "greatest.h"
#include "magitrickle/json.h"
#include <string.h>
TEST strict_documents_preserve_literal_escape_and_reject_nul(void) {
    const char *bad[] = {"{}junk", "{} {}", "{\"id\":\"one\\u0000two\"}",
        "{\"i\\u0000d\":1}", "[\"\\u0000\"]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        ASSERT(mt_json_parse_document(bad[i], strlen(bad[i])) == NULL);
    }
    ASSERT(mt_json_parse_document("{}\0x", 4) == NULL);
    const char *literal = "{\"name\":\"\\\\u0000\"}";
    cJSON *root = mt_json_parse_document(literal, strlen(literal));
    ASSERT(root); ASSERT_STR_EQ("\\u0000", cJSON_GetObjectItemCaseSensitive(root, "name")->valuestring);
    cJSON_Delete(root);
    root = mt_json_parse_document(" {} \n\t", 6); ASSERT(root); cJSON_Delete(root);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(strict_documents_preserve_literal_escape_and_reject_nul);
    GREATEST_MAIN_END();
}
