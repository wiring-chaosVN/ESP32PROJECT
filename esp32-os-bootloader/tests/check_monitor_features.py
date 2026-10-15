from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "bootloader_components/bldr_hooks/hooks.c").read_text()
readme = (root / "README.md").read_text()

required_handlers = [
    "info", "partitions", "mtd", "verify", "flash", "erase", "unlock",
    "memwl", "dump", "jump", "memrl", "xmdm", "sha1sum", "cpufreq", "baud",
]
missing = []
for name in required_handlers:
    if f'strcmp(cmd, "{name}")' not in source:
        missing.append(name)
if missing:
    print("Missing command handlers:", ", ".join(missing), file=sys.stderr)
    sys.exit(1)

for token in [
    "find_partition_for_range", "consume_unlock_for_range",
    "bootloader_flash_erase_sector", "bootloader_flash_write",
    "esp_image_verify", "range_in_buffer",
]:
    if token not in source:
        print(f"Missing required safety/implementation token: {token}", file=sys.stderr)
        sys.exit(1)

help_block = source[source.index("static void print_help(void)"):
                    source.index("static void print_command_help")]
for name in required_handlers:
    if name not in help_block:
        print(f"Command '{name}' missing from top-level help", file=sys.stderr)
        sys.exit(1)

if '#define BLDR_VERSION             "1.1.0"' not in source:
    print("Source version mismatch", file=sys.stderr)
    sys.exit(1)
if "# ESP32-OS Bootloader v1.1.0" not in readme:
    print("README version mismatch", file=sys.stderr)
    sys.exit(1)

print(f"PASS: {len(required_handlers)} command names, write/erase guards, image verifier and version metadata")
