#include "magitrickle/quic.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mt_quic_datagram_t parsed;
    (void)mt_quic_parse(data, size, &parsed);
    return 0;
}
