#include <ESPCompressor.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string &message) {
	throw std::runtime_error(message);
}

void expectTrue(bool condition, const std::string &message) {
	if (!condition) {
		fail(message);
	}
}

template <typename T>
void expectEqual(const T &actual, const T &expected, const std::string &message) {
	if (!(actual == expected)) {
		fail(message);
	}
}

struct VectorPrint : public Print {
	std::vector<uint8_t> data;

	size_t write(const uint8_t *buffer, size_t size) override {
		data.insert(data.end(), buffer, buffer + size);
		return size;
	}
};

struct VectorStream : public Stream {
	explicit VectorStream(std::vector<uint8_t> bytes) : payload(std::move(bytes)) {
	}

	int available() override {
		return static_cast<int>(payload.size() - offset);
	}

	int read() override {
		if (offset >= payload.size()) {
			return -1;
		}
		return payload[offset++];
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		payload.insert(payload.end(), buffer, buffer + size);
		return size;
	}

	std::vector<uint8_t> payload;
	size_t offset = 0;
};

struct SlowBufferSource : public CompressionSource {
	explicit SlowBufferSource(std::vector<uint8_t> bytes, size_t chunkSize, int delayMs)
	    : _bytes(std::move(bytes)), _chunkSize(chunkSize), _delayMs(delayMs) {
	}

	CompressionError open() noexcept override {
		_offset = 0;
		_open = true;
		return CompressionError::Ok;
	}

	void close() noexcept override {
		_open = false;
	}

	size_t read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept override {
		err = CompressionError::Ok;
		if (!_open) {
			err = CompressionError::ReadFailed;
			return 0;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(_delayMs));
		const size_t remaining = _bytes.size() - _offset;
		const size_t amount = std::min(remaining, std::min(capacity, _chunkSize));
		if (amount != 0) {
			std::copy_n(_bytes.data() + _offset, amount, buffer);
			_offset += amount;
		}
		return amount;
	}

	bool eof() const noexcept override {
		return _offset >= _bytes.size();
	}

	bool hasKnownSize() const noexcept override {
		return true;
	}

	uint64_t totalSize() const noexcept override {
		return _bytes.size();
	}

	std::vector<uint8_t> _bytes;
	size_t _chunkSize = 0;
	int _delayMs = 0;
	size_t _offset = 0;
	bool _open = false;
};

std::vector<uint8_t> makeCompressiblePayload() {
	const std::string text =
	    "{\"collections\":{\"users\":[{\"_id\":\"abc\",\"role\":\"admin\",\"role2\":\"admin\","
	    "\"role3\":\"admin\",\"name\":\"ESPToolKit\"}]}}";
	std::vector<uint8_t> out;
	for (int i = 0; i < 64; ++i) {
		out.insert(out.end(), text.begin(), text.end());
	}
	return out;
}

std::vector<uint8_t> roundTripCompressDecompress(
    ESPCompressor &compressor,
    CompressionSource &source,
    const CompressionJobOptions &options = {}
) {
	std::vector<uint8_t> compressed;
	DynamicBufferSink compressedSink(compressed);
	CompressionResult compressResult = compressor.compress(source, compressedSink, nullptr, options);
	expectTrue(compressResult.ok(), "compression should succeed");

	BufferSource compressedSource(compressed.data(), compressed.size());
	std::vector<uint8_t> restored;
	DynamicBufferSink restoredSink(restored);
	CompressionResult decompressResult =
	    compressor.decompress(compressedSource, restoredSink, nullptr, options);
	expectTrue(decompressResult.ok(), "decompression should succeed");
	return restored;
}

void waitForJob(const CompressionJobHandle &handle, int timeoutMs = 4000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (!handle.done() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!handle.done()) {
		fail("async job timed out");
	}
}

void testBufferRoundTrip() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::vector<uint8_t> input = makeCompressiblePayload();
	BufferSource source(input.data(), input.size());
	const std::vector<uint8_t> restored = roundTripCompressDecompress(compressor, source);
	expectEqual(restored, input, "restored buffer should equal original input");
}

void testUnknownSizeStreamRoundTrip() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::vector<uint8_t> input = makeCompressiblePayload();
	VectorStream stream(input);
	StreamSource source(stream);
	const std::vector<uint8_t> restored = roundTripCompressDecompress(compressor, source);
	expectEqual(restored, input, "restored stream payload should equal original input");
}

void testRawFallbackFlag() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	std::vector<uint8_t> input(2048);
	uint32_t state = 0x12345678u;
	for (size_t i = 0; i < input.size(); ++i) {
		state = state * 1664525u + 1013904223u;
		input[i] = static_cast<uint8_t>((state >> 24) & 0xFFu);
	}

	BufferSource source(input.data(), input.size());
	std::vector<uint8_t> compressed;
	DynamicBufferSink sink(compressed);
	CompressionResult result = compressor.compress(source, sink);
	expectTrue(result.ok(), "compression should succeed");
	expectTrue(compressed.size() > (kEscEncodedHeaderSize + kEscEncodedBlockHeaderSize), "compressed output should contain block");
	expectTrue((compressed[kEscEncodedHeaderSize] & kEscBlockFlagRaw) != 0, "first block should use raw fallback");
}

