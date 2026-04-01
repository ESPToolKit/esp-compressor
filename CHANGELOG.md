# Changelog

All notable changes to this project will be documented in this file.

## [1.0.0] - 2026-04-01

### Added
- Initial `ESPCompressor` v1 release with synchronous and asynchronous compression/decompression APIs.
- Stable `.esc` container with block metadata, CRC32 checks, and raw-block fallback.
- Built-in buffer, file, stream, and print adapters with transactional sink behavior.
- ESPBufferManager-backed staging buffers for PSRAM-aware operation.
- Host test suite, Arduino examples, CI workflow, and release workflow.
