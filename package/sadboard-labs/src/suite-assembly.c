#include "suite.h"
#include <stddef.h>
#include <string.h>

#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
extern uint64_t sb_asm_width_bug(const uint32_t *);
extern uint64_t sb_asm_width_fixed(const uint32_t *);
extern int64_t sb_asm_sign_bug(const int8_t *);
extern int64_t sb_asm_sign_fixed(const int8_t *);
extern uintptr_t sb_asm_pointer_bug(uintptr_t);
extern uintptr_t sb_asm_pointer_fixed(uintptr_t);
extern unsigned sb_asm_branch_bug(uint32_t, uint32_t);
extern unsigned sb_asm_branch_fixed(uint32_t, uint32_t);
extern uint64_t sb_asm_call_bug(uint64_t);
extern uint64_t sb_asm_call_fixed(uint64_t);
extern uint64_t sb_asm_layout_bug(const void *);
extern uint64_t sb_asm_layout_fixed(const void *);
extern uint64_t sb_asm_bounds_bug(const uint32_t *, uint64_t, uint64_t *);
extern uint64_t sb_asm_bounds_fixed(const uint32_t *, uint64_t, uint64_t *);
extern uint64_t sb_asm_preserve_bug(void);
extern uint64_t sb_asm_preserve_fixed(void);

void suite_assembly(bool fixed, struct sample *r, unsigned kind)
{
    r->completed = 1;
    if (kind == 0) {
        _Alignas(8) uint32_t words[2] = {UINT32_C(0x89abcdef), UINT32_C(0x12345678)};
        r->checksum = words[0];
        r->before = words[1];
        r->observed = fixed ? sb_asm_width_fixed(words) : sb_asm_width_bug(words);
        r->auxiliary = fixed ? 4 : 8;
        suite_event("first_word", words[0]);
        suite_event("adjacent_word", words[1]);
    } else if (kind == 1) {
        int8_t byte = INT8_MIN;
        r->checksum = (uint64_t)(int64_t)byte;
        r->before = (uint8_t)byte;
        r->observed = (uint64_t)(fixed ? sb_asm_sign_fixed(&byte) : sb_asm_sign_bug(&byte));
        r->auxiliary = 8;
    } else if (kind == 2) {
        uint32_t value = 7;
        uintptr_t pointer = (uintptr_t)&value;
        suite_require(pointer > UINT32_MAX, "high-address pointer prerequisite");
        r->checksum = value;
        r->before = pointer;
        r->observed = fixed ? sb_asm_pointer_fixed(pointer) : sb_asm_pointer_bug(pointer);
        r->after = r->observed;
        r->auxiliary = UINT32_MAX;
        /* The deliberately truncated value is observed, never dereferenced. */
        suite_event("pointer_dereferenced_after_truncation", 0);
    } else if (kind == 3) {
        unsigned (*accept)(uint32_t, uint32_t) = fixed ? sb_asm_branch_fixed : sb_asm_branch_bug;
        unsigned small = accept(16, 64), oversized = accept(UINT32_C(0x80000000), 64);
        r->completed = 2;
        r->checksum = 1;
        r->observed = small + oversized;
        r->auxiliary = oversized;
        r->before = 16;
        r->after = UINT32_C(0x80000000);
        suite_event("accepted_small", small);
        suite_event("accepted_oversized", oversized);
    } else if (kind == 4) {
        r->before = 7;
        r->checksum = 15;
        r->observed = fixed ? sb_asm_call_fixed(7) : sb_asm_call_bug(7);
        r->auxiliary = 85;
    } else if (kind == 5) {
        struct layout {
            uint8_t tag;
            uint32_t count;
            uint64_t stamp;
        } object;
        memset(&object, 0, sizeof(object));
        object.tag = 0x11;
        object.count = UINT32_C(0x12345678);
        object.stamp = UINT64_C(0x1122334455667788);
        _Static_assert(offsetof(struct layout, count) == 4, "teaching ABI offset");
        r->checksum = object.count;
        r->before = sizeof(object);
        r->after = offsetof(struct layout, count);
        r->observed = fixed ? sb_asm_layout_fixed(&object) : sb_asm_layout_bug(&object);
        r->auxiliary = fixed ? 4 : 1;
    } else if (kind == 6) {
        const uint32_t values[5] = {1, 2, 3, 4, 55};
        uint64_t iterations = 0;
        r->completed = 4;
        for (unsigned i = 0; i < 4; i++)
            r->checksum += values[i];
        r->observed = fixed ? sb_asm_bounds_fixed(values, 4, &iterations)
                            : sb_asm_bounds_bug(values, 4, &iterations);
        r->auxiliary = iterations;
        r->before = 4;
        r->after = sizeof(values);
        suite_event("logical_count", 4);
        suite_event("allocated_count", 5);
        suite_event("sentinel", values[4]);
        suite_event("actual_iterations", iterations);
    } else {
        r->checksum = 0x1234;
        r->before = 0x1234;
        r->observed = fixed ? sb_asm_preserve_fixed() : sb_asm_preserve_bug();
        r->after = r->observed;
        r->auxiliary = 19;
        suite_event("C_caller_X19_restored_by_probe", 1);
    }
    suite_event("assembly_variant", kind);
    suite_event("reference_value", r->checksum);
    suite_event("actual_value", r->observed);
}
#else
void suite_assembly(bool fixed, struct sample *r, unsigned kind)
{
    (void)fixed;
    (void)r;
    (void)kind;
    fputs("A64 teaching cases require a little-endian AArch64 target; no emulation is used\n", stderr);
    exit(77);
}
#endif
