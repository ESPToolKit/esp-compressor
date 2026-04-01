#include <ArduinoJson.h>
#include <ESPCompressor.h>

#include <vector>

ESPCompressor compressor;

class StringStreamAdapter : public Stream {
  public:
    explicit StringStreamAdapter(const String &text) : _text(text) {
    }

    int available() override {
        return static_cast<int>(_text.length() - _offset);
    }

    int read() override {
        if (_offset >= _text.length()) {
            return -1;
        }
        return static_cast<uint8_t>(_text[_offset++]);
    }

    int peek() override {
        if (_offset >= _text.length()) {
            return -1;
        }
        return static_cast<uint8_t>(_text[_offset]);
    }

    size_t write(uint8_t) override {
        return 0;
    }

  private:
    String _text;
    size_t _offset = 0;
};

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

    StringStreamAdapter inputStream(json);
    StreamSource source(inputStream, json.length(), true);
    PrintSink sink(Serial);

    CompressionJobOptions options;
    options.allowPartialOutput = true;
    CompressionResult result = compressor.compress(source, sink, nullptr, options);
    Serial.printf("\nstatus: %s\n", compressionErrorToString(result.error));
}

void loop() {
}
