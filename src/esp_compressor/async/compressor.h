#pragma once

#include "../io/compression_io.h"
#include "../types/compression_types.h"

#include <mutex>
#include <memory>

struct CompressionJobControl;
class ESPCompressor;

#ifdef ESPCOMPRESSOR_TESTING
CompressionJobHandle espCompressorTestSimulateAsyncSetupFailure(
    ESPCompressor &compressor,
    CompressionOperation operation,
    CompressionError error,
    const CompressionCallbacks &callbacks = {}
) noexcept;
#endif

class CompressionJobHandle {
  public:
	CompressionJobHandle() = default;

	bool valid() const noexcept;
	bool done() const noexcept;
	bool cancel() const noexcept;
	CompressionJobState state() const noexcept;
	CompressionResult result() const noexcept;
	uint32_t id() const noexcept;

  private:
	friend class ESPCompressor;
	explicit CompressionJobHandle(std::shared_ptr<CompressionJobControl> control)
	    : _control(std::move(control)) {
	}

	std::shared_ptr<CompressionJobControl> _control;
};

class ESPCompressor {
  public:
	ESPCompressor() = default;
	~ESPCompressor();

	CompressionError init(const ESPCompressorConfig &config = {}) noexcept;
	void deinit() noexcept;
	bool isInitialized() const noexcept;

	CompressionResult compress(
	    CompressionSource &source,
	    CompressionSink &sink,
	    ProgressCallback onProgress = nullptr,
	    const CompressionJobOptions &options = {}
	) noexcept;

	CompressionResult decompress(
	    CompressionSource &source,
	    CompressionSink &sink,
	    ProgressCallback onProgress = nullptr,
	    const CompressionJobOptions &options = {}
	) noexcept;

	CompressionJobHandle compressAsync(
	    std::shared_ptr<CompressionSource> source,
	    std::shared_ptr<CompressionSink> sink,
	    const CompressionCallbacks &callbacks = {},
	    const CompressionJobOptions &options = {}
	) noexcept;

	CompressionJobHandle decompressAsync(
	    std::shared_ptr<CompressionSource> source,
	    std::shared_ptr<CompressionSink> sink,
	    const CompressionCallbacks &callbacks = {},
	    const CompressionJobOptions &options = {}
	) noexcept;

	bool cancel(const CompressionJobHandle &handle) noexcept;
	bool isBusy() const noexcept;
	CompressionResult lastResult() const noexcept;

  private:
#ifdef ESPCOMPRESSOR_TESTING
	friend CompressionJobHandle espCompressorTestSimulateAsyncSetupFailure(
	    ESPCompressor &compressor,
	    CompressionOperation operation,
	    CompressionError error,
	    const CompressionCallbacks &callbacks
	) noexcept;
#endif

	CompressionJobHandle submitAsync(
	    CompressionOperation operation,
	    std::shared_ptr<CompressionSource> source,
	    std::shared_ptr<CompressionSink> sink,
	    const CompressionCallbacks &callbacks,
	    const CompressionJobOptions &options
	) noexcept;

	CompressionJobHandle rejectAcceptedAsyncSetupJob(
	    const std::shared_ptr<CompressionJobControl> &job,
	    CompressionError error
	) noexcept;

	void finishAsyncJob(const std::shared_ptr<CompressionJobControl> &job) noexcept;

	ESPCompressorConfig _config{};
	bool _initialized = false;
	bool _busy = false;
	mutable std::mutex _mutex;
	std::shared_ptr<CompressionJobControl> _activeJob;
	CompressionResult _lastResult{};
	uint32_t _nextJobId = 1;
};
