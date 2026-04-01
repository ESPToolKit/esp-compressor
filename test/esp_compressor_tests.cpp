#include <ESPCompressor.h>
#include <ESPCompressorFormat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
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

std::vector<uint8_t> compressPayload(ESPCompressor &compressor, const std::vector<uint8_t> &input) {
	BufferSource source(input.data(), input.size());
	std::vector<uint8_t> compressed;
	DynamicBufferSink sink(compressed);
	const CompressionResult result = compressor.compress(source, sink);
	expectTrue(result.ok(), "compression should succeed");
	return compressed;
}

EscBlockHeader readFirstBlockHeader(const std::vector<uint8_t> &compressed) {
	EscBlockHeader blockHeader{};
	const CompressionError error = decodeEscBlockHeader(
	    compressed.data() + kEscEncodedHeaderSize,
	    kEscEncodedBlockHeaderSize,
	    blockHeader
	);
	expectEqual(error, CompressionError::Ok, "block header should decode");
	return blockHeader;
}

void writeFirstBlockHeader(std::vector<uint8_t> &compressed, const EscBlockHeader &blockHeader) {
	const CompressionError error = encodeEscBlockHeader(
	    blockHeader,
	    compressed.data() + kEscEncodedHeaderSize,
	    kEscEncodedBlockHeaderSize
	);
	expectEqual(error, CompressionError::Ok, "block header should encode");
}

