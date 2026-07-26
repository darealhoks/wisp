/* libFuzzer harness for the D-Bus wire reader (src/dbus_wire.c).
 *
 * This is the crown-jewel attack surface: any process on the session bus feeds
 * these primitives attacker-chosen lengths and nested signatures (notification
 * a{sv} hints, header fields). The readers are side-effect-free, so we drive
 * them straight off fuzz bytes under ASan.
 *
 *   make fuzz && ./build/fuzz/fuzz_dbus -runs=20000 fuzz/corpus
 *
 * Layout of one input: [siglen:1][signature:siglen][body:rest]. skip_val walks
 * the signature over the body (the recursive path with the depth cap), then the
 * primitive readers are hammered directly to catch alignment/bound math.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dbus.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    size_t siglen = data[0] % 16;
    if (1 + siglen > size) return 0;

    char sig[16];
    memcpy(sig, data + 1, siglen);
    sig[siglen] = 0;

    const uint8_t *body = data + 1 + siglen;
    int blen = (int)(size - 1 - siglen);

    /* signature-driven recursive skip */
    R r = { .b = body, .len = blen, .pos = 0, .ok = 1 };
    const char *s = sig;
    while (r.ok && *s)
        if (skip_val(&r, &s, 0) < 0) break;

    /* primitive readers, in case a raw byte stream trips alignment/bounds */
    R r2 = { .b = body, .len = blen, .pos = 0, .ok = 1 };
    while (r2.ok && r2.pos < r2.len) {
        int before = r2.pos;
        rbyte(&r2); ru16(&r2); ru32(&r2); ri32(&r2);
        rstr(&r2); rsig(&r2);
        if (r2.pos == before) r2.pos++;   /* guarantee progress */
    }
    return 0;
}
