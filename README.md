# ESPCompressor

ESPCompressor is an async-first compression library for ESP32 projects that need predictable memory usage, safe backup files, and a clean API for buffers, filesystem paths, and custom streams. The v1 codec uses a lightweight block-based `.esc` container with CRC32 checks and a simple LZ-style compressor tuned for embedded restore flows rather than maximum compression ratio.

## CI / Release / License
[![CI](https://github.com/ESPToolKit/esp-compressor/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-compressor/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-compressor?sort=semver)](https://github.com/ESPToolKit/esp-compressor/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features
- Compress and decompress buffers, files, and Arduino `Stream` / `Print` adapters.
- Public synchronous APIs plus async background execution with progress and completion callbacks.
- Stable `.esc` container with versioned header, block records, CRC32 checks, and raw-block fallback.
- Transactional sinks for buffer and file outputs so failed jobs do not silently publish partial data.
- Cooperative cancellation and clean busy-state rejection.
- PSRAM-aware staging buffers through `ESPBufferManager`.
- Shaped for `ESPJsonDB` snapshot backup/restore flows without adding a direct dependency.

## Quick Start
```cpp
#include <ESPCompressor.h>

ESPCompressor compressor;

void setup() {
    compressor.init();

    std::vector<uint8_t> input = {'h', 'e', 'l', 'l', 'o'};
    BufferSource source(input.data(), input.size());

    std::vector<uint8_t> compressed;
    DynamicBufferSink sink(compressed);

    CompressionResult result = compressor.compress(source, sink);
    if (!result.ok()) {
        return;
    }
}

void loop() {
}
```

## Async Example
```cpp
ESPCompressor compressor;
compressor.init();

auto source = std::make_shared<FileSource>(LittleFS, "/db/snapshot.json");
auto sink = std::make_shared<FileSink>(LittleFS, "/db/snapshot.esc");

compressor.compressAsync(
    source,
    sink,
    CompressionCallbacks{
        [](const CompressionProgress &progress) {
            if (progress.hasKnownTotalInput) {
                Serial.printf("progress: %.2f%%\n", progress.percent);
            }
        },
        [](const CompressionResult &result) {
            Serial.printf("done: %s\n", compressionErrorToString(result.error));
        },
    }
);
```

## Core API
- `CompressionError init(const ESPCompressorConfig& config = {})`
- `void deinit()`
- `CompressionResult compress(CompressionSource&, CompressionSink&, ProgressCallback = nullptr, const CompressionJobOptions& = {})`
- `CompressionResult decompress(CompressionSource&, CompressionSink&, ProgressCallback = nullptr, const CompressionJobOptions& = {})`
- `CompressionJobHandle compressAsync(std::shared_ptr<CompressionSource>, std::shared_ptr<CompressionSink>, const CompressionCallbacks& = {}, const CompressionJobOptions& = {})`
- `CompressionJobHandle decompressAsync(...)`
- `bool cancel(const CompressionJobHandle&)`
- `CompressionResult lastResult() const`

## Container Format
- Magic: `ESC1`
- Version: `1`
- Algorithm id: `1` (`LZ-lite`)
- Header includes optional original size, block size, and header CRC32.
- Each block stores flags, original size, stored size, original-data CRC32, and payload.
- If compression is not smaller than the input block, ESPCompressor stores the block raw.

## ESPJsonDB Backup Flow
ESPCompressor intentionally stays independent from `ESPJsonDB`, but it fits snapshot export/import cleanly:

```cpp
JsonDocument snapshot = db.getSnapshot(SnapshotMode::InMemoryConsistent);
std::string json;
serializeJson(snapshot, json);

BufferSource source(reinterpret_cast<const uint8_t *>(json.data()), json.size());
FileSink sink(LittleFS, "/backups/snapshot.esc");
CompressionResult result = compressor.compress(source, sink);
```

To restore, decompress the `.esc` file, deserialize the JSON payload, then call `restoreFromSnapshot()`.

## Examples
- `examples/basic_roundtrip` – buffer-to-buffer roundtrip with status output.
- `examples/file_roundtrip` – LittleFS file compression and decompression.
- `examples/stream_roundtrip` – serialize JSON into a stream source and emit compressed bytes to a `Print` sink.

## Notes
- Callbacks execute on the async worker context and should return quickly.
- Non-transactional sinks such as `PrintSink` are rejected by default unless `CompressionJobOptions::allowPartialOutput` is enabled.
- `FileSink` writes to `path.tmp` first and renames only after a successful commit.
- Unknown-size sources are supported; in that case `CompressionProgress::hasKnownTotalInput` is `false`.

## Tests
The host CMake test suite under `test/` covers block roundtrips, corruption handling, raw fallback, transactional sink behavior, and async lifecycle checks. The Arduino examples are built in CI through PlatformIO and Arduino CLI.

## License
ESPCompressor is released under the [MIT License](LICENSE.md).

## ESPToolKit
- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
- Visit the website: <https://www.esptoolkit.hu/>
