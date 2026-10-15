/*
 * ESP32-OS bootloader UART monitor.
 * Target: ESP32-S3 / ESP-IDF v4.4.7.
 *
 * This extends Espressif's second-stage bootloader with its documented hooks;
 * it does not replace ESP-IDF app selection/loading. Flash access uses the
 * bootloader's own flash routines and ESP-IDF's partition/image validators.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp32s3/rom/ets_sys.h"
#include "esp32s3/rom/uart.h"
#include "esp32s3/rom/rtc.h"
#include "esp32s3/rom/spi_flash.h"
#include "soc/rtc.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_struct.h"
#include "hal/wdt_hal.h"
#include "bootloader_flash_priv.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_flash_encrypt.h"

#define BLDR_VERSION             "1.1.0"
#define BLDR_TRIGGER_MS          500U
#define BLDR_MONITOR_MAX_MS      1800000U /* 30 minutes; watchdog is fed while waiting */
#define BLDR_LINE_MAX            128U
#define BLDR_ARG_MAX             8U
#define BLDR_SECTOR_SIZE         0x1000U
#define BLDR_FLASH_IO_CHUNK      512U
#define BLDR_XMODEM_PACKET_MAX   1024U
#define BLDR_XMODEM_WRITE_BUFFER_SIZE 1024U
#define BLDR_RAM_BUFFER_SIZE     (64U * 1024U)
#define BLDR_EXEC_BUFFER_SIZE    (16U * 1024U)
#define BLDR_XMODEM_RETRY_MAX    20U
#define BLDR_XMODEM_BYTE_TIMEOUT 3000U
#define BLDR_XMODEM_IDLE_TIMEOUT 1000U
#define BLDR_UART_NUM            0U
#define XMODEM_SOH               0x01U
#define XMODEM_STX               0x02U
#define XMODEM_EOT               0x04U
#define XMODEM_ACK               0x06U
#define XMODEM_NAK               0x15U
#define XMODEM_CAN               0x18U
#define XMODEM_CRC_REQ           'C'

/* These buffers are intentionally owned by the monitor. `info` prints their
 * runtime addresses, and all RAM transfer commands restrict destinations to
 * these known buffers rather than accepting arbitrary CPU addresses. */
static uint8_t g_ram_buffer[BLDR_RAM_BUFFER_SIZE] __attribute__((aligned(32)));
static uint8_t g_exec_payload[BLDR_EXEC_BUFFER_SIZE]
    __attribute__((section(".iram1"), aligned(32)));
static uint8_t g_io_buffer[BLDR_XMODEM_PACKET_MAX] __attribute__((aligned(32)));
static uint8_t g_write_buffer[BLDR_XMODEM_WRITE_BUFFER_SIZE] __attribute__((aligned(32)));
static uint32_t g_partition_table_storage[ESP_PARTITION_TABLE_MAX_LEN / sizeof(uint32_t)]
    __attribute__((aligned(32)));
static uint32_t g_monitor_elapsed_ms;
static bool g_skip_lf;
static bool g_exec_payload_valid;
static uint32_t g_exec_payload_start;
static uint32_t g_exec_payload_len;
static bool g_unlock_armed;
static char g_unlock_label[17];
static uint32_t g_unlock_deadline_ms;
static uint32_t g_uart_baud = 115200U;

static void uart_putc(char c)
{
    (void)uart_tx_one_char((uint8_t)c);
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static void prompt(void)
{
    uart_puts("bldr> ");
}

static void set_uart_baud(uint32_t baud)
{
    uint32_t apb_hz = rtc_clk_apb_freq_get();
    if (apb_hz == 0U) apb_hz = 80000000U;
    uart_div_modify(BLDR_UART_NUM, (uint32_t)(((uint64_t)apb_hz << 4) / baud));
    g_uart_baud = baud;
}

static void feed_watchdog(void)
{
#if defined(CONFIG_BOOTLOADER_WDT_ENABLE) && CONFIG_BOOTLOADER_WDT_ENABLE
    static wdt_hal_context_t ctx = { .inst = WDT_RWDT, .rwdt_dev = &RTCCNTL };
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
#endif
}

static void delay_1ms(void)
{
    ets_delay_us(1000);
    if (g_monitor_elapsed_ms < UINT32_MAX) {
        ++g_monitor_elapsed_ms;
    }
    if ((g_monitor_elapsed_ms & 0xFFU) == 0U) {
        feed_watchdog();
    }
}

static bool read_byte_timeout(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (uart_rx_one_char(out) == 0) {
            return true;
        }
        delay_1ms();
        ++waited;
    }
    return false;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint32_t base = 10U;
    uint32_t v = 0U;
    const char *p = s;
    if (s == NULL || *s == '\0' || out == NULL || *s == '-') {
        return false;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16U;
        p += 2;
        if (*p == '\0') return false;
    }
    for (; *p != '\0'; ++p) {
        uint32_t d;
        if (*p >= '0' && *p <= '9') d = (uint32_t)(*p - '0');
        else if (base == 16U && *p >= 'a' && *p <= 'f') d = 10U + (uint32_t)(*p - 'a');
        else if (base == 16U && *p >= 'A' && *p <= 'F') d = 10U + (uint32_t)(*p - 'A');
        else return false;
        if (d >= base || v > (UINT32_MAX - d) / base) return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

static int split_args(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;
        if (argc >= max_args) return -1;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') ++p;
        if (*p != '\0') *p++ = '\0';
    }
    return argc;
}

static bool command_has_help(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) return true;
    }
    return false;
}

static uint32_t ram_buffer_base(void) { return (uint32_t)(uintptr_t)g_ram_buffer; }
static uint32_t exec_buffer_base(void) { return (uint32_t)(uintptr_t)g_exec_payload; }

static bool range_in_buffer(uint32_t addr, uint32_t len, bool writable, bool *is_exec)
{
    uint64_t end = (uint64_t)addr + (uint64_t)len;
    uint64_t rb = ram_buffer_base();
    uint64_t eb = exec_buffer_base();
    if (len == 0 || end > UINT32_MAX) return false;
    if ((uint64_t)addr >= rb && end <= rb + sizeof(g_ram_buffer)) {
        if (is_exec != NULL) *is_exec = false;
        (void)writable;
        return true;
    }
    if ((uint64_t)addr >= eb && end <= eb + sizeof(g_exec_payload)) {
        if (is_exec != NULL) *is_exec = true;
        (void)writable;
        return true;
    }
    return false;
}

