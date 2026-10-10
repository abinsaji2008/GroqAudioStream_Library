#ifndef GROQ_AUDIO_STREAM_H
#define GROQ_AUDIO_STREAM_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>

/**
 * GroqAudioStream
 *
 * Streams a 16-bit, mono PCM WAV payload to Groq's transcription endpoint
 * using HTTP/1.1 chunked transfer encoding. The caller owns microphone capture
 * and passes microphone bytes with writeAudio(). The final transcription is
 * returned by finish(); this is upload-streaming, not live partial transcription.
 *
 * Security note: by default TLS certificate verification is disabled to match
 * common ESP32 sketches. Call setInsecureTLS(false) only after configuring a
 * trusted CA certificate on the underlying client (this library does not embed
 * a CA certificate).
 */
class GroqAudioStream {
public:
  /** Pin and format configuration for an INMP441 connected to ESP32 I2S.
   * Set these fields in the main sketch. Defaults are only examples, not fixed pins.
   */
  struct MicrophoneConfig {
    int bclkPin = 17;          // INMP441 SCK/BCLK
    int wsPin = 15;            // INMP441 WS/LRCLK
    int dataPin = 16;          // INMP441 SD -> ESP32 data-in
    uint32_t sampleRate = 16000;
    uint32_t readTimeoutMs = 1000;
    bool useLeftChannel = true; // L/R connected to GND = left slot
  };

  GroqAudioStream();

  /** Configure and start the supplied ESP_I2S microphone with chosen pins.
   * The caller still owns readBytes() and must call endMicrophone() when done.
   */
  bool beginMicrophone(I2SClass &mic, const MicrophoneConfig &config);
  void endMicrophone(I2SClass &mic);

  /** Configure the HTTPS endpoint, API key, Whisper model, and language.
   * endpointUrl example: https://api.groq.com/openai/v1/audio/transcriptions
   */
  bool begin(const char *endpointUrl,
             const char *apiKey,
             const char *model = "whisper-large-v3-turbo",
             const char *language = "en",
             uint32_t sampleRate = 16000,
             const char *boundary = "----esp32-groq-audio");

  /** By default useInsecureTLS=true for compatibility; see header note above. */
  void setInsecureTLS(bool enabled = true);
  /** Configure a trusted root CA and enable certificate verification. */
  void setCACert(const char *rootCaPem);
  void setTimeoutMs(uint32_t timeoutMs);

  /** Connect and begin the multipart request. Call before writing audio. */
  bool startStream();

  /** Write raw signed 16-bit little-endian mono PCM samples. */
  bool writeAudio(const uint8_t *data, size_t length);

  /** Close the multipart request and wait for the final transcript. */
  bool finish(String &transcript);

  /** Abort the active request and close the socket. */
  void abort();

  bool isActive() const;
  int lastHttpStatus() const;
  String lastError() const;

private:
  WiFiClientSecure client_;
  String host_;
  String path_;
  String apiKey_;
  String model_;
  String language_;
  String boundary_;
  String lastError_;
  uint16_t port_;
  uint32_t sampleRate_;
  uint32_t timeoutMs_;
  int httpStatus_;
  bool insecureTLS_;
  bool configured_;
  bool active_;

  bool parseEndpoint(const char *endpointUrl);
  bool sendChunk(const uint8_t *data, size_t length);
  bool sendTextChunk(const String &text);
  bool sendFormField(const char *name, const String &value);
  void makeWavHeader(uint8_t header[44]) const;
  bool readLine(String &line, uint32_t timeoutMs);
  bool readResponse(String &body);
  bool readExactBody(size_t length, String &body, uint32_t startedMs);
};

#endif
