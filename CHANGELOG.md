# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Changed
- Updated the `ESPJsonDB` integration guidance to use the optional `ESPJsonDBCompressor.h` bridge instead of the manual snapshot-to-string roundtrip.
- Unified same-instance busy-state handling across synchronous and asynchronous jobs.
- Tightened `.esc` decompression validation to reject oversized or malformed block headers before allocation.
- Released transactional buffer-sink staging memory immediately after successful commits.
- Narrowed the default public header surface and moved `.esc` format internals behind the explicit `ESPCompressorFormat.h` opt-in header.
- Changed `FileSink` commit to use best-effort replacement with rollback so rename failures preserve the previous destination.
- Documented async callback execution context and `StreamSource` zero-byte-read EOF behavior more explicitly.

### Fixed
- CI now pins PIOArduino Core to `v6.1.19` and installs the ESP32 platform via `pio pkg install`, restoring PlatformIO compatibility with the current `platform-espressif32` package.

## [1.0.0] - 2026-04-01

### Added
- Initial `ESPCompressor` v1 release with synchronous and asynchronous compression/decompression APIs.
- Stable `.esc` container with block metadata, CRC32 checks, and raw-block fallback.
- Built-in buffer, file, stream, and print adapters with transactional sink behavior.
- ESPBufferManager-backed staging buffers for PSRAM-aware operation.
- Host test suite, Arduino examples, CI workflow, and release workflow.