static bool copy_to_ram(uint32_t addr, const void *src, uint32_t len)
{
    bool is_exec = false;
    if ((addr & 3U) != 0U || (len & 3U) != 0U ||
        !range_in_buffer(addr, len, true, &is_exec)) return false;
    if (is_exec) {
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)addr;
        const uint32_t *words = (const uint32_t *)src;
        for (uint32_t i = 0; i < len / 4U; ++i) dst[i] = words[i];
        __asm__ __volatile__("memw" ::: "memory");
    } else {
        memcpy((void *)(uintptr_t)addr, src, len);
    }
    return true;
}

static bool load_partition_table(esp_partition_info_t **out, int *count)
{
    esp_err_t err = bootloader_flash_read(CONFIG_PARTITION_TABLE_OFFSET,
                                           g_partition_table_storage,
                                           ESP_PARTITION_TABLE_MAX_LEN, true);
    if (err != ESP_OK) {
        ets_printf("ERR: partition-table read failed (0x%x)\r\n", (unsigned)err);
        return false;
    }
    esp_partition_info_t *table = (esp_partition_info_t *)g_partition_table_storage;
    err = esp_partition_table_verify(table, false, count);
    if (err != ESP_OK) {
        uart_puts("ERR: partition table invalid or MD5 check failed.\r\n");
        return false;
    }
    *out = table;
    return true;
}

static void partition_label(const esp_partition_info_t *p, char out[17])
{
    memcpy(out, p->label, 16);
    out[16] = '\0';
}

static bool partition_is_mutable(const esp_partition_info_t *p)
{
    /* Only standard app/data partitions can be modified by monitor commands. */
    if (p == NULL || (p->type != PART_TYPE_APP && p->type != PART_TYPE_DATA)) return false;
    /* Bit 1 is treated as read-only for forward compatibility with partition
     * table variants that expose a read-only flag. */
    if ((p->flags & (1U << 1)) != 0U) return false;
    uint64_t end = (uint64_t)p->pos.offset + p->pos.size;
    if (p->pos.size == 0 || end > g_rom_flashchip.chip_size) return false;
    return true;
}

static bool find_partition_by_label(const char *label, esp_partition_info_t *out)
{
    esp_partition_info_t *table = NULL;
    int count = 0;
    if (!load_partition_table(&table, &count)) return false;
    for (int i = 0; i < count; ++i) {
        char name[17];
        partition_label(&table[i], name);
        if (strcmp(name, label) == 0) {
            if (out != NULL) *out = table[i];
            return true;
        }
    }
    return false;
}

static bool protected_flash_range(uint32_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    uint64_t protected_start = CONFIG_BOOTLOADER_OFFSET_IN_FLASH;
    uint64_t protected_end = (uint64_t)CONFIG_PARTITION_TABLE_OFFSET + BLDR_SECTOR_SIZE;
    return ((uint64_t)addr < protected_end && end > protected_start);
}

static bool find_partition_for_range(uint32_t addr, uint32_t len, esp_partition_info_t *out)
{
    uint64_t end = (uint64_t)addr + len;
    esp_partition_info_t *table = NULL;
    int count = 0;
    if (len == 0 || end > g_rom_flashchip.chip_size || protected_flash_range(addr, len)) return false;
    if (!load_partition_table(&table, &count)) return false;
    for (int i = 0; i < count; ++i) {
        uint64_t pstart = table[i].pos.offset;
        uint64_t pend = pstart + table[i].pos.size;
        if ((uint64_t)addr >= pstart && end <= pend) {
            if (!partition_is_mutable(&table[i])) return false;
            if (out != NULL) *out = table[i];
            return true;
        }
    }
    return false;
}

static bool consume_unlock_for_range(uint32_t addr, uint32_t len, esp_partition_info_t *p)
{
    esp_partition_info_t found;
    char name[17];
    if (!find_partition_for_range(addr, len, &found)) {
        uart_puts("ERR: range is not entirely inside a writable data/app partition.\r\n");
        return false;
    }
    partition_label(&found, name);
    if (g_unlock_armed && g_monitor_elapsed_ms >= g_unlock_deadline_ms) {
        g_unlock_armed = false;
        g_unlock_label[0] = '\0';
        uart_puts("ERR: unlock token expired; run unlock again.\r\n");
        return false;
    }
    if (!g_unlock_armed || strcmp(g_unlock_label, name) != 0) {
        ets_printf("ERR: run 'unlock %s' before this write/erase operation.\r\n", name);
        return false;
    }
    g_unlock_armed = false; /* one-shot authorization */
    g_unlock_label[0] = '\0';
    if (p != NULL) *p = found;
    return true;
}

static bool partition_writes_encrypted(const esp_partition_info_t *p)
{
    if (!esp_flash_encryption_enabled()) return false;
    /* ESP-IDF encrypts application partitions and data partitions explicitly
     * marked encrypted. Plain data partitions remain plaintext. */
    return (p->type == PART_TYPE_APP) || ((p->flags & PART_FLAG_ENCRYPTED) != 0U);
}

static void print_partition_table(void)
{
    esp_partition_info_t *table = NULL;
    int count = 0;
    if (!load_partition_table(&table, &count)) return;
    uart_puts("Label\tType Subtype Offset     Size       Flags\r\n");
    for (int i = 0; i < count; ++i) {
        char name[17];
        partition_label(&table[i], name);
        ets_printf("%s\t0x%02x 0x%02x 0x%08x 0x%08x 0x%08x\r\n",
                   name, (unsigned)table[i].type, (unsigned)table[i].subtype,
                   (unsigned)table[i].pos.offset, (unsigned)table[i].pos.size,
                   (unsigned)table[i].flags);
    }
}