std::string readFileString(const std::filesystem::path &path) {
	std::ifstream file(path, std::ios::binary);
	return std::string(
	    std::istreambuf_iterator<char>(file),
	    std::istreambuf_iterator<char>()
	);
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

void testDecompressionBlockBoundsValidation() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::vector<uint8_t> input = makeCompressiblePayload();
	const std::vector<uint8_t> baseline = compressPayload(compressor, input);

	{
		std::vector<uint8_t> corrupted = baseline;
		EscBlockHeader blockHeader = readFirstBlockHeader(corrupted);
		blockHeader.originalSize = kESPCompressorDefaultBlockSize + 1;
		writeFirstBlockHeader(corrupted, blockHeader);

		BufferSource source(corrupted.data(), corrupted.size());
		std::vector<uint8_t> restored;
		DynamicBufferSink sink(restored);
		const CompressionResult result = compressor.decompress(source, sink);
		expectEqual(result.error, CompressionError::CorruptData, "oversized original block should fail");
	}

	{
		std::vector<uint8_t> corrupted = baseline;
		EscBlockHeader blockHeader = readFirstBlockHeader(corrupted);
		blockHeader.storedSize = blockHeader.originalSize + 1;
		writeFirstBlockHeader(corrupted, blockHeader);

		BufferSource source(corrupted.data(), corrupted.size());
		std::vector<uint8_t> restored;
		DynamicBufferSink sink(restored);
		const CompressionResult result = compressor.decompress(source, sink);
		expectEqual(result.error, CompressionError::CorruptData, "stored block larger than original should fail");
	}

	{
		std::vector<uint8_t> corrupted = baseline;
		EscBlockHeader blockHeader = readFirstBlockHeader(corrupted);
		blockHeader.flags |= kEscBlockFlagRaw;
		blockHeader.storedSize = blockHeader.originalSize > 0 ? (blockHeader.originalSize - 1) : 0;
		writeFirstBlockHeader(corrupted, blockHeader);

		BufferSource source(corrupted.data(), corrupted.size());
		std::vector<uint8_t> restored;
		DynamicBufferSink sink(restored);
		const CompressionResult result = compressor.decompress(source, sink);
		expectEqual(result.error, CompressionError::CorruptData, "raw block size mismatch should fail");
	}

	{
		std::vector<uint8_t> corrupted = baseline;
		EscBlockHeader blockHeader = readFirstBlockHeader(corrupted);
		blockHeader.flags &= static_cast<uint8_t>(~kEscBlockFlagFinal);
		blockHeader.originalSize = 0;
		blockHeader.storedSize = 0;
		blockHeader.crc32 = 0;
		writeFirstBlockHeader(corrupted, blockHeader);

		BufferSource source(corrupted.data(), corrupted.size());
		std::vector<uint8_t> restored;
		DynamicBufferSink sink(restored);
		const CompressionResult result = compressor.decompress(source, sink);
		expectEqual(result.error, CompressionError::CorruptData, "zero-sized non-final block should fail");
	}
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

void testBufferSinkCommitReleasesStaging() {
	std::vector<uint8_t> output;
	DynamicBufferSink dynamicSink(output, false);
	expectEqual(dynamicSink.open(), CompressionError::Ok, "dynamic sink should open");
	const uint8_t dynamicPayload[] = {1, 2, 3, 4};
	expectEqual(dynamicSink.write(dynamicPayload, sizeof(dynamicPayload)), CompressionError::Ok, "dynamic sink write should succeed");
	expectEqual(dynamicSink.bytesWritten(), static_cast<uint64_t>(sizeof(dynamicPayload)), "dynamic sink should report staged bytes");
	expectEqual(dynamicSink.commit(), CompressionError::Ok, "dynamic sink commit should succeed");
	expectEqual(dynamicSink.bytesWritten(), static_cast<uint64_t>(0), "dynamic sink should release staged bytes after commit");
	expectEqual(output, std::vector<uint8_t>(dynamicPayload, dynamicPayload + sizeof(dynamicPayload)), "dynamic sink output should match payload");
	dynamicSink.close();

	expectEqual(dynamicSink.open(), CompressionError::Ok, "dynamic sink should reopen");
	const uint8_t nextDynamicPayload[] = {9};
	expectEqual(dynamicSink.write(nextDynamicPayload, sizeof(nextDynamicPayload)), CompressionError::Ok, "dynamic sink second write should succeed");
	expectEqual(dynamicSink.commit(), CompressionError::Ok, "dynamic sink second commit should succeed");
	expectEqual(dynamicSink.bytesWritten(), static_cast<uint64_t>(0), "dynamic sink should release staging on reuse");
	expectEqual(output, std::vector<uint8_t>(nextDynamicPayload, nextDynamicPayload + sizeof(nextDynamicPayload)), "dynamic sink reuse should publish new payload");
	dynamicSink.close();

	uint8_t fixedStorage[8] = {};
	FixedBufferSink fixedSink(fixedStorage, sizeof(fixedStorage), false);
	expectEqual(fixedSink.open(), CompressionError::Ok, "fixed sink should open");
	const uint8_t fixedPayload[] = {5, 6, 7};
	expectEqual(fixedSink.write(fixedPayload, sizeof(fixedPayload)), CompressionError::Ok, "fixed sink write should succeed");
	expectEqual(fixedSink.bytesWritten(), static_cast<uint64_t>(sizeof(fixedPayload)), "fixed sink should report staged bytes");
	expectEqual(fixedSink.commit(), CompressionError::Ok, "fixed sink commit should succeed");
	expectEqual(fixedSink.bytesWritten(), static_cast<uint64_t>(0), "fixed sink should release staged bytes after commit");
	expectTrue(std::equal(std::begin(fixedPayload), std::end(fixedPayload), fixedStorage), "fixed sink output should match payload");
	fixedSink.close();
}

void testFileSinkPreservesDestinationOnRenameFailure() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	const std::filesystem::path root =
	    std::filesystem::temp_directory_path() / "esp_compressor_file_sink_replace";
	std::filesystem::remove_all(root);
	fs::FS fs(root);
	std::filesystem::create_directories(root / "backups");
	{
		std::ofstream existing(root / "backups" / "existing.esc", std::ios::binary);
		existing << "old-backup";
	}

	const std::vector<uint8_t> input = makeCompressiblePayload();
	BufferSource source(input.data(), input.size());
	FileSink sink(fs, "/backups/existing.esc");
	fs.failRenameOnCall(2);
	const CompressionResult result = compressor.compress(source, sink);
	expectEqual(result.error, CompressionError::WriteFailed, "rename failure should surface as write failure");
	expectEqual(readFileString(root / "backups" / "existing.esc"), std::string("old-backup"), "existing destination should be preserved");
	expectTrue(!fs.exists("/backups/existing.esc.tmp"), "temp file should be cleaned up after rename failure");
	expectTrue(!fs.exists("/backups/existing.esc.bak"), "backup file should not remain after rollback");
}

