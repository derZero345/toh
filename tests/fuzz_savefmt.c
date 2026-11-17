/* libFuzzer target for the save file decoder. Built by `make fuzz` with -DSAVEFMT_FUZZING,
 * which skips the integrity tag so that mutated inputs reach the parser. */
#include "savefmt.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    SaveData d;
    if (savefmt_decode(data, size, &d) == SAVE_OK) {
        /* everything the decoder accepts must be writable again */
        uint8_t *out;
        size_t len;
        if (savefmt_encode(&d, NULL, &out, &len) != SAVE_OK) abort();
        free(out);
    }
    savefmt_free(&d);
    return 0;
}