static void cmd_info(void)
{
    rtc_cpu_freq_config_t cfg;
    rtc_clk_cpu_freq_get_config(&cfg);
    ets_printf("ESP32-OS bootloader monitor %s\r\n", BLDR_VERSION);
    uart_puts("Target: ESP32-S3\r\n");
    ets_printf("Reset reason (CPU0): %d\r\n", (int)rtc_get_reset_reason(0));
    ets_printf("Flash JEDEC ID: 0x%06x\r\n", (unsigned)(bootloader_read_flash_id() & 0xFFFFFFU));
    ets_printf("Flash size detected by ROM: %u bytes (0x%x)\r\n",
               (unsigned)g_rom_flashchip.chip_size, (unsigned)g_rom_flashchip.chip_size);
    ets_printf("Partition table offset: 0x%08x\r\n", (unsigned)CONFIG_PARTITION_TABLE_OFFSET);
    ets_printf("CPU frequency: %u MHz\r\n", (unsigned)cfg.freq_mhz);
    ets_printf("UART0 baud: %u\r\n", (unsigned)g_uart_baud);
    ets_printf("RAM transfer buffer: 0x%08x, %u bytes\r\n",
               (unsigned)ram_buffer_base(), (unsigned)sizeof(g_ram_buffer));
    ets_printf("Executable IRAM buffer: 0x%08x, %u bytes\r\n",
               (unsigned)exec_buffer_base(), (unsigned)sizeof(g_exec_payload));
    ets_printf("Unlock: %s\r\n", g_unlock_armed ? g_unlock_label : "none");
#if defined(CONFIG_SECURE_FLASH_ENC_ENABLED) && CONFIG_SECURE_FLASH_ENC_ENABLED
    uart_puts("Flash encryption support: build-enabled\r\n");
#else
    uart_puts("Flash encryption support: build-config default; runtime state checked\r\n");
#endif
}

static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

typedef struct {
    uint32_t h[5];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t used;
} sha1_ctx_t;

static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32U - n)); }

static void sha1_transform(sha1_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999U; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1U; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6U; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->h[0] = 0x67452301U; ctx->h[1] = 0xEFCDAB89U; ctx->h[2] = 0x98BADCFEU;
    ctx->h[3] = 0x10325476U; ctx->h[4] = 0xC3D2E1F0U;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->total_bytes += len;
    while (len > 0) {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take; data += take; len -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20])
{
    uint64_t bits = ctx->total_bytes * 8U;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, 64 - ctx->used);
        sha1_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (unsigned i = 0; i < 8; ++i) ctx->block[63 - i] = (uint8_t)(bits >> (i * 8));
    sha1_transform(ctx, ctx->block);
    for (unsigned i = 0; i < 5; ++i) {
        digest[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

static bool sha1_flash_range(uint32_t addr, uint32_t len, uint8_t digest[20])
{
    sha1_ctx_t ctx;
    if (len == 0 || (addr & 3U) != 0U || (len & 3U) != 0U ||
        (uint64_t)addr + len > g_rom_flashchip.chip_size) return false;
    sha1_init(&ctx);
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > BLDR_FLASH_IO_CHUNK) n = BLDR_FLASH_IO_CHUNK;
        if (bootloader_flash_read(addr + done, g_io_buffer, n, true) != ESP_OK) return false;
        sha1_update(&ctx, g_io_buffer, n);
        done += n;
        feed_watchdog();
    }
    sha1_final(&ctx, digest);
    return true;
}

static void print_sha1(const uint8_t digest[20])
{
    for (unsigned i = 0; i < 20; ++i) ets_printf("%02x", digest[i]);
    uart_puts("\r\n");
}

static bool check_flash_encryption_alignment(const esp_partition_info_t *p, uint32_t addr, uint32_t len)
{
    if (partition_writes_encrypted(p) && (((addr | len) & 31U) != 0U)) {
        uart_puts("ERR: encrypted writes require flash address and length aligned to 32 bytes on ESP-IDF 4.4.7.\r\n");
        return false;
    }
    return true;
}

static bool flash_write_from_ram(uint32_t flash_addr, uint32_t ram_addr, uint32_t len)
{
    esp_partition_info_t p;
    bool exec_source = false;
    if (len == 0 || (flash_addr & 3U) != 0U || (ram_addr & 3U) != 0U || (len & 3U) != 0U) {
        uart_puts("ERR: flash address, RAM address and length must be 4-byte aligned; len must be nonzero.\r\n");
        return false;
    }
    if (!range_in_buffer(ram_addr, len, false, &exec_source)) {
        uart_puts("ERR: source must lie entirely in the monitor's RAM/IRAM buffers (see info).\r\n");
        return false;
    }
    if (!find_partition_for_range(flash_addr, len, &p)) {
        uart_puts("ERR: destination must lie entirely in a writable partition, outside the bootloader/table.\r\n");
        return false;
    }
    if (!check_flash_encryption_alignment(&p, flash_addr, len)) return false;
    if (!consume_unlock_for_range(flash_addr, len, &p)) return false;

    bool encrypt = partition_writes_encrypted(&p);
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > BLDR_FLASH_IO_CHUNK) n = BLDR_FLASH_IO_CHUNK;
        if (encrypt) n &= ~31U;
        if (n == 0) return false;
        bool ignored_exec = false;
        if (!range_in_buffer(ram_addr + done, n, false, &ignored_exec)) return false;
        /* Copy into aligned scratch. Encrypted ROM writes transform the source
         * buffer in place, so never pass the caller's buffer directly. */
        memcpy(g_io_buffer, (const void *)(uintptr_t)(ram_addr + done), n);
        esp_err_t err = bootloader_flash_write(flash_addr + done, g_io_buffer, n, encrypt);
        if (err != ESP_OK) {
            ets_printf("ERR: flash write failed at 0x%08x (0x%x); target may be partially written.\r\n",
                       (unsigned)(flash_addr + done), (unsigned)err);
            return false;
        }
        done += n;
        feed_watchdog();
    }
    ets_printf("OK: wrote %u bytes to flash 0x%08x. Run verify if this is an app image.\r\n",
               (unsigned)len, (unsigned)flash_addr);
    (void)exec_source;
    return true;
}

