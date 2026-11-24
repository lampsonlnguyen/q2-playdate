#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unicorn/arm.h>
#include <unicorn/unicorn.h>
#include <zlib.h>

enum {
    HEADER_SIZE = 48, MAX_IMAGE = 65536,
    CODE_PAGE = 0x60000000, CODE = CODE_PAGE + 32, RETURN_PC = CODE_PAGE + 2,
    STACK = 0x20000000, STACK_SIZE = 4096, ITERATIONS = 120, EXPECTED = 50005000
};

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int load_fixture(const char *path, unsigned char *image,
                        size_t *image_size, uint32_t *entry)
{
    static unsigned char packed[MAX_IMAGE];
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 0;
    }
    size_t size = fread(packed, 1, sizeof packed, file);
    int complete = size < sizeof packed && feof(file) && !ferror(file);
    fclose(file);
    if (!complete || size <= HEADER_SIZE || memcmp(packed, "Playdate PDX", 13))
        return 0;

    uint32_t unpacked_size = read_le32(packed + 32);
    uint32_t program_size = read_le32(packed + 36);
    uint32_t shim = read_le32(packed + 40);
    uint32_t relocations = read_le32(packed + 44);
    /* The bundled fixture has no BSS or relocations. */
    if (unpacked_size > MAX_IMAGE || program_size != unpacked_size ||
        relocations != 0 || (shim & ~1u) >= program_size)
        return 0;

    unsigned long actual = unpacked_size;
    if (uncompress(image, &actual, packed + HEADER_SIZE, size - HEADER_SIZE) != Z_OK ||
        actual != unpacked_size)
        return 0;
    *image_size = actual;
    *entry = CODE + (shim | 1u);
    return 1;
}

static int check_uc(uc_err status, const char *operation)
{
    if (status == UC_ERR_OK)
        return 1;
    fprintf(stderr, "%s: %s\n", operation, uc_strerror(status));
    return 0;
}

static uint64_t time_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}

static int call_fixture(uc_engine *engine, uint32_t entry, uint32_t event, uint32_t expected)
{
    uint32_t r0 = 0, sp = STACK + STACK_SIZE, lr = RETURN_PC | 1u, pc = 0;
    if (!check_uc(uc_reg_write(engine, UC_ARM_REG_R0, &r0), "API argument") ||
        !check_uc(uc_reg_write(engine, UC_ARM_REG_R1, &event), "event argument") ||
        !check_uc(uc_reg_write(engine, UC_ARM_REG_SP, &sp), "stack pointer") ||
        !check_uc(uc_reg_write(engine, UC_ARM_REG_LR, &lr), "return address") ||
        !check_uc(uc_emu_start(engine, entry, RETURN_PC, 0, 40000), "execute Thumb code") ||
        !check_uc(uc_reg_read(engine, UC_ARM_REG_PC, &pc), "read program counter") ||
        !check_uc(uc_reg_read(engine, UC_ARM_REG_R0, &r0), "read return register"))
        return 0;
    if (pc != RETURN_PC || r0 != expected) {
        fprintf(stderr, "Unexpected return: PC=%08" PRIx32 ", R0=%" PRIu32 "\n", pc, r0);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/pdex.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    static unsigned char image[MAX_IMAGE];
    size_t size;
    uint32_t entry;
    if (!load_fixture(argv[1], image, &size, &entry)) {
        fprintf(stderr, "Unsupported or invalid native probe binary\n");
        return EXIT_FAILURE;
    }

    uc_engine *engine;
    if (!check_uc(uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &engine), "open Unicorn"))
        return EXIT_FAILURE;

    int result = EXIT_FAILURE;
    if (!check_uc(uc_ctl_set_cpu_model(engine, UC_CPU_ARM_CORTEX_M7), "select Cortex-M7") ||
        !check_uc(uc_mem_map(engine, CODE_PAGE, (size + 32 + 4095) & ~(size_t)4095,
                            UC_PROT_READ | UC_PROT_EXEC), "map code") ||
        !check_uc(uc_mem_write(engine, CODE, image, size), "load code") ||
        !check_uc(uc_mem_map(engine, STACK, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE), "map stack"))
        goto done;

    uint64_t cold_start = time_ns();
    if (!call_fixture(engine, entry, 0, EXPECTED))
        goto done;
    uint64_t cold = time_ns() - cold_start;
    if (!call_fixture(engine, entry, 1, 0))
        goto done;

    uint64_t total = 0, peak = 0;
    for (unsigned frame = 0; frame < ITERATIONS; frame++) {
        uint64_t start = time_ns();
        if (!call_fixture(engine, entry, 0, EXPECTED))
            goto done;
        uint64_t elapsed = time_ns() - start;
        total += elapsed;
        if (elapsed > peak)
            peak = elapsed;
    }

    struct rusage usage;
    printf("ARM Cortex-M7 probe passed (%u-bit host process)\n",
           (unsigned)(8 * sizeof(void *)));
    printf("Cold call: %.3f ms; warm calls: %d, mean %.3f ms, max %.3f ms\n",
           cold / 1e6, ITERATIONS, total / (ITERATIONS * 1e6), peak / 1e6);
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        printf("Peak RSS: %ld KiB\n", usage.ru_maxrss);
    result = EXIT_SUCCESS;

done:
    uc_close(engine);
    return result;
}
