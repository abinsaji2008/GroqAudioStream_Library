#include "GroqAudioStream.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {
static const size_t MAX_RESPONSE_BYTES = 8192;

String lowerCopy(String value) {
  value.toLowerCase();
  return value;
}
}

GroqAudioStream::GroqAudioStream()
    : port_(443), sampleRate_(16000), timeoutMs_(30000), httpStatus_(0),
      insecureTLS_(true), configured_(false), active_(false) {}


bool GroqAudioStream::beginMicrophone(I2SClass &mic, const MicrophoneConfig &config) {
  if (config.sampleRate == 0) {
    lastError_ = "Microphone sample rate must be greater than zero";
    return false;
  }

  mic.setTimeout(config.readTimeoutMs < 1 ? 1 : config.readTimeoutMs);
  mic.setPins(config.bclkPin, config.wsPin, -1, config.dataPin, -1);

  if (!mic.begin(I2S_MODE_STD, config.sampleRate,
                 I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO,
                 config.useLeftChannel ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT)) {
    lastError_ = "I2S microphone initialization failed; check BCLK, WS and SD pins";
    mic.end();
    return false;
  }

  if (!mic.configureRX(config.sampleRate,
                       I2S_DATA_BIT_WIDTH_32BIT,
                       I2S_SLOT_MODE_MONO,
                       I2S_RX_TRANSFORM_32_TO_16)) {
    lastError_ = "I2S microphone RX configuration failed";
    mic.end();
    return false;
  }

  lastError_ = "";
  return true;
}

void GroqAudioStream::endMicrophone(I2SClass &mic) {
  mic.end();
}

bool GroqAudioStream::begin(const char *endpointUrl,
                            const char *apiKey,
                            const char *model,
                            const char *language,
                            uint32_t sampleRate,
                            const char *boundary) {
  abort();
  lastError_ = "";
  httpStatus_ = 0;

  if (!endpointUrl || !apiKey || !model || !language || !boundary ||
      !endpointUrl[0] || !apiKey[0] || !model[0] || !boundary[0]) {
    lastError_ = "Missing endpoint, API key, model, or multipart boundary";
    configured_ = false;
    return false;
  }

  apiKey_ = apiKey;
  model_ = model;
  language_ = language;
  boundary_ = boundary;
  sampleRate_ = sampleRate;

  if (sampleRate_ == 0) {
    lastError_ = "Sample rate must be greater than zero";
    configured_ = false;
    return false;
  }

  configured_ = parseEndpoint(endpointUrl);
  return configured_;
}

void GroqAudioStream::setInsecureTLS(bool enabled) {
  insecureTLS_ = enabled;
}

void GroqAudioStream::setCACert(const char *rootCaPem) {
  if (rootCaPem && rootCaPem[0]) {
    client_.setCACert(rootCaPem);
    insecureTLS_ = false;
  }
}

void GroqAudioStream::setTimeoutMs(uint32_t timeoutMs) {
  timeoutMs_ = timeoutMs < 1000 ? 1000 : timeoutMs;
  client_.setTimeout(timeoutMs_);
}

bool GroqAudioStream::parseEndpoint(const char *endpointUrl) {
  String url(endpointUrl);
  url.trim();

  if (!url.startsWith("https://")) {
    lastError_ = "Endpoint URL must start with https://";
    return false;
  }

  String rest = url.substring(8);
  int pathStart = rest.indexOf('/');
  String authority;
  if (pathStart < 0) {
    authority = rest;
    path_ = "/";
  } else {
    authority = rest.substring(0, pathStart);
    path_ = rest.substring(pathStart);
  }

  // Disallow URL user-info and fragments; keep endpoint parsing predictable.
  if (authority.length() == 0 || authority.indexOf('@') >= 0 ||
      path_.indexOf('#') >= 0) {
    lastError_ = "Invalid HTTPS endpoint URL";
    return false;
  }

  int colon = authority.lastIndexOf(':');
  if (colon > 0) {
    host_ = authority.substring(0, colon);
    long portValue = authority.substring(colon + 1).toInt();
    if (portValue < 1 || portValue > 65535) {
      lastError_ = "Invalid HTTPS endpoint port";
      return false;
    }
    port_ = (uint16_t)portValue;
  } else {
    host_ = authority;
    port_ = 443;
  }

  if (host_.length() == 0 || path_.length() == 0) {
    lastError_ = "Invalid endpoint host or path";
    return false;
  }

  // Query strings may be part of a valid endpoint path and are retained.
  return true;
}