static bool flash_erase_blocks(uint32_t addr, uint32_t blocks)
{
    uint64_t len64 = (uint64_t)blocks * BLDR_SECTOR_SIZE;
    if (blocks == 0 || len64 > UINT32_MAX || (addr & (BLDR_SECTOR_SIZE - 1U)) != 0U) {
        uart_puts("ERR: erase expects a sector-aligned address and a positive sector count.\r\n");
        return false;
    }
    uint32_t len = (uint32_t)len64;
    if (!consume_unlock_for_range(addr, len, NULL)) return false;
    uint32_t n = 0;
    while (n < blocks) {
        esp_err_t err = bootloader_flash_erase_sector((addr / BLDR_SECTOR_SIZE) + n);
        if (err != ESP_OK) {
            ets_printf("ERR: erase failed at 0x%08x (0x%x); range may be partially erased.\r\n",
                       (unsigned)(addr + n * BLDR_SECTOR_SIZE), (unsigned)err);
            return false;
        }
        ++n;
        feed_watchdog();
    }
    ets_printf("OK: erased %u sector(s) at 0x%08x (%u bytes).\r\n",
               (unsigned)blocks, (unsigned)addr, (unsigned)len);
    return true;
}

static bool flash_write_packet(uint32_t addr, const uint8_t *data, uint32_t len,
                               const esp_partition_info_t *partition, uint32_t *last_erased_sector)
{
    uint32_t done = 0;
    bool encrypt = partition_writes_encrypted(partition);
    if (((addr | len) & 3U) != 0U) return false;
    if (!check_flash_encryption_alignment(partition, addr, len)) return false;
    while (done < len) {
        uint32_t current = addr + done;
        uint32_t sector = current / BLDR_SECTOR_SIZE;
        uint32_t in_sector = current & (BLDR_SECTOR_SIZE - 1U);
        uint32_t n = len - done;
        if (n > BLDR_SECTOR_SIZE - in_sector) n = BLDR_SECTOR_SIZE - in_sector;
        if (sector != *last_erased_sector) {
            esp_err_t er = bootloader_flash_erase_sector(sector);
            if (er != ESP_OK) {
                ets_printf("ERR: sector erase failed at 0x%08x (0x%x).\r\n", (unsigned)(sector * BLDR_SECTOR_SIZE), (unsigned)er);
                return false;
            }
            *last_erased_sector = sector;
            feed_watchdog();
        }
        if (encrypt && ((current | n) & 31U) != 0U) {
            uart_puts("ERR: XMODEM encrypted-write chunk is not 32-byte aligned.\r\n");
            return false;
        }
        uint32_t pos = 0;
        while (pos < n) {
            uint32_t wn = n - pos;
            if (wn > BLDR_FLASH_IO_CHUNK) wn = BLDR_FLASH_IO_CHUNK;
            if (encrypt) wn &= ~31U;
            if (wn == 0) return false;
            /* XMODEM receives into g_io_buffer; use a separate scratch buffer to avoid
             * overlapping source/destination when programming the received packet. */
            if (wn > sizeof(g_write_buffer)) return false;
            memcpy(g_write_buffer, data + done + pos, wn);
            esp_err_t wr = bootloader_flash_write(current + pos, g_write_buffer, wn, encrypt);
            if (wr != ESP_OK) {
                ets_printf("ERR: XMODEM flash write failed at 0x%08x (0x%x).\r\n", (unsigned)(current + pos), (unsigned)wr);
                return false;
            }
            pos += wn;
            feed_watchdog();
        }
        done += n;
    }
    return true;
}

static bool xmodem_receive(uint32_t ram_addr, uint32_t max_len,
                           bool to_flash, uint32_t flash_addr,
                           const esp_partition_info_t *flash_partition,
                           uint32_t *out_bytes)
{
    bool to_exec = false;
    if (!to_flash && ((max_len & 127U) != 0U || max_len == 0 ||
                      !range_in_buffer(ram_addr, max_len, true, &to_exec) || (ram_addr & 3U) != 0U)) {
        uart_puts("ERR: XMODEM RAM target must be a monitor buffer; length must be a nonzero multiple of 128.\r\n");
        return false;
    }
    if (to_flash && (max_len == 0 || (max_len & 127U) != 0U || flash_partition == NULL ||
                     !partition_is_mutable(flash_partition) ||
                     (uint64_t)flash_addr + max_len > (uint64_t)flash_partition->pos.offset + flash_partition->pos.size ||
                     (flash_addr & (BLDR_SECTOR_SIZE - 1U)) != 0U)) {
        uart_puts("ERR: XMODEM flash target must be sector-aligned and the max length a multiple of 128, within one partition.\r\n");
        return false;
    }
    uint32_t received = 0;
    uint8_t expected_block = 1;
    uint8_t retries = 0;
    uint32_t last_erased_sector = UINT32_MAX;
    uart_puts("XMODEM/CRC mode: start sending the file now; Ctrl+C from sender usually cancels.\r\n");
    uart_putc((char)XMODEM_CRC_REQ);
    for (;;) {
        uint8_t c = 0;
        if (!read_byte_timeout(&c, BLDR_XMODEM_IDLE_TIMEOUT)) {
            if (++retries > BLDR_XMODEM_RETRY_MAX) {
                uart_putc((char)XMODEM_CAN); uart_putc((char)XMODEM_CAN);
                uart_puts("\r\nERR: XMODEM timed out.\r\n");
                return false;
            }
            uart_putc((char)(expected_block == 1U ? XMODEM_CRC_REQ : XMODEM_NAK));
            continue;
        }
        if (c == XMODEM_EOT) {
            uart_putc((char)XMODEM_ACK);
            if (out_bytes != NULL) *out_bytes = received;
            ets_printf("\r\nXMODEM complete: %u packet bytes received.\r\n", (unsigned)received);
            if (!to_flash && to_exec) {
                g_exec_payload_valid = received > 0;
                g_exec_payload_start = ram_addr;
                g_exec_payload_len = received;
            }
            return received > 0;
        }
        if (c == XMODEM_CAN) {
            (void)read_byte_timeout(&c, 100);
            uart_puts("\r\nXMODEM cancelled by sender.\r\n");
            return false;
        }
        uint32_t packet_len;
        if (c == XMODEM_SOH) packet_len = 128U;
        else if (c == XMODEM_STX) packet_len = 1024U;
        else { uart_putc((char)XMODEM_NAK); continue; }

        uint8_t block, block_inv, crc_hi, crc_lo;
        bool got = read_byte_timeout(&block, BLDR_XMODEM_BYTE_TIMEOUT) &&
                   read_byte_timeout(&block_inv, BLDR_XMODEM_BYTE_TIMEOUT);
        for (uint32_t i = 0; got && i < packet_len; ++i) {
            got = read_byte_timeout(&g_io_buffer[i], BLDR_XMODEM_BYTE_TIMEOUT);
        }
        if (got) got = read_byte_timeout(&crc_hi, BLDR_XMODEM_BYTE_TIMEOUT) &&
                       read_byte_timeout(&crc_lo, BLDR_XMODEM_BYTE_TIMEOUT);
        if (!got) { uart_putc((char)XMODEM_NAK); if (++retries > BLDR_XMODEM_RETRY_MAX) return false; continue; }
        uint16_t want_crc = (uint16_t)(((uint16_t)crc_hi << 8) | crc_lo);
        if ((uint8_t)(block + block_inv) != 0xFFU || crc16_xmodem(g_io_buffer, packet_len) != want_crc) {
            uart_putc((char)XMODEM_NAK);
            if (++retries > BLDR_XMODEM_RETRY_MAX) return false;
            continue;
        }
        if (block == (uint8_t)(expected_block - 1U)) {
            uart_putc((char)XMODEM_ACK); /* Duplicate packet after a lost ACK */
            retries = 0;
            continue;
        }
        if (block != expected_block) {
            uart_putc((char)XMODEM_NAK);
            if (++retries > BLDR_XMODEM_RETRY_MAX) return false;
            continue;
        }
        if ((uint64_t)received + packet_len > max_len) {
            uart_putc((char)XMODEM_CAN); uart_putc((char)XMODEM_CAN);
            uart_puts("\r\nERR: transfer exceeded the supplied length.\r\n");
            return false;
        }
        bool ok;
        if (to_flash) {
            ok = flash_write_packet(flash_addr + received, g_io_buffer, packet_len,
                                    flash_partition, &last_erased_sector);
        } else {
            ok = copy_to_ram(ram_addr + received, g_io_buffer, packet_len);
        }
        if (!ok) {
            uart_putc((char)XMODEM_CAN); uart_putc((char)XMODEM_CAN);
            uart_puts("\r\nERR: could not store XMODEM packet; destination may be partially written.\r\n");
            return false;
        }
        received += packet_len;
        ++expected_block;
        retries = 0;
        uart_putc((char)XMODEM_ACK);
        feed_watchdog();
    }
}

