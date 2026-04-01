#include <ESPCompressor.h>
#include <LittleFS.h>

ESPCompressor compressor;

void setup() {
    Serial.begin(115200);

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
        return;
    }

    if (compressor.init() != CompressionError::Ok) {
        Serial.println("compressor init failed");
        return;
    }

    File sourceFile = LittleFS.open("/roundtrip.txt", "w");
    sourceFile.print("ESPCompressor makes async backup payloads smaller when the data is repetitive.");
    sourceFile.close();

    FileSource source(LittleFS, "/roundtrip.txt");
    FileSink compressedSink(LittleFS, "/roundtrip.esc");
    CompressionResult compressResult = compressor.compress(source, compressedSink);
    Serial.printf("compress: %s\n", compressionErrorToString(compressResult.error));

    FileSource compressedSource(LittleFS, "/roundtrip.esc");
    FileSink restoredSink(LittleFS, "/roundtrip.out.txt");
    CompressionResult decompressResult = compressor.decompress(compressedSource, restoredSink);
    Serial.printf("decompress: %s\n", compressionErrorToString(decompressResult.error));
}

void loop() {
}
