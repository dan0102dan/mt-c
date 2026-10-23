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
            /* cJSON deliberately prints out-of-range floating-point values as
             * null, so semantic equality is not a promise for arbitrary input. */
            if (!copy) { abort(); }
            cJSON_Delete(copy); free(wire);
        }
        cJSON_Delete(root);
    }
    return 0;
}