static bool verify_image(uint32_t addr, uint32_t len)
{
    if (len == 0 || (addr & 0xFFFU) != 0U ||
        (uint64_t)addr + len > g_rom_flashchip.chip_size || protected_flash_range(addr, len)) {
        uart_puts("ERR: invalid image range; start address must be 4 KiB aligned and outside bootloader/partition table.\r\n");
        return false;
    }
    esp_partition_pos_t pos = { .offset = addr, .size = len };
    esp_image_metadata_t metadata;
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &metadata);
    if (err != ESP_OK) {
        ets_printf("VERIFY FAIL: ESP-IDF image validator returned 0x%x.\r\n", (unsigned)err);
        return false;
    }
    ets_printf("VERIFY OK: ESP image, %u bytes; segments=%u; entry=0x%08x.\r\n",
               (unsigned)metadata.image_len, (unsigned)metadata.image.segment_count,
               (unsigned)metadata.image.entry_addr);
    return true;
}

static void print_help(void)
{
    uart_puts("ESP32-OS bootloader monitor (ESP-IDF v4.4.7 / ESP32-S3)\r\n");
    uart_puts("  help [command] | <command> --help | <command> -h\r\n");
    uart_puts("  info                              Chip, flash, clocks and RAM buffers\r\n");
    uart_puts("  partitions | mtd                  Validate/list the on-flash partition table\r\n");
    uart_puts("  verify <label>                    Verify an app image in a named partition\r\n");
    uart_puts("  verify <flash_addr> <len>         Verify an image at a flash address\r\n");
    uart_puts("  sha1sum -a <flash_addr> <len>     SHA-1 a raw flash range (integrity only)\r\n");
    uart_puts("  sha1sum -p <label>                SHA-1 a named partition (integrity only)\r\n");
    uart_puts("  unlock <label>                    Arm one destructive operation for that partition\r\n");
    uart_puts("  erase <flash_addr> <blocks>       Erase blocks of 4096 bytes; requires unlock\r\n");
    uart_puts("  flash <flash_addr> <ram> <len>    Write an already-erased range; requires unlock\r\n");
    uart_puts("  xmdm <ram_addr> <max_len>         XMODEM/CRC receive to a monitor RAM buffer\r\n");
    uart_puts("  xmdm flash <label> <offset> <max_len>  Stream XMODEM into partition; requires unlock\r\n");
    uart_puts("  dump <flash_addr> <len> <ram_addr> Copy flash to a monitor RAM/IRAM buffer\r\n");
    uart_puts("  jump <ram_entry_addr>              Call an entry in a loaded executable IRAM payload\r\n");
    uart_puts("  memrl <count> <ram_addr>          Read count 32-bit words in monitor buffers\r\n");
    uart_puts("  memwl <value> <ram_addr>           Write one 32-bit word in monitor buffers\r\n");
    uart_puts("  cpufreq <40|80|160|240>            Set bootloader CPU frequency (MHz)\r\n");
    uart_puts("  baud <rate>                        Set UART0 baud (9600..921600 supported rates)\r\n");
    uart_puts("  version | go | reboot              Version, continue ESP-IDF boot, reset chip\r\n");
    uart_puts("  sha1sum -a <addr> <len> | -p <label> Hash raw flash/partition (integrity only)\r\n");
    uart_puts("Address/length values accept decimal or 0x-prefixed hex.\r\n");
    uart_puts("Run info first: only the monitor's printed RAM/IRAM buffers are accepted.\r\n");
}