bool GroqAudioStream::startStream() {
  if (!configured_) {
    lastError_ = "Call begin() with valid configuration before startStream()";
    return false;
  }

  if (active_) abort();
  lastError_ = "";
  httpStatus_ = 0;

  if (insecureTLS_) {
    client_.setInsecure();
  }
  client_.setTimeout(timeoutMs_);

  if (!client_.connect(host_.c_str(), port_)) {
    lastError_ = "TLS connection to transcription endpoint failed";
    client_.stop();
    return false;
  }

  String request;
  request.reserve(320 + host_.length() + path_.length() + apiKey_.length() + boundary_.length());
  request += "POST ";
  request += path_;
  request += " HTTP/1.1\r\nHost: ";
  request += host_;
  request += "\r\nAuthorization: Bearer ";
  request += apiKey_;
  request += "\r\nUser-Agent: ESP32-GroqAudioStream\r\nContent-Type: multipart/form-data; boundary=";
  request += boundary_;
  request += "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";

  if (client_.print(request) != request.length()) {
    lastError_ = "Failed to send HTTP request headers";
    client_.stop();
    return false;
  }

  active_ = true;

  if (!sendFormField("model", model_) ||
      (language_.length() && !sendFormField("language", language_)) ||
      !sendFormField("response_format", "text")) {
    lastError_ = "Failed to send multipart form fields";
    abort();
    return false;
  }

  String fileHeader;
  fileHeader.reserve(boundary_.length() + 130);
  fileHeader += "--";
  fileHeader += boundary_;
  fileHeader += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  if (!sendTextChunk(fileHeader)) {
    lastError_ = "Failed to send audio multipart header";
    abort();
    return false;
  }

  uint8_t wavHeader[44];
  makeWavHeader(wavHeader);
  if (!sendChunk(wavHeader, sizeof(wavHeader))) {
    lastError_ = "Failed to send WAV header";
    abort();
    return false;
  }

  return true;
}

bool GroqAudioStream::writeAudio(const uint8_t *data, size_t length) {
  if (!active_) {
    lastError_ = "No active stream; call startStream() first";
    return false;
  }
  if (!data && length) {
    lastError_ = "Audio data pointer is null";
    return false;
  }
  if (length == 0) return true;

  if (!sendChunk(data, length)) {
    lastError_ = "Failed while sending an audio chunk";
    client_.stop();
    active_ = false;
    return false;
  }
  return true;
}

bool GroqAudioStream::finish(String &transcript) {
  transcript = "";
  if (!active_) {
    if (lastError_.length() == 0) lastError_ = "No active stream to finish";
    return false;
  }

  String tail;
  tail.reserve(boundary_.length() + 12);
  tail += "\r\n--";
  tail += boundary_;
  tail += "--\r\n";

  if (!sendTextChunk(tail)) {
    lastError_ = "Failed to close multipart file field";
    abort();
    return false;
  }

  static const uint8_t finalChunk[] = {'0', '\r', '\n', '\r', '\n'};
  if (client_.write(finalChunk, sizeof(finalChunk)) != sizeof(finalChunk)) {
    lastError_ = "Failed to terminate HTTP chunked request";
    abort();
    return false;
  }

  active_ = false;
  String body;
  bool gotResponse = readResponse(body);
  client_.stop();

  if (!gotResponse) {
    if (lastError_.length() == 0) lastError_ = "Timed out or failed while reading Groq response";
    return false;
  }

  body.trim();
  if (httpStatus_ < 200 || httpStatus_ >= 300) {
    lastError_ = "Groq returned HTTP " + String(httpStatus_) + ": " + body;
    return false;
  }

  if (body.length() == 0) {
    lastError_ = "Groq returned an empty transcription";
    return false;
  }

  transcript = body;
  return true;
}

