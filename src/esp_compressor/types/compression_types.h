#pragma once

#include "../util/compat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class CompressionSource;
class CompressionSink;
class CompressionJobHandle;

constexpr size_t kESPCompressorDefaultBlockSize = 2048;
constexpr size_t kESPCompressorDefaultWindowSize = 2048;
constexpr size_t kESPCompressorDefaultTaskStackSize = 8192;

enum class CompressionOperation : uint8_t {
	Compress = 0,
	Decompress,
};

enum class CompressionError : uint8_t {
	Ok = 0,
	NotInitialized,
	Busy,
	InvalidArgument,
	OpenFailed,
	ReadFailed,
	WriteFailed,
	OutputOverflow,
	Cancelled,
	CorruptData,
	UnsupportedVersion,
	UnsupportedAlgorithm,
	NoMemory,
	InternalError,
};

enum class CompressionJobState : uint8_t {
	Idle = 0,
	Running,
	Completed,
	Failed,
	Cancelled,
	Rejected,
};

struct ESPCompressorConfig {
	bool usePSRAMBuffers = true;
	size_t blockSize = kESPCompressorDefaultBlockSize;
	size_t windowSize = kESPCompressorDefaultWindowSize;
	size_t taskStackSize = kESPCompressorDefaultTaskStackSize;
	UBaseType_t taskPriority = 2;
	BaseType_t coreId = tskNO_AFFINITY;
};

struct CompressionJobOptions {
	bool allowPartialOutput = false;
};

struct CompressionProgress {
	CompressionOperation operation = CompressionOperation::Compress;
	uint64_t totalInputBytes = 0;
	uint64_t processedInputBytes = 0;
	uint64_t totalOutputBytes = 0;
	uint64_t producedOutputBytes = 0;
	float percent = 0.0f;
	bool hasKnownTotalInput = false;
	bool done = false;
	bool cancelled = false;
};

struct CompressionResult {
	CompressionError error = CompressionError::Ok;
	CompressionOperation operation = CompressionOperation::Compress;
	uint64_t inputBytes = 0;
	uint64_t outputBytes = 0;
	size_t blocksProcessed = 0;

	bool ok() const {
		return error == CompressionError::Ok;
	}
};

using ProgressCallback = std::function<void(const CompressionProgress &)>;
using CompletionCallback = std::function<void(const CompressionResult &)>;

struct CompressionCallbacks {
	ProgressCallback onProgress{};
	CompletionCallback onComplete{};
};

const char *compressionErrorToString(CompressionError error);