static void print_command_help(const char *cmd)
{
    if (strcmp(cmd, "info") == 0) uart_puts("info: print chip/flash ID and size, CPU frequency, partition offset and usable RAM buffer addresses.\r\n");
    else if (strcmp(cmd, "partitions") == 0 || strcmp(cmd, "mtd") == 0) uart_puts("partitions/mtd: read and validate the partition table at CONFIG_PARTITION_TABLE_OFFSET, then list label/type/subtype/offset/size/flags.\r\n");
    else if (strcmp(cmd, "verify") == 0) uart_puts("verify <label> OR verify <flash_addr> <len>: use ESP-IDF image validation (checksum, appended SHA-256 and configured signature checks). Intended for ESP app images.\r\n");
    else if (strcmp(cmd, "flash") == 0) uart_puts("flash <flash_addr> <ram_addr> <len>: program a previously erased range from a monitor buffer. Must unlock its partition first.\r\n");
    else if (strcmp(cmd, "erase") == 0) uart_puts("erase <flash_addr> <blocks>: erase blocks * 4096 bytes. Address must be sector-aligned and the range inside one unlocked partition.\r\n");
    else if (strcmp(cmd, "unlock") == 0) uart_puts("unlock <label>: arm exactly one later flash/erase/XMODEM-to-flash operation for a writable partition; bootloader/table are never unlockable.\r\n");
    else if (strcmp(cmd, "dump") == 0) uart_puts("dump <flash_addr> <len> <ram_addr>: flash source/length must be 4-byte aligned and destination must fit wholly in a monitor RAM/IRAM buffer.\r\n");
    else if (strcmp(cmd, "xmdm") == 0) uart_puts("xmdm <ram_addr> <max_len> receives XMODEM/CRC into a monitor RAM buffer. xmdm flash <label> <offset> <max_len> streams directly to a partition and erases sectors on demand. Max length must be a multiple of 128; XMODEM padding may be present at the end of the received payload.\r\n");
    else if (strcmp(cmd, "jump") == 0) uart_puts("jump <ram_addr>: call a payload entry inside the loaded executable IRAM buffer. Payload must be built for that address and return/handle reset correctly.\r\n");
    else if (strcmp(cmd, "sha1sum") == 0) uart_puts("sha1sum -a <flash_addr> <len> hashes raw flash bytes; sha1sum -p <label> hashes a partition. SHA-1 is not authentication.\r\n");
    else if (strcmp(cmd, "memrl") == 0) uart_puts("memrl <count> <ram_addr>: print count 32-bit words. Reads are restricted to monitor RAM/IRAM buffers.\r\n");
    else if (strcmp(cmd, "memwl") == 0) uart_puts("memwl <value> <ram_addr>: write one 32-bit word in a monitor RAM/IRAM buffer.\r\n");
    else if (strcmp(cmd, "cpufreq") == 0) uart_puts("cpufreq <40|80|160|240>: switch CPU clock using ESP-IDF RTC clock configuration.\r\n");
    else if (strcmp(cmd, "baud") == 0) uart_puts("baud <rate>: supported rates: 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600. Set host terminal to match.\r\n");
    else if (strcmp(cmd, "go") == 0) uart_puts("go: return from the hook; ESP-IDF continues partition selection, app validation and loading.\r\n");
    else if (strcmp(cmd, "reboot") == 0) uart_puts("reboot: software reset the ESP32-S3.\r\n");
    else if (strcmp(cmd, "version") == 0) uart_puts("version: show the bootloader monitor source version.\r\n");
    else uart_puts("ERR: unknown command. Use help.\r\n");
}

static bool read_line(char *line, size_t cap)
{
    size_t len = 0;
    while (g_monitor_elapsed_ms < BLDR_MONITOR_MAX_MS) {
        uint8_t c = 0;
        if (uart_rx_one_char(&c) == 0) {
            if (g_skip_lf) {
                g_skip_lf = false;
                if (c == '\n') continue;
            }
            if (c == '\r' || c == '\n') {
                if (c == '\r') g_skip_lf = true;
                uart_puts("\r\n");
                line[len] = '\0';
                return true;
            }
            if (c == 0x03U) {
                uart_puts("^C\r\nLeaving monitor.\r\n");
                return false;
            }
            if (c == 0x08U || c == 0x7FU) {
                if (len > 0) { --len; uart_puts("\b \b"); }
                continue;
            }
            if (c < 0x20U || c > 0x7EU) {
                uart_puts("\r\nERR: ASCII commands only; line discarded.\r\n");
                len = 0;
                prompt();
                continue;
            }
            if (len + 1U >= cap) {
                uart_puts("\r\nERR: line too long; line discarded.\r\n");
                len = 0;
                prompt();
                continue;
            }
            line[len++] = (char)c;
            uart_putc((char)c);
        }
        delay_1ms();
    }
    uart_puts("\r\nMonitor time limit reached; continuing normal boot.\r\n");
    return false;
}

