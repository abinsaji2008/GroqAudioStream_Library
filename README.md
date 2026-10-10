# GroqAudioStream Arduino Library

Version: **1.2.0**

An ESP32 Arduino library that uploads 16-bit, mono PCM audio to the Groq Speech-to-Text API using HTTP/1.1 chunked transfer encoding.

## What belongs where

- **This library:** HTTPS request, multipart fields, WAV header, chunked PCM upload, response decoding and final transcript return.
- **Main sketch:** INMP441 pin constants, microphone reads/VAD, endpoint URL, GPT/LLM reasoning, JSON/action parsing and application logic.
- **secrets.h:** Wi-Fi credentials and Groq API key. Do not commit this file or publish the key.

This streams the audio upload while recording. Groq returns the completed transcription after upload finishes; it is not live partial-transcription streaming.

## Install

In Arduino IDE, choose **Sketch → Include Library → Add .ZIP Library…** and select the library ZIP, or place the `GroqAudioStream` folder in your Arduino `libraries` directory. Include it with:

```cpp
#include <GroqAudioStream.h>
```

## Configure INMP441 pins in the main sketch

Edit only these constants in the main `.ino` file to match your wiring:

```cpp
#define MIC_SD_PIN   16  // INMP441 SD -> ESP32 data-in
#define MIC_SCK_PIN  17  // INMP441 SCK/BCLK
#define MIC_WS_PIN   15  // INMP441 WS/LRCLK
#define MIC_USE_LEFT_CHANNEL true // INMP441 L/R connected to GND = left
```

Pass the pins into the library's configuration when starting the microphone:

```cpp
I2SClass mic;
GroqAudioStream groqAudio;

GroqAudioStream::MicrophoneConfig micConfig;
micConfig.dataPin = MIC_SD_PIN;
micConfig.bclkPin = MIC_SCK_PIN;
micConfig.wsPin = MIC_WS_PIN;
micConfig.sampleRate = 16000;
micConfig.useLeftChannel = MIC_USE_LEFT_CHANNEL;
micConfig.readTimeoutMs = 1000;

if (!groqAudio.beginMicrophone(mic, micConfig)) {
  Serial.println(groqAudio.lastError());
}
```

Use the same constants in `WakeNet9Arduino::Config` when using WakeNet9, so both libraries use the same physical pins:

```cpp
WakeNet9Arduino::Config wakeConfig;
wakeConfig.dataPin = MIC_SD_PIN;
wakeConfig.bclkPin = MIC_SCK_PIN;
wakeConfig.wsPin = MIC_WS_PIN;
wakeConfig.sampleRate = 16000;
wakeConfig.use32BitInput = true;
wakeConfig.useLeftChannel = MIC_USE_LEFT_CHANNEL;
wakeNet.begin(wakeConfig);
```

Defaults in library configuration structs are examples only. The main sketch can override them; the library source does not need editing when pins change.

## Keep endpoint and API key outside the library

Set the endpoint URL in the main sketch:

```cpp
const char *GROQ_STT_URL =
    "https://api.groq.com/openai/v1/audio/transcriptions";
```

Keep credentials in a local `secrets.h` file, which must not be uploaded:

```cpp
#pragma once
#define SEED_WIFI_SSID "your-wifi-name"
#define SEED_WIFI_PASS "your-wifi-password"
#define SEED_GROQ_KEY "your-groq-api-key"
```

Configure the library with the endpoint from the main sketch and key from `secrets.h`:

```cpp
groqAudio.begin(GROQ_STT_URL, SEED_GROQ_KEY,
                "whisper-large-v3-turbo", "en", 16000);
```

## Streaming API

```cpp
if (!groqAudio.startStream()) {
  Serial.println(groqAudio.lastError());
  return;
}

// During microphone capture, send each raw signed 16-bit little-endian
// mono PCM block as it is read from I2S:
groqAudio.writeAudio(audioBytes, bytesRead);

String transcript;
if (groqAudio.finish(transcript)) {
  Serial.println(transcript);
} else {
  Serial.println(groqAudio.lastError());
}
```

Call `groqAudio.abort()` on capture/upload failure, and call `groqAudio.endMicrophone(mic)` when the microphone is no longer needed.

## Audio format

- 16 kHz sample rate
- Mono
- Signed 16-bit little-endian PCM samples
- The library creates the WAV header. Do not send an additional WAV header via `writeAudio()`.

## TLS warning

For compatibility with common ESP32 sketches, the library defaults to `WiFiClientSecure::setInsecure()`, which encrypts traffic without verifying the server certificate. For production use, configure a trusted root CA using `setCACert()`.

## Release notes

### 1.2.0
- Documents and standardizes main-sketch INMP441 pin configuration through `MicrophoneConfig`.
- Keeps the endpoint URL in the main sketch and credentials supplied at runtime from `secrets.h`.
- Keeps transcription transport separate from the main sketch's LLM reasoning and device actions.
