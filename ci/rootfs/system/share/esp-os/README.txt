ESP-OS shared system data.

This ROMFS payload is read-only. It carries version, console, mount and
development security policy files. Runtime mounting of the external rootfs
MTD at /system is not yet implemented by this build pipeline.