static bool process_command(int argc, char **argv, bool *leave_monitor)
{
    uint32_t a = 0, b = 0, c = 0;
    if (argc == 0) return true;
    if (command_has_help(argc, argv)) { print_command_help(argv[0]); return true; }
    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) {
        if (argc == 1) print_help(); else print_command_help(argv[1]);
    } else if (strcmp(cmd, "version") == 0) {
        ets_printf("ESP32-OS bootloader monitor %s\r\n", BLDR_VERSION);
    } else if (strcmp(cmd, "info") == 0) {
        cmd_info();
    } else if (strcmp(cmd, "partitions") == 0 || strcmp(cmd, "mtd") == 0) {
        print_partition_table();
    } else if (strcmp(cmd, "go") == 0) {
        uart_puts("Continuing normal ESP-IDF boot...\r\n");
        *leave_monitor = true;
    } else if (strcmp(cmd, "reboot") == 0) {
        uart_puts("Rebooting...\r\n");
        ets_delay_us(20000);
        software_reset();
        for (;;) { }
    } else if (strcmp(cmd, "unlock") == 0) {
        /* A new unlock attempt always replaces/disarms any previous token. */
        g_unlock_armed = false;
        g_unlock_label[0] = '\0';
        if (argc != 2) { uart_puts("Usage: unlock <partition-label>\r\n"); return true; }
        esp_partition_info_t p;
        if (!find_partition_by_label(argv[1], &p)) { uart_puts("ERR: partition not found or partition table invalid.\r\n"); return true; }
        if (!partition_is_mutable(&p)) { uart_puts("ERR: critical/read-only partition cannot be unlocked.\r\n"); return true; }
        if (strlen(argv[1]) > 16U) { uart_puts("ERR: label exceeds ESP-IDF's 16-character partition label limit.\r\n"); return true; }
        strncpy(g_unlock_label, argv[1], sizeof(g_unlock_label) - 1U);
        g_unlock_label[sizeof(g_unlock_label) - 1U] = '\0';
        g_unlock_deadline_ms = g_monitor_elapsed_ms + 30000U;
        g_unlock_armed = true;
        ets_printf("OK: '%s' armed for one flash/erase/XMODEM-to-flash operation (30 s timeout).\r\n", g_unlock_label);
    } else if (strcmp(cmd, "erase") == 0) {
        if (argc != 3 || !parse_u32(argv[1], &a) || !parse_u32(argv[2], &b)) {
            uart_puts("Usage: erase <flash_addr> <blocks-of-4096-bytes>\r\n"); return true;
        }
        (void)flash_erase_blocks(a, b);
    } else if (strcmp(cmd, "flash") == 0) {
        if (argc != 4 || !parse_u32(argv[1], &a) || !parse_u32(argv[2], &b) || !parse_u32(argv[3], &c)) {
            uart_puts("Usage: flash <flash_addr> <ram_addr> <len>\r\n"); return true;
        }
        (void)flash_write_from_ram(a, b, c);
    } else if (strcmp(cmd, "dump") == 0) {
        if (argc != 4 || !parse_u32(argv[1], &a) || !parse_u32(argv[2], &b) || !parse_u32(argv[3], &c)) {
            uart_puts("Usage: dump <flash_addr> <len> <ram_addr>\r\n"); return true;
        }
        bool dst_exec = false;
        if (b == 0 || (a & 3U) != 0U || (b & 3U) != 0U || (c & 3U) != 0U ||
            (uint64_t)a + b > g_rom_flashchip.chip_size || !range_in_buffer(c, b, true, &dst_exec)) {
            uart_puts("ERR: invalid range; use 4-byte alignment and a destination in a printed monitor buffer.\r\n"); return true;
        }
        uint32_t done = 0;
        while (done < b) {
            uint32_t n = b - done;
            if (n > BLDR_FLASH_IO_CHUNK) n = BLDR_FLASH_IO_CHUNK;
            if (bootloader_flash_read(a + done, g_io_buffer, n, true) != ESP_OK || !copy_to_ram(c + done, g_io_buffer, n)) {
                uart_puts("ERR: dump failed.\r\n"); return true;
            }
            done += n; feed_watchdog();
        }
        if (dst_exec) { g_exec_payload_valid = true; g_exec_payload_start = c; g_exec_payload_len = b; }
        ets_printf("OK: copied %u bytes from flash 0x%08x to RAM 0x%08x.\r\n", (unsigned)b, (unsigned)a, (unsigned)c);
    } else if (strcmp(cmd, "verify") == 0) {
        if (argc == 2) {
            esp_partition_info_t p;
            if (!find_partition_by_label(argv[1], &p)) { uart_puts("ERR: partition not found or partition table invalid.\r\n"); return true; }
            if (p.type != PART_TYPE_APP) { uart_puts("ERR: verify <label> accepts application partitions; use verify <addr> <len> for a raw image range.\r\n"); return true; }
            (void)verify_image(p.pos.offset, p.pos.size);
        } else if (argc == 3 && parse_u32(argv[1], &a) && parse_u32(argv[2], &b)) {
            (void)verify_image(a, b);
        } else {
            uart_puts("Usage: verify <app-partition-label> OR verify <flash_addr> <len>\r\n");
        }
    } else if (strcmp(cmd, "sha1sum") == 0) {
        uint8_t digest[20];
        if (argc == 4 && strcmp(argv[1], "-a") == 0 && parse_u32(argv[2], &a) && parse_u32(argv[3], &b)) {
            if (!sha1_flash_range(a, b, digest)) uart_puts("ERR: SHA-1 range invalid/read failed; address and length must be 4-byte aligned.\r\n");
            else print_sha1(digest);
        } else if (argc == 3 && strcmp(argv[1], "-p") == 0) {
            esp_partition_info_t p;
            if (!find_partition_by_label(argv[2], &p)) uart_puts("ERR: partition not found or partition table invalid.\r\n");
            else if (!sha1_flash_range(p.pos.offset, p.pos.size, digest)) uart_puts("ERR: SHA-1 read failed.\r\n");
            else { print_sha1(digest); ets_printf("Partition: %s\r\n", argv[2]); }
        } else {
            uart_puts("Usage: sha1sum -a <flash_addr> <len> OR sha1sum -p <partition-label>\r\n");
        }
    } else if (strcmp(cmd, "xmdm") == 0) {
        uint32_t n = 0;
        if (argc == 3 && parse_u32(argv[1], &a) && parse_u32(argv[2], &b)) {
            (void)xmodem_receive(a, b, false, 0, NULL, &n);
        } else if (argc == 5 && strcmp(argv[1], "flash") == 0 && parse_u32(argv[3], &b) && parse_u32(argv[4], &c)) {
            esp_partition_info_t p;
            if (!find_partition_by_label(argv[2], &p)) { uart_puts("ERR: partition not found or partition table invalid.\r\n"); return true; }
            if (!partition_is_mutable(&p)) { uart_puts("ERR: partition is read-only.\r\n"); return true; }
            if (c == 0 || (c & 127U) != 0U || (uint64_t)b + c > p.pos.size ||
                b > UINT32_MAX - p.pos.offset ||
                ((p.pos.offset + b) & (BLDR_SECTOR_SIZE - 1U)) != 0U) {
                uart_puts("ERR: XMODEM range must be sector-aligned, length a nonzero multiple of 128, and fit inside the partition.\r\n"); return true;
            }
            if (!consume_unlock_for_range(p.pos.offset + b, c, NULL)) return true;
            (void)xmodem_receive(0, c, true, p.pos.offset + b, &p, &n);
            if (n > 0) ets_printf("Partial/complete flash transfer touched %u bytes at 0x%08x; run verify on app images.\r\n", (unsigned)n, (unsigned)(p.pos.offset + b));
        } else {
            uart_puts("Usage: xmdm <ram_addr> <max_len> OR xmdm flash <label> <offset> <max_len>\r\n");
        }
    } else if (strcmp(cmd, "jump") == 0) {
        if (argc != 2 || !parse_u32(argv[1], &a)) { uart_puts("Usage: jump <ram_entry_addr>\r\n"); return true; }
        uint64_t end = (uint64_t)g_exec_payload_start + g_exec_payload_len;
        if (!g_exec_payload_valid || (a & 3U) != 0U || a < g_exec_payload_start || (uint64_t)a >= end ||
            a < exec_buffer_base() || (uint64_t)a >= (uint64_t)exec_buffer_base() + sizeof(g_exec_payload)) {
            uart_puts("ERR: entry must be 4-byte aligned and inside a successfully loaded executable IRAM buffer.\r\n"); return true;
        }
        ets_printf("Jumping to RAM payload at 0x%08x. A returning payload will return to the monitor.\r\n", (unsigned)a);
        ets_delay_us(10000);
        __asm__ __volatile__("memw" ::: "memory");
        ((void (*)(void))(uintptr_t)a)();
        uart_puts("RAM payload returned.\r\n");
    } else if (strcmp(cmd, "memrl") == 0) {
        if (argc != 3 || !parse_u32(argv[1], &a) || !parse_u32(argv[2], &b) || a == 0 || a > 64U || (b & 3U) != 0U) {
            uart_puts("Usage: memrl <count-of-32-bit-words> <ram_addr> (count 1..64)\r\n"); return true;
        }
        bool ignored = false;
        uint32_t len = a * 4U;
        if (!range_in_buffer(b, len, false, &ignored)) { uart_puts("ERR: address range is outside the monitor buffers.\r\n"); return true; }
        volatile const uint32_t *p = (volatile const uint32_t *)(uintptr_t)b;
        for (uint32_t i = 0; i < a; ++i) {
            ets_printf("0x%08x: 0x%08x%s", (unsigned)(b + i * 4U), (unsigned)p[i], ((i & 3U) == 3U || i + 1U == a) ? "\r\n" : "  ");
        }
    } else if (strcmp(cmd, "memwl") == 0) {
        if (argc != 3 || !parse_u32(argv[1], &a) || !parse_u32(argv[2], &b) || (b & 3U) != 0U) {
            uart_puts("Usage: memwl <32-bit-value> <ram_addr>\r\n"); return true;
        }
        bool is_exec = false;
        if (!range_in_buffer(b, sizeof(uint32_t), true, &is_exec)) { uart_puts("ERR: address is outside the monitor buffers.\r\n"); return true; }
        *(volatile uint32_t *)(uintptr_t)b = a;
        __asm__ __volatile__("memw" ::: "memory");
        if (is_exec) g_exec_payload_valid = false;
        ets_printf("OK: [0x%08x] = 0x%08x\r\n", (unsigned)b, (unsigned)a);
    } else if (strcmp(cmd, "cpufreq") == 0) {
        if (argc != 2 || !parse_u32(argv[1], &a)) { uart_puts("Usage: cpufreq <40|80|160|240>\r\n"); return true; }
        if (a != 40U && a != 80U && a != 160U && a != 240U) { uart_puts("ERR: supported CPU frequencies are 40, 80, 160, 240 MHz.\r\n"); return true; }
        rtc_cpu_freq_config_t cfg;
        if (!rtc_clk_cpu_freq_mhz_to_config(a, &cfg)) { uart_puts("ERR: clock configuration not supported by this revision/configuration.\r\n"); return true; }
        rtc_clk_cpu_freq_set_config(&cfg);
        rtc_clk_cpu_freq_get_config(&cfg);
        set_uart_baud(g_uart_baud); /* APB frequency may change with CPU clock selection. */
        ets_printf("CPU frequency now %u MHz; UART re-tuned to %u baud.\r\n",
                   (unsigned)cfg.freq_mhz, (unsigned)g_uart_baud);
    } else if (strcmp(cmd, "baud") == 0) {
        static const uint32_t rates[] = {9600U, 19200U, 38400U, 57600U, 115200U, 230400U, 460800U, 921600U};
        bool supported = false;
        if (argc != 2 || !parse_u32(argv[1], &a)) { uart_puts("Usage: baud <9600|19200|38400|57600|115200|230400|460800|921600>\r\n"); return true; }
        for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) if (a == rates[i]) supported = true;
        if (!supported) { uart_puts("ERR: unsupported baud rate.\r\n"); return true; }
        set_uart_baud(a);
        ets_printf("UART0 baud changed to %u; set the terminal to the same rate.\r\n", (unsigned)a);
    } else {
        uart_puts("ERR: unknown command. Use 'help'.\r\n");
    }
    return true;
}

