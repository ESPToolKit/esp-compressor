#include "compressor.h"

#include "../codec/codec.h"
#include "../format/esc_format.h"
#include "../util/memory_buffer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#if ESPCOMPRESSOR_HAS_FREERTOS
extern "C" {
#include <freertos/task.h>
}
#endif

namespace {

struct ScopedIoSession {
	CompressionSource *source = nullptr;
	CompressionSink *sink = nullptr;
	bool committed = false;
	bool opened = false;

	~ScopedIoSession() {
		if (sink && opened && !committed) {
			sink->abort();
		}
		if (sink) {
			sink->close();
		}
		if (source) {
			source->close();
		}
	}
};

struct RunContext {
	const ESPCompressorConfig *config = nullptr;
	CompressionOperation operation = CompressionOperation::Compress;
	CompressionSource *source = nullptr;
	CompressionSink *sink = nullptr;
	ProgressCallback onProgress{};
	CompressionJobOptions options{};
	std::atomic<bool> *cancelRequested = nullptr;
};

CompressionResult makeResult(CompressionOperation operation, CompressionError error) {
	CompressionResult result{};
	result.operation = operation;
	result.error = error;
	return result;
}

class ScopedSyncRunSlot {
  public:
	ScopedSyncRunSlot(bool &busy, std::mutex &mutex) : _busy(&busy), _mutex(&mutex) {
	}

	ScopedSyncRunSlot(const ScopedSyncRunSlot &) = delete;
	ScopedSyncRunSlot &operator=(const ScopedSyncRunSlot &) = delete;

	~ScopedSyncRunSlot() {
		release();
	}

	void release() noexcept {
		if (!_busy || !_mutex) {
			return;
		}

		std::lock_guard<std::mutex> guard(*_mutex);
		*_busy = false;
		_busy = nullptr;
		_mutex = nullptr;
	}