void testSyncBusyStateAcrossSyncAndAsyncCalls() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	std::vector<uint8_t> input = makeCompressiblePayload();
	input.insert(input.end(), input.begin(), input.end());
	std::atomic<bool> syncDone{false};
	std::atomic<CompressionError> syncError{CompressionError::InternalError};

	std::thread worker([&]() {
		SlowBufferSource slowSource(input, 128, 5);
		std::vector<uint8_t> compressed;
		DynamicBufferSink slowSink(compressed);
		const CompressionResult result = compressor.compress(slowSource, slowSink);
		syncError.store(result.error, std::memory_order_release);
		syncDone.store(true, std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	while (!compressor.isBusy() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	expectTrue(compressor.isBusy(), "compressor should report busy during sync work");

	BufferSource secondSource(input.data(), input.size());
	std::vector<uint8_t> secondCompressed;
	DynamicBufferSink secondSink(secondCompressed);
	const CompressionResult syncBusyResult = compressor.compress(secondSource, secondSink);
	expectEqual(syncBusyResult.error, CompressionError::Busy, "concurrent sync compression should be rejected");

	auto asyncSource = std::make_shared<BufferSource>(input.data(), input.size());
	auto asyncBuffer = std::make_shared<std::vector<uint8_t>>();
	auto asyncSink = std::make_shared<DynamicBufferSink>(*asyncBuffer);
	const CompressionJobHandle asyncBusyHandle = compressor.compressAsync(asyncSource, asyncSink);
	expectEqual(asyncBusyHandle.state(), CompressionJobState::Rejected, "async job should be rejected while sync work is active");
	expectEqual(asyncBusyHandle.result().error, CompressionError::Busy, "async busy rejection should report busy");

	worker.join();
	expectTrue(syncDone.load(std::memory_order_acquire), "sync worker should finish");
	expectEqual(syncError.load(std::memory_order_acquire), CompressionError::Ok, "sync worker should succeed");
	expectTrue(!compressor.isBusy(), "compressor should clear busy state after sync work");
}

void testAsyncSetupFailureCompletionDispatchesOnce() {
	ESPCompressor compressor;
	expectEqual(compressor.init(), CompressionError::Ok, "init should succeed");

	for (CompressionError error : {CompressionError::NoMemory, CompressionError::InternalError}) {
		int completionCount = 0;
		CompressionResult completionResult{};

		CompressionCallbacks callbacks{};
		callbacks.onComplete = [&](const CompressionResult &result) {
			++completionCount;
			completionResult = result;
		};

		const CompressionJobHandle handle = espCompressorTestSimulateAsyncSetupFailure(
		    compressor,
		    CompressionOperation::Compress,
		    error,
		    callbacks
		);

		expectEqual(handle.state(), CompressionJobState::Rejected, "setup failure should reject the async job");
		expectEqual(handle.result().error, error, "rejected async job should report the setup failure");
		expectEqual(completionCount, 1, "completion callback should run exactly once for setup failure");
		expectEqual(completionResult.error, error, "completion callback should receive the setup failure");
		expectEqual(completionResult.operation, CompressionOperation::Compress, "completion callback should preserve operation");
		expectEqual(compressor.lastResult().error, error, "last result should match the setup failure");
		expectTrue(!compressor.isBusy(), "compressor should not stay busy after setup failure");
	}
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
		testDecompressionBlockBoundsValidation();
		testFixedBufferOverflow();
		testFileSinkTransactionalCleanup();
		testBufferSinkCommitReleasesStaging();
		testFileSinkPreservesDestinationOnRenameFailure();
		testSyncBusyStateAcrossSyncAndAsyncCalls();
		testAsyncSetupFailureCompletionDispatchesOnce();
		testAsyncProgressBusyAndCompletion();
		testAsyncCancellationAndDeinit();
	} catch (const std::exception &ex) {
		std::cerr << "FAIL: " << ex.what() << '\n';
		return 1;
	}

	std::cout << "All esp-compressor tests passed\n";
	return 0;
}
