#include <ESPCompressor.h>

#include <vector>

ESPCompressor compressor;

void setup() {
    Serial.begin(115200);

    if (compressor.init() != CompressionError::Ok) {
        Serial.println("compressor init failed");
        return;
    }

    std::vector<uint8_t> input = {'E', 'S', 'P', 'T', 'o', 'o', 'l', 'K', 'i', 't'};
    BufferSource source(input.data(), input.size());

    std::vector<uint8_t> compressed;
    DynamicBufferSink compressedSink(compressed);
    CompressionResult compressResult = compressor.compress(source, compressedSink);
    Serial.printf("compress: %s, bytes=%u\n", compressionErrorToString(compressResult.error), static_cast<unsigned>(compressResult.outputBytes));

    BufferSource compressedSource(compressed.data(), compressed.size());
    std::vector<uint8_t> restored;
    DynamicBufferSink restoredSink(restored);
    CompressionResult decompressResult = compressor.decompress(compressedSource, restoredSink);
    Serial.printf("decompress: %s, restored=%u\n", compressionErrorToString(decompressResult.error), static_cast<unsigned>(restored.size()));
}

void loop() {
}