  private:
	bool *_busy = nullptr;
	std::mutex *_mutex = nullptr;
};

bool isCancelled(const std::atomic<bool> *flag) {
	return flag && flag->load(std::memory_order_acquire);
}

void sleepBriefly() {
#if ESPCOMPRESSOR_HAS_FREERTOS
	vTaskDelay(1);
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

bool shouldAbortForCancel(const RunContext &ctx, CompressionResult &result) {
	if (!isCancelled(ctx.cancelRequested)) {
		return false;
	}
	result.error = CompressionError::Cancelled;
	return true;
}

bool validateEscBlockBounds(const EscHeader &header, const EscBlockHeader &blockHeader) {
	if (blockHeader.originalSize == 0 &&
	    (blockHeader.flags & kEscBlockFlagFinal) == 0) {
		return false;
	}
	if (blockHeader.originalSize > header.blockSize) {
		return false;
	}
	if (blockHeader.storedSize > blockHeader.originalSize) {
		return false;
	}
	if ((blockHeader.flags & kEscBlockFlagRaw) != 0 &&
	    blockHeader.storedSize != blockHeader.originalSize) {
		return false;
	}
	return true;
}

void emitProgress(
    const RunContext &ctx,
    CompressionProgress &progress,
    const CompressionResult &result,
    bool done
) {
	progress.done = done;
	progress.cancelled = result.error == CompressionError::Cancelled;
	if (progress.hasKnownTotalInput && progress.totalInputBytes != 0) {
		progress.percent = static_cast<float>(
		    (100.0 * static_cast<double>(progress.processedInputBytes)) /
		    static_cast<double>(progress.totalInputBytes)
		);
		if (progress.percent > 100.0f) {
			progress.percent = 100.0f;
		}
	} else {
		progress.percent = 0.0f;
	}
	if (ctx.onProgress) {
		ctx.onProgress(progress);
	}
}

CompressionError readExact(
    CompressionSource &source,
    uint8_t *buffer,
    size_t size,
    uint64_t &processedBytes
) {
	size_t offset = 0;
	while (offset < size) {
		CompressionError err = CompressionError::Ok;
		const size_t chunk = source.read(buffer + offset, size - offset, err);
		if (err != CompressionError::Ok) {
			return err;
		}
		if (chunk == 0) {
			return CompressionError::CorruptData;
		}
		offset += chunk;
		processedBytes += chunk;
	}
	return CompressionError::Ok;
}

CompressionResult runCompress(const RunContext &ctx) {
	CompressionResult result = makeResult(ctx.operation, CompressionError::Ok);
	if (!ctx.config || !ctx.source || !ctx.sink || ctx.config->blockSize == 0 || ctx.config->windowSize == 0 ||
	    ctx.config->blockSize > UINT32_MAX) {
		result.error = CompressionError::InvalidArgument;
		return result;
	}
	if (!ctx.sink->isTransactional() && !ctx.options.allowPartialOutput) {
		result.error = CompressionError::InvalidArgument;
		return result;
	}

	ScopedIoSession session;
	session.source = ctx.source;
	session.sink = ctx.sink;

	result.error = ctx.source->open();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	result.error = ctx.sink->open();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	session.opened = true;

	MemoryBuffer blockBuffer(ctx.config->usePSRAMBuffers);
	MemoryBuffer encodedBlock(ctx.config->usePSRAMBuffers);
	MemoryBuffer headerBuffer(ctx.config->usePSRAMBuffers);
	if (!blockBuffer.resize(ctx.config->blockSize)) {
		result.error = CompressionError::NoMemory;
		return result;
	}

	EscHeader header{};
	header.blockSize = static_cast<uint32_t>(ctx.config->blockSize);
	header.originalSize = ctx.source->hasKnownSize() ? ctx.source->totalSize() : kEscUnknownOriginalSize;
	if (!ctx.source->hasKnownSize()) {
		header.flags |= kEscHeaderFlagUnknownOriginalSize;
	}
	result.error = encodeEscHeader(header, headerBuffer);
	if (result.error != CompressionError::Ok) {
		return result;
	}
	result.error = ctx.sink->write(headerBuffer.data(), headerBuffer.size());
	if (result.error != CompressionError::Ok) {
		return result;
	}

	CompressionProgress progress{};
	progress.operation = ctx.operation;
	progress.hasKnownTotalInput = ctx.source->hasKnownSize();
	progress.totalInputBytes = ctx.source->totalSize();
	progress.producedOutputBytes = ctx.sink->bytesWritten();

	bool wroteAnyBlock = false;
	bool lastBlockWasFinal = false;
	while (true) {
		if (shouldAbortForCancel(ctx, result)) {
			return result;
		}

		CompressionError readErr = CompressionError::Ok;
		const size_t readBytes = ctx.source->read(blockBuffer.data(), ctx.config->blockSize, readErr);
		if (readErr != CompressionError::Ok) {
			result.error = readErr;
			return result;
		}
		result.inputBytes += readBytes;
		progress.processedInputBytes = result.inputBytes;

		if (readBytes == 0) {
			if (!ctx.source->eof()) {
				result.error = CompressionError::ReadFailed;
				return result;
			}
			break;
		}

		result.error = compressLzLiteBlock(
		    blockBuffer.data(),
		    readBytes,
		    ctx.config->windowSize,
		    encodedBlock
		);
		if (result.error != CompressionError::Ok) {
			return result;
		}

		const bool useRaw = encodedBlock.size() >= readBytes;
		const uint8_t *payload = useRaw ? blockBuffer.data() : encodedBlock.data();
		const size_t payloadSize = useRaw ? readBytes : encodedBlock.size();

		EscBlockHeader blockHeader{};
		blockHeader.flags = useRaw ? kEscBlockFlagRaw : 0;
		if (ctx.source->eof()) {
			blockHeader.flags |= kEscBlockFlagFinal;
		}
		lastBlockWasFinal = (blockHeader.flags & kEscBlockFlagFinal) != 0;
		blockHeader.originalSize = static_cast<uint32_t>(readBytes);
		blockHeader.storedSize = static_cast<uint32_t>(payloadSize);
		blockHeader.crc32 = espCompressorCrc32(blockBuffer.data(), readBytes);

		uint8_t blockHeaderBytes[kEscEncodedBlockHeaderSize];
		result.error = encodeEscBlockHeader(blockHeader, blockHeaderBytes, sizeof(blockHeaderBytes));
		if (result.error != CompressionError::Ok) {
			return result;
		}
		result.error = ctx.sink->write(blockHeaderBytes, sizeof(blockHeaderBytes));
		if (result.error != CompressionError::Ok) {
			return result;
		}
		result.error = ctx.sink->write(payload, payloadSize);
		if (result.error != CompressionError::Ok) {
			return result;
		}

		result.blocksProcessed++;
		wroteAnyBlock = true;
		result.outputBytes = ctx.sink->bytesWritten();
		progress.producedOutputBytes = result.outputBytes;
		emitProgress(ctx, progress, result, false);
	}

	if (!wroteAnyBlock || !lastBlockWasFinal) {
		EscBlockHeader blockHeader{};
		blockHeader.flags = kEscBlockFlagRaw | kEscBlockFlagFinal;
		uint8_t blockHeaderBytes[kEscEncodedBlockHeaderSize];
		result.error = encodeEscBlockHeader(blockHeader, blockHeaderBytes, sizeof(blockHeaderBytes));
		if (result.error != CompressionError::Ok) {
			return result;
		}
		result.error = ctx.sink->write(blockHeaderBytes, sizeof(blockHeaderBytes));
		if (result.error != CompressionError::Ok) {
			return result;
		}
		result.outputBytes = ctx.sink->bytesWritten();
	}

	if (shouldAbortForCancel(ctx, result)) {
		return result;
	}

	result.error = ctx.sink->commit();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	session.committed = true;
	result.outputBytes = ctx.sink->bytesWritten();
	progress.processedInputBytes = result.inputBytes;
	progress.producedOutputBytes = result.outputBytes;
	emitProgress(ctx, progress, result, true);
	return result;
}

CompressionResult runDecompress(const RunContext &ctx) {
	CompressionResult result = makeResult(ctx.operation, CompressionError::Ok);
	if (!ctx.config || !ctx.source || !ctx.sink) {
		result.error = CompressionError::InvalidArgument;
		return result;
	}
	if (!ctx.sink->isTransactional() && !ctx.options.allowPartialOutput) {
		result.error = CompressionError::InvalidArgument;
		return result;
	}

	ScopedIoSession session;
	session.source = ctx.source;
	session.sink = ctx.sink;

	result.error = ctx.source->open();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	result.error = ctx.sink->open();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	session.opened = true;

	uint64_t processedCompressedInput = 0;
	uint8_t encodedHeader[kEscEncodedHeaderSize];
	result.error = readExact(*ctx.source, encodedHeader, sizeof(encodedHeader), processedCompressedInput);
	if (result.error != CompressionError::Ok) {
		return result;
	}

	EscHeader header{};
	result.error = decodeEscHeader(encodedHeader, sizeof(encodedHeader), header);
	if (result.error != CompressionError::Ok) {
		return result;
	}
	if (header.version != kEscFormatVersion) {
		result.error = CompressionError::UnsupportedVersion;
		return result;
	}
	if (header.algorithm != kEscAlgorithmLzLite) {
		result.error = CompressionError::UnsupportedAlgorithm;
		return result;
	}
	if (header.blockSize == 0) {
		result.error = CompressionError::CorruptData;
		return result;
	}

	CompressionProgress progress{};
	progress.operation = ctx.operation;
	progress.hasKnownTotalInput = ctx.source->hasKnownSize();
	progress.totalInputBytes = ctx.source->totalSize();
	progress.totalOutputBytes =
	    (header.flags & kEscHeaderFlagUnknownOriginalSize) ? 0 : header.originalSize;
	progress.producedOutputBytes = 0;

	MemoryBuffer storedBlock(ctx.config->usePSRAMBuffers);
	MemoryBuffer outputBlock(ctx.config->usePSRAMBuffers);

	bool sawFinalBlock = false;
	while (!sawFinalBlock) {
		if (shouldAbortForCancel(ctx, result)) {
			return result;
		}

		uint8_t blockHeaderBytes[kEscEncodedBlockHeaderSize];
		result.error =
		    readExact(*ctx.source, blockHeaderBytes, sizeof(blockHeaderBytes), processedCompressedInput);
		if (result.error != CompressionError::Ok) {
			return result;
		}

		EscBlockHeader blockHeader{};
		result.error = decodeEscBlockHeader(blockHeaderBytes, sizeof(blockHeaderBytes), blockHeader);
		if (result.error != CompressionError::Ok) {
			return result;
		}
		sawFinalBlock = (blockHeader.flags & kEscBlockFlagFinal) != 0;
		if (!validateEscBlockBounds(header, blockHeader)) {
			result.error = CompressionError::CorruptData;
			return result;
		}

		if (!storedBlock.resize(blockHeader.storedSize)) {
			result.error = CompressionError::NoMemory;
			return result;
		}
		if (blockHeader.storedSize != 0) {
			result.error = readExact(
			    *ctx.source,
			    storedBlock.data(),
			    blockHeader.storedSize,
			    processedCompressedInput
			);
			if (result.error != CompressionError::Ok) {
				return result;
			}
		}

		if (blockHeader.flags & kEscBlockFlagRaw) {
			if (blockHeader.originalSize != blockHeader.storedSize) {
				result.error = CompressionError::CorruptData;
				return result;
			}
			if (espCompressorCrc32(storedBlock.data(), storedBlock.size()) != blockHeader.crc32) {
				result.error = CompressionError::CorruptData;
				return result;
			}
			result.error = ctx.sink->write(storedBlock.data(), storedBlock.size());
			if (result.error != CompressionError::Ok) {
				return result;
			}
			result.outputBytes += storedBlock.size();
		} else {
			result.error = decompressLzLiteBlock(
			    storedBlock.data(),
			    storedBlock.size(),
			    blockHeader.originalSize,
			    outputBlock
			);
			if (result.error != CompressionError::Ok) {
				return result;
			}
			if (espCompressorCrc32(outputBlock.data(), outputBlock.size()) != blockHeader.crc32) {
				result.error = CompressionError::CorruptData;
				return result;
			}
			result.error = ctx.sink->write(outputBlock.data(), outputBlock.size());
			if (result.error != CompressionError::Ok) {
				return result;
			}
			result.outputBytes += outputBlock.size();
		}

		result.blocksProcessed++;
		result.inputBytes = processedCompressedInput;
		progress.processedInputBytes = processedCompressedInput;
		progress.producedOutputBytes = result.outputBytes;
		emitProgress(ctx, progress, result, false);
	}

	if (!(header.flags & kEscHeaderFlagUnknownOriginalSize) && result.outputBytes != header.originalSize) {
		result.error = CompressionError::CorruptData;
		return result;
	}

	if (shouldAbortForCancel(ctx, result)) {
		return result;
	}

	result.error = ctx.sink->commit();
	if (result.error != CompressionError::Ok) {
		return result;
	}
	session.committed = true;
	result.inputBytes = processedCompressedInput;
	progress.processedInputBytes = result.inputBytes;
	progress.producedOutputBytes = result.outputBytes;
	emitProgress(ctx, progress, result, true);
	return result;
}

CompressionResult runSyncJob(const RunContext &ctx) {
	return ctx.operation == CompressionOperation::Compress ? runCompress(ctx) : runDecompress(ctx);
}

} // namespace

struct CompressionJobControl {
	uint32_t id = 0;
	CompressionOperation operation = CompressionOperation::Compress;
	std::shared_ptr<CompressionSource> source;
	std::shared_ptr<CompressionSink> sink;
	CompressionCallbacks callbacks{};
	CompressionJobOptions options{};
	std::atomic<bool> cancelRequested{false};
	std::atomic<CompressionJobState> state{CompressionJobState::Idle};
	std::mutex mutex;
	CompressionResult result{};
};

const char *compressionErrorToString(CompressionError error) {
	switch (error) {
		case CompressionError::Ok:
			return "Ok";
		case CompressionError::NotInitialized:
			return "Not initialized";
		case CompressionError::Busy:
			return "Busy";
		case CompressionError::InvalidArgument:
			return "Invalid argument";
		case CompressionError::OpenFailed:
			return "Open failed";
		case CompressionError::ReadFailed:
			return "Read failed";
		case CompressionError::WriteFailed:
			return "Write failed";
		case CompressionError::OutputOverflow:
			return "Output overflow";
		case CompressionError::Cancelled:
			return "Cancelled";
		case CompressionError::CorruptData:
			return "Corrupt data";
		case CompressionError::UnsupportedVersion:
			return "Unsupported version";
		case CompressionError::UnsupportedAlgorithm:
			return "Unsupported algorithm";
		case CompressionError::NoMemory:
			return "No memory";
		case CompressionError::InternalError:
			return "Internal error";
		default:
			return "Unknown";
	}
}

bool CompressionJobHandle::valid() const noexcept {
	return static_cast<bool>(_control);
}

bool CompressionJobHandle::done() const noexcept {
	if (!_control) {
		return true;
	}
	const CompressionJobState current = _control->state.load(std::memory_order_acquire);
	return current == CompressionJobState::Completed || current == CompressionJobState::Failed ||
	       current == CompressionJobState::Cancelled || current == CompressionJobState::Rejected;
}

bool CompressionJobHandle::cancel() const noexcept {
	if (!_control) {
		return false;
	}
	_control->cancelRequested.store(true, std::memory_order_release);
	return true;
}

CompressionJobState CompressionJobHandle::state() const noexcept {
	return _control ? _control->state.load(std::memory_order_acquire) : CompressionJobState::Rejected;
}

CompressionResult CompressionJobHandle::result() const noexcept {
	if (!_control) {
		CompressionResult result{};
		result.error = CompressionError::InternalError;
		return result;
	}
	std::lock_guard<std::mutex> guard(_control->mutex);
	return _control->result;
}

uint32_t CompressionJobHandle::id() const noexcept {
	return _control ? _control->id : 0;
}

ESPCompressor::~ESPCompressor() {
	deinit();
}

CompressionError ESPCompressor::init(const ESPCompressorConfig &config) noexcept {
	if (config.blockSize == 0 || config.windowSize == 0 || config.taskStackSize == 0 ||
	    config.blockSize > UINT32_MAX || config.windowSize > 0xFFFFu) {
		return CompressionError::InvalidArgument;
	}

	std::lock_guard<std::mutex> guard(_mutex);
	if (_busy) {
		return CompressionError::Busy;
	}
	_config = config;
	_initialized = true;
	return CompressionError::Ok;
}

void ESPCompressor::deinit() noexcept {
	std::shared_ptr<CompressionJobControl> active;
	{
		std::lock_guard<std::mutex> guard(_mutex);
		_initialized = false;
		active = _activeJob;
	}

	if (active) {
		active->cancelRequested.store(true, std::memory_order_release);
		while (true) {
			CompressionJobState state = active->state.load(std::memory_order_acquire);
			if (state == CompressionJobState::Completed || state == CompressionJobState::Failed ||
			    state == CompressionJobState::Cancelled || state == CompressionJobState::Rejected) {
				break;
			}
			sleepBriefly();
		}
	}

	while (true) {
		bool busy = false;
		{
			std::lock_guard<std::mutex> guard(_mutex);
			busy = _busy;
			if (!busy) {
				_activeJob.reset();
			}
		}
		if (!busy) {
			break;
		}
		sleepBriefly();
	}
}

bool ESPCompressor::isInitialized() const noexcept {
	std::lock_guard<std::mutex> guard(_mutex);
	return _initialized;
}

CompressionResult ESPCompressor::compress(
    CompressionSource &source,
    CompressionSink &sink,
    ProgressCallback onProgress,
    const CompressionJobOptions &options
) noexcept {
	RunContext ctx{};
	{
		std::lock_guard<std::mutex> guard(_mutex);
		if (!_initialized) {
			return makeResult(CompressionOperation::Compress, CompressionError::NotInitialized);
		}
		if (_busy) {
			return makeResult(CompressionOperation::Compress, CompressionError::Busy);
		}
		_busy = true;
		ctx.config = &_config;
	}
	ScopedSyncRunSlot runSlot(_busy, _mutex);
	ctx.operation = CompressionOperation::Compress;
	ctx.source = &source;
	ctx.sink = &sink;
	ctx.onProgress = std::move(onProgress);
	ctx.options = options;
	CompressionResult result = runSyncJob(ctx);
	runSlot.release();
	std::lock_guard<std::mutex> guard(_mutex);
	_lastResult = result;
	return result;
}

CompressionResult ESPCompressor::decompress(
    CompressionSource &source,
    CompressionSink &sink,
    ProgressCallback onProgress,
    const CompressionJobOptions &options
) noexcept {
	RunContext ctx{};
	{
		std::lock_guard<std::mutex> guard(_mutex);
		if (!_initialized) {
			return makeResult(CompressionOperation::Decompress, CompressionError::NotInitialized);
		}
		if (_busy) {
			return makeResult(CompressionOperation::Decompress, CompressionError::Busy);
		}
		_busy = true;
		ctx.config = &_config;
	}
	ScopedSyncRunSlot runSlot(_busy, _mutex);
	ctx.operation = CompressionOperation::Decompress;
	ctx.source = &source;
	ctx.sink = &sink;
	ctx.onProgress = std::move(onProgress);
	ctx.options = options;
	CompressionResult result = runSyncJob(ctx);
	runSlot.release();
	std::lock_guard<std::mutex> guard(_mutex);
	_lastResult = result;
	return result;
}

CompressionJobHandle ESPCompressor::compressAsync(
    std::shared_ptr<CompressionSource> source,
    std::shared_ptr<CompressionSink> sink,
    const CompressionCallbacks &callbacks,
    const CompressionJobOptions &options
) noexcept {
	return submitAsync(CompressionOperation::Compress, std::move(source), std::move(sink), callbacks, options);
}

CompressionJobHandle ESPCompressor::decompressAsync(
    std::shared_ptr<CompressionSource> source,
    std::shared_ptr<CompressionSink> sink,
    const CompressionCallbacks &callbacks,
    const CompressionJobOptions &options
) noexcept {
	return submitAsync(CompressionOperation::Decompress, std::move(source), std::move(sink), callbacks, options);
}

CompressionJobHandle ESPCompressor::submitAsync(
    CompressionOperation operation,
    std::shared_ptr<CompressionSource> source,
    std::shared_ptr<CompressionSink> sink,
    const CompressionCallbacks &callbacks,
    const CompressionJobOptions &options
) noexcept {
	auto job = std::make_shared<CompressionJobControl>();
	job->operation = operation;
	job->source = std::move(source);
	job->sink = std::move(sink);
	job->callbacks = callbacks;
	job->options = options;
	job->result.operation = operation;

	{
		std::lock_guard<std::mutex> guard(_mutex);
		job->id = _nextJobId++;
		if (!_initialized) {
			job->state.store(CompressionJobState::Rejected, std::memory_order_release);
			job->result.error = CompressionError::NotInitialized;
		} else if (!job->source || !job->sink) {
			job->state.store(CompressionJobState::Rejected, std::memory_order_release);
			job->result.error = CompressionError::InvalidArgument;
		} else if (_busy) {
			job->state.store(CompressionJobState::Rejected, std::memory_order_release);
			job->result.error = CompressionError::Busy;
		} else {
			job->state.store(CompressionJobState::Running, std::memory_order_release);
			_busy = true;
			_activeJob = job;
		}
		if (job->state.load(std::memory_order_acquire) == CompressionJobState::Rejected) {
			_lastResult = job->result;
		}
	}

	if (job->state.load(std::memory_order_acquire) == CompressionJobState::Rejected) {
		if (job->callbacks.onComplete) {
			job->callbacks.onComplete(job->result);
		}
		return CompressionJobHandle(job);
	}

#if ESPCOMPRESSOR_HAS_FREERTOS
	struct AsyncTaskArgs {
		ESPCompressor *owner = nullptr;
		std::shared_ptr<CompressionJobControl> job;
	};

	auto *args = new (std::nothrow) AsyncTaskArgs{this, job};
	if (!args) {
		return rejectAcceptedAsyncSetupJob(job, CompressionError::NoMemory);
	}

	const BaseType_t created = xTaskCreatePinnedToCore(
	    [](void *ptr) {
		    std::unique_ptr<AsyncTaskArgs> args(static_cast<AsyncTaskArgs *>(ptr));
		    RunContext ctx{};
		    {
			    std::lock_guard<std::mutex> guard(args->owner->_mutex);
			    ctx.config = &args->owner->_config;
		    }
		    ctx.operation = args->job->operation;
		    ctx.source = args->job->source.get();
		    ctx.sink = args->job->sink.get();
		    ctx.onProgress = args->job->callbacks.onProgress;
		    ctx.options = args->job->options;
		    ctx.cancelRequested = &args->job->cancelRequested;
		    CompressionResult result = runSyncJob(ctx);
		    {
			    std::lock_guard<std::mutex> guard(args->job->mutex);
			    args->job->result = result;
		    }
		    args->job->state.store(
		        result.error == CompressionError::Ok
		            ? CompressionJobState::Completed
		            : (result.error == CompressionError::Cancelled ? CompressionJobState::Cancelled
		                                                          : CompressionJobState::Failed),
		        std::memory_order_release
		    );
		    args->owner->finishAsyncJob(args->job);
		    vTaskDelete(nullptr);
	    },
	    "ESPCompressor",
	    static_cast<uint32_t>((_config.taskStackSize + sizeof(StackType_t) - 1) / sizeof(StackType_t)),
	    args,
	    _config.taskPriority,
	    nullptr,
	    _config.coreId
	);
	if (created != pdPASS) {
		delete args;
		return rejectAcceptedAsyncSetupJob(job, CompressionError::InternalError);
	}
#else
	std::thread([this, job]() {
		RunContext ctx{};
		{
			std::lock_guard<std::mutex> guard(_mutex);
			ctx.config = &_config;
		}
		ctx.operation = job->operation;
		ctx.source = job->source.get();
		ctx.sink = job->sink.get();
		ctx.onProgress = job->callbacks.onProgress;
		ctx.options = job->options;
		ctx.cancelRequested = &job->cancelRequested;
		CompressionResult result = runSyncJob(ctx);
		{
			std::lock_guard<std::mutex> guard(job->mutex);
			job->result = result;
		}
		job->state.store(
		    result.error == CompressionError::Ok
		        ? CompressionJobState::Completed
		        : (result.error == CompressionError::Cancelled ? CompressionJobState::Cancelled
		                                                      : CompressionJobState::Failed),
		    std::memory_order_release
		);
		finishAsyncJob(job);
	}).detach();
#endif

	return CompressionJobHandle(job);
}

CompressionJobHandle ESPCompressor::rejectAcceptedAsyncSetupJob(
    const std::shared_ptr<CompressionJobControl> &job,
    CompressionError error
) noexcept {
	job->state.store(CompressionJobState::Rejected, std::memory_order_release);
	{
		std::lock_guard<std::mutex> guard(job->mutex);
		job->result.error = error;
	}
	finishAsyncJob(job);
	return CompressionJobHandle(job);
}

void ESPCompressor::finishAsyncJob(const std::shared_ptr<CompressionJobControl> &job) noexcept {
	CompletionCallback callback;
	CompressionResult result{};
	{
		std::lock_guard<std::mutex> guard(job->mutex);
		result = job->result;
		callback = job->callbacks.onComplete;
	}

	{
		std::lock_guard<std::mutex> guard(_mutex);
		_lastResult = result;
		if (_activeJob == job) {
			_activeJob.reset();
		}
		_busy = false;
	}

	if (callback) {
		callback(result);
	}
}

#ifdef ESPCOMPRESSOR_TESTING
CompressionJobHandle espCompressorTestSimulateAsyncSetupFailure(
    ESPCompressor &compressor,
    CompressionOperation operation,
    CompressionError error,
    const CompressionCallbacks &callbacks
) noexcept {
	auto job = std::make_shared<CompressionJobControl>();
	job->id = 0;
	job->operation = operation;
	job->callbacks = callbacks;
	job->result.operation = operation;

	{
		std::lock_guard<std::mutex> guard(compressor._mutex);
		job->id = compressor._nextJobId++;
		compressor._busy = true;
		compressor._activeJob = job;
	}

	return compressor.rejectAcceptedAsyncSetupJob(job, error);
}
#endif

bool ESPCompressor::cancel(const CompressionJobHandle &handle) noexcept {
	return handle.cancel();
}

bool ESPCompressor::isBusy() const noexcept {
	std::lock_guard<std::mutex> guard(_mutex);
	return _busy;
}

CompressionResult ESPCompressor::lastResult() const noexcept {
	std::lock_guard<std::mutex> guard(_mutex);
	return _lastResult;
}