void GroqAudioStream::abort() {
  if (client_.connected() || client_.available()) client_.stop();
  else client_.stop();
  active_ = false;
}

bool GroqAudioStream::isActive() const { return active_; }
int GroqAudioStream::lastHttpStatus() const { return httpStatus_; }
String GroqAudioStream::lastError() const { return lastError_; }

bool GroqAudioStream::sendChunk(const uint8_t *data, size_t length) {
  if (!active_ || !data) return false;

  char sizeLine[20];
  int count = snprintf(sizeLine, sizeof(sizeLine), "%lX\r\n", (unsigned long)length);
  if (count <= 0 || (size_t)count >= sizeof(sizeLine)) return false;
  if (client_.write((const uint8_t *)sizeLine, (size_t)count) != (size_t)count) return false;
  if (length && client_.write(data, length) != length) return false;
  static const uint8_t crlf[] = {'\r', '\n'};
  return client_.write(crlf, sizeof(crlf)) == sizeof(crlf);
}

bool GroqAudioStream::sendTextChunk(const String &text) {
  return sendChunk((const uint8_t *)text.c_str(), text.length());
}

bool GroqAudioStream::sendFormField(const char *name, const String &value) {
  String field;
  field.reserve(boundary_.length() + strlen(name) + value.length() + 80);
  field += "--";
  field += boundary_;
  field += "\r\nContent-Disposition: form-data; name=\"";
  field += name;
  field += "\"\r\n\r\n";
  field += value;
  field += "\r\n";
  return sendTextChunk(field);
}

void GroqAudioStream::makeWavHeader(uint8_t header[44]) const {
  // For a live-uploaded WAV the final payload length is not yet known.
  // 0xFFFFFFFF marks an unknown/streaming RIFF and data length, matching the
  // streaming strategy used by the caller's original firmware.
  const uint32_t unknownSize = 0xFFFFFFFFUL;
  const uint32_t sampleRate = sampleRate_;
  const uint32_t byteRate = sampleRate_ * 2UL;
  const uint32_t fmtLength = 16;
  const uint16_t pcm = 1;
  const uint16_t channels = 1;
  const uint16_t blockAlign = 2;
  const uint16_t bitsPerSample = 16;

  memcpy(header, "RIFF", 4);
  memcpy(header + 4, &unknownSize, 4);
  memcpy(header + 8, "WAVEfmt ", 8);
  memcpy(header + 16, &fmtLength, 4);
  memcpy(header + 20, &pcm, 2);
  memcpy(header + 22, &channels, 2);
  memcpy(header + 24, &sampleRate, 4);
  memcpy(header + 28, &byteRate, 4);
  memcpy(header + 32, &blockAlign, 2);
  memcpy(header + 34, &bitsPerSample, 2);
  memcpy(header + 36, "data", 4);
  memcpy(header + 40, &unknownSize, 4);
}

bool GroqAudioStream::readLine(String &line, uint32_t timeoutMs) {
  line = "";
  const uint32_t start = millis();

  while ((uint32_t)(millis() - start) < timeoutMs) {
    while (client_.available()) {
      int raw = client_.read();
      if (raw < 0) break;
      char c = (char)raw;
      if (c == '\n') return true;
      if (c != '\r') {
        if (line.length() < 1024) line += c;
      }
    }

    if (!client_.connected() && !client_.available()) {
      return line.length() > 0;
    }
    delay(1);
  }
  return false;
}

