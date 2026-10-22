#include "magitrickle/json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cJSON *root = mt_json_parse_document((const char *)data, size);
    if (root) {
        char *wire = cJSON_PrintUnformatted(root);
        if (wire) {
            cJSON *copy = mt_json_parse_document(wire, strlen(wire));
            if (!copy || !cJSON_Compare(root, copy, true)) { abort(); }
            cJSON_Delete(copy); free(wire);
        }
        cJSON_Delete(root);
    }
    return 0;
}