void bootloader_hooks_include(void)
{
    /* Strong symbol forces the static library containing the hook into the link. */
}

void bootloader_before_init(void)
{
    /* Keep the pre-init hook empty; ROM flash/UART state is not yet fully ready. */
}

void bootloader_after_init(void)
{
    uint32_t waited_ms = 0;
    uint8_t c = 0;
    ets_printf("\r\nESP32-OS bootloader monitor %s\r\n", BLDR_VERSION);
    ets_printf("Press any key within %u ms to enter bldr> (otherwise boot normally).\r\n", (unsigned)BLDR_TRIGGER_MS);
    while (waited_ms < BLDR_TRIGGER_MS) {
        if (uart_rx_one_char(&c) == 0) {
            char line[BLDR_LINE_MAX];
            (void)c; /* Consumes the key used to enter the monitor. */
            uart_puts("\r\nMonitor entered. Type 'help'.\r\n");
            prompt();
            while (g_monitor_elapsed_ms < BLDR_MONITOR_MAX_MS) {
                if (!read_line(line, sizeof(line))) break;
                char *argv[BLDR_ARG_MAX];
                int argc = split_args(line, argv, BLDR_ARG_MAX);
                if (argc < 0) {
                    uart_puts("ERR: too many command arguments.\r\n");
                } else {
                    bool leave_monitor = false;
                    (void)process_command(argc, argv, &leave_monitor);
                    if (leave_monitor) return;
                }
                prompt();
            }
            uart_puts("Monitor exited; continuing normal ESP-IDF boot.\r\n");
            return;
        }
        ets_delay_us(1000);
        ++waited_ms;
        if ((waited_ms & 0xFFU) == 0U) feed_watchdog();
    }
}