bool GroqAudioStream::readExactBody(size_t length, String &body, uint32_t startedMs) {
  size_t remaining = length;
  while (remaining > 0 && body.length() < MAX_RESPONSE_BYTES) {
    if (client_.available()) {
      int raw = client_.read();
      if (raw >= 0) {
        body += (char)raw;
        --remaining;
      }
    } else {
      if (!client_.connected()) break;
      if ((uint32_t)(millis() - startedMs) > timeoutMs_) return false;
      delay(1);
    }
  }
  return remaining == 0;
}

bool GroqAudioStream::readResponse(String &body) {
  body = "";
  bool chunked = false;
  long contentLength = -1;
  String line;
  bool sawStatus = false;

  // Read the status line.
  if (!readLine(line, timeoutMs_)) {
    lastError_ = "Timed out waiting for HTTP status line";
    return false;
  }
  int firstSpace = line.indexOf(' ');
  if (line.startsWith("HTTP/") && firstSpace >= 0) {
    httpStatus_ = line.substring(firstSpace + 1, firstSpace + 4).toInt();
    sawStatus = true;
  }
  if (!sawStatus) {
    lastError_ = "Invalid HTTP status line: " + line;
    return false;
  }

  // Read headers.
  uint8_t headerCount = 0;
  while (headerCount++ < 64) {
    if (!readLine(line, timeoutMs_)) {
      lastError_ = "Timed out reading HTTP response headers";
      return false;
    }
    if (line.length() == 0) break;
    String lower = lowerCopy(line);
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (lower.startsWith("content-length:")) {
      String value = line.substring(line.indexOf(':') + 1);
      value.trim();
      contentLength = value.toInt();
    }
  }

  if (chunked) {
    while (body.length() < MAX_RESPONSE_BYTES) {
      if (!readLine(line, timeoutMs_)) {
        lastError_ = "Timed out reading chunk size from HTTP response";
        return false;
      }
      int semicolon = line.indexOf(';');
      if (semicolon >= 0) line = line.substring(0, semicolon);
      line.trim();
      long chunkSize = strtol(line.c_str(), nullptr, 16);
      if (chunkSize < 0) {
        lastError_ = "Invalid chunk size in HTTP response";
        return false;
      }
      if (chunkSize == 0) {
        // Ignore optional trailers until the blank line or connection close.
        while (readLine(line, 1000) && line.length() > 0) {}
        break;
      }
      size_t remaining = (size_t)chunkSize;
      uint32_t chunkStarted = millis();
      while (remaining > 0 && body.length() < MAX_RESPONSE_BYTES) {
        if (client_.available()) {
          int raw = client_.read();
          if (raw >= 0) {
            body += (char)raw;
            --remaining;
          }
        } else {
          if (!client_.connected()) {
            lastError_ = "Connection closed inside HTTP response chunk";
            return false;
          }
          if ((uint32_t)(millis() - chunkStarted) > timeoutMs_) {
            lastError_ = "Timed out inside HTTP response chunk";
            return false;
          }
          delay(1);
        }
      }
      if (remaining != 0) {
        lastError_ = "Groq response exceeded the configured response buffer";
        return false;
      }
      // Consume the CRLF after this chunk.
      if (!readLine(line, timeoutMs_)) {
        lastError_ = "Missing CRLF after HTTP response chunk";
        return false;
      }
    }
  } else if (contentLength >= 0) {
    if ((size_t)contentLength > MAX_RESPONSE_BYTES) {
      lastError_ = "Groq response exceeds the configured response buffer";
      return false;
    }
    if (!readExactBody((size_t)contentLength, body, millis())) {
      lastError_ = "Timed out or connection closed while reading HTTP body";
      return false;
    }
  } else {
    // Some servers close the connection to delimit the body.
    uint32_t started = millis();
    while ((client_.connected() || client_.available()) && body.length() < MAX_RESPONSE_BYTES) {
      if (client_.available()) {
        int raw = client_.read();
        if (raw >= 0) body += (char)raw;
      } else {
        if ((uint32_t)(millis() - started) > timeoutMs_) {
          lastError_ = "Timed out reading HTTP response body";
          return false;
        }
        delay(1);
      }
    }
  }

  return true;
}
