#include <ArduinoJson.h>
#include <ESPCompressor.h>

ESPCompressor compressor;

void setup() {
    Serial.begin(115200);

    if (compressor.init() != CompressionError::Ok) {
        Serial.println("compressor init failed");
        return;
    }

    JsonDocument doc;
    doc["library"] = "ESPCompressor";
    doc["feature"] = "stream";
    doc["value"] = 42;

    String json;
    serializeJson(doc, json);

    StreamString inputStream;
    inputStream.print(json);
    StreamSource source(inputStream, json.length(), true);
    PrintSink sink(Serial);

    CompressionJobOptions options;
    options.allowPartialOutput = true;
    CompressionResult result = compressor.compress(source, sink, nullptr, options);
    Serial.printf("\nstatus: %s\n", compressionErrorToString(result.error));
}

void loop() {
}