void testCorruptionDetection() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::vector<uint8_t> input = makeCompressiblePayload();
	BufferSource source(input.data(), input.size());
	std::vector<uint8_t> compressed;
	DynamicBufferSink sink(compressed);
	expectTrue(compressor.compress(source, sink).ok(), "compression should succeed");
	compressed.back() ^= 0xFFu;

	BufferSource corruptedSource(compressed.data(), compressed.size());
	std::vector<uint8_t> output;
	DynamicBufferSink outputSink(output);
	const CompressionResult result = compressor.decompress(corruptedSource, outputSink);
	expectEqual(result.error, CompressionError::CorruptData, "corrupted payload should fail");
}

void testFixedBufferOverflow() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::vector<uint8_t> input = makeCompressiblePayload();
	BufferSource source(input.data(), input.size());
	std::vector<uint8_t> compressed;
	DynamicBufferSink compressedSink(compressed);
	expectTrue(compressor.compress(source, compressedSink).ok(), "compression should succeed");

	BufferSource compressedSource(compressed.data(), compressed.size());
	std::vector<uint8_t> tiny(16, 0);
	FixedBufferSink tinySink(tiny.data(), tiny.size());
	const CompressionResult result = compressor.decompress(compressedSource, tinySink);
	expectEqual(result.error, CompressionError::OutputOverflow, "tiny fixed sink should overflow");
}

void testFileSinkTransactionalCleanup() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::filesystem::path root =
	    std::filesystem::temp_directory_path() / "esp_compressor_file_sink_cleanup";
	std::filesystem::remove_all(root);
	fs::FS fs(root);

	const std::vector<uint8_t> input = makeCompressiblePayload();
	BufferSource source(input.data(), input.size());
	std::vector<uint8_t> compressed;
	DynamicBufferSink compressedSink(compressed);
	expectTrue(compressor.compress(source, compressedSink).ok(), "compression should succeed");
	compressed.back() ^= 0xFFu;

	BufferSource corruptedSource(compressed.data(), compressed.size());
	FileSink fileSink(fs, "/backups/corrupt.esc");
	const CompressionResult result = compressor.decompress(corruptedSource, fileSink);
	expectEqual(result.error, CompressionError::CorruptData, "decompression should detect corruption");
	expectTrue(!fs.exists("/backups/corrupt.esc"), "final file should not exist after failure");
	expectTrue(!fs.exists("/backups/corrupt.esc.tmp"), "temp file should be removed after failure");
}

void testAsyncProgressBusyAndCompletion() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	std::vector<uint8_t> input = makeCompressiblePayload();
	auto source = std::make_shared<SlowBufferSource>(input, 128, 5);
	auto sinkBuffer = std::make_shared<std::vector<uint8_t>>();
	auto sink = std::make_shared<DynamicBufferSink>(*sinkBuffer);
	int progressCount = 0;
	int completionCount = 0;

	CompressionCallbacks callbacks{};
	callbacks.onProgress = [&](const CompressionProgress &progress) {
		if (!progress.done) {
			++progressCount;
		}
	};
	callbacks.onComplete = [&](const CompressionResult &) {
		++completionCount;
	};

	const CompressionJobHandle handle = compressor.compressAsync(source, sink, callbacks);
	const CompressionJobHandle busyHandle =
	    compressor.compressAsync(source, sink, CompressionCallbacks{});
	expectEqual(busyHandle.state(), CompressionJobState::Rejected, "second async job should be rejected");
	expectEqual(busyHandle.result().error, CompressionError::Busy, "second async job should report busy");

	waitForJob(handle);
	expectEqual(handle.state(), CompressionJobState::Completed, "primary async job should complete");
	expectTrue(handle.result().ok(), "primary async job should succeed");
	expectTrue(progressCount > 1, "progress callback should run multiple times");
	expectEqual(completionCount, 1, "completion callback should run exactly once");
}

void testAsyncCancellationAndDeinit() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	std::vector<uint8_t> input = makeCompressiblePayload();
	input.insert(input.end(), input.begin(), input.end());
	auto source = std::make_shared<SlowBufferSource>(input, 128, 10);
	auto sinkBuffer = std::make_shared<std::vector<uint8_t>>();
	auto sink = std::make_shared<DynamicBufferSink>(*sinkBuffer);
	const CompressionJobHandle handle = compressor.compressAsync(source, sink);
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	expectTrue(handle.cancel(), "cancel should succeed");
	waitForJob(handle);
	expectEqual(handle.state(), CompressionJobState::Cancelled, "cancelled async job should report cancelled");

	auto secondSource = std::make_shared<SlowBufferSource>(input, 128, 10);
	auto secondSinkBuffer = std::make_shared<std::vector<uint8_t>>();
	auto secondSink = std::make_shared<DynamicBufferSink>(*secondSinkBuffer);
	const CompressionJobHandle secondHandle = compressor.compressAsync(secondSource, secondSink);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	compressor.deinit();
	waitForJob(secondHandle);
	expectEqual(secondHandle.state(), CompressionJobState::Cancelled, "deinit should cancel active job");
	expectTrue(!compressor.isBusy(), "compressor should not stay busy after deinit");
}

} // namespace

int main() {
	try {
		testBufferRoundTrip();
		testUnknownSizeStreamRoundTrip();
		testRawFallbackFlag();
		testCorruptionDetection();
		testFixedBufferOverflow();
		testFileSinkTransactionalCleanup();
		testAsyncProgressBusyAndCompletion();
		testAsyncCancellationAndDeinit();
	} catch (const std::exception &ex) {
		std::cerr << "FAIL: " << ex.what() << '\n';
		return 1;
	}

	std::cout << "All esp-compressor tests passed\n";
	return 0;
}
