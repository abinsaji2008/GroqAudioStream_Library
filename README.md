# GroqAudioStream Arduino Library

**Version 1.3.0** — one-call audio capture and upload streaming for ESP32 + INMP441 + Groq Whisper.

## Responsibility split

- **WakeNet9Arduino:** detects the wake word.
- **Main sketch:** checks Wi-Fi, releases WakeNet's I2S peripheral, calls this library, handles LLM reasoning, and prints the final answer.
- **GroqAudioStream:** captures microphone audio, applies filter/gain/VAD, streams the audio upload, and returns one `String` containing the transcript.
- **`secrets.h`:** keeps Wi-Fi credentials and Groq API key private.

This library **does not run LLM reasoning**. The returned transcript is passed to your main sketch's reasoning function. Your sketch can print only the final assistant answer.

## Install

Arduino IDE → **Sketch → Include Library → Add .ZIP Library…** → choose the library ZIP.

Requirements:
- ESP32 Arduino core with `ESP_I2S.h`.
- WakeNet9Arduino and a compatible ESP-SR model partition named `model` containing `wn9_hiesp` for the complete wake-word flow.
- ArduinoJson 6.x only if you use JSON handling in your main sketch.
- A local `secrets.h`; never commit it or publish your API key.

## Minimal API

Keep the endpoint URL in the main sketch and the API key in `secrets.h`:

```cpp
const char *GROQ_STT_URL =
    "https://api.groq.com/openai/v1/audio/transcriptions";

GroqAudioStream groqAudio;
GroqAudioStream::Config config; // defaults live in the library

// Override only what you need:
config.sdPin = MIC_SD_PIN;
config.sckPin = MIC_SCK_PIN;
config.wsPin = MIC_WS_PIN;
config.useLeftChannel = MIC_USE_LEFT_CHANNEL;
groqAudio.setConfig(config);

groqAudio.begin(GROQ_STT_URL, SEED_GROQ_KEY,
                "whisper-large-v3-turbo", "en");

// After checking Wi-Fi and stopping WakeNet's I2S:
String transcript = groqAudio.transcribe();
if (transcript.length()) {
  String answer = groqReason(transcript); // your main-sketch function
  Serial.println(answer);                 // final answer only
}
```

The public API intentionally contains only three operations: `setConfig()`, `begin()`, and `transcribe()`. `transcribe()` returns transcript text, or an empty string for no speech/failure. Error diagnostics are printed to Serial.

## Library defaults

All recording behavior defaults are inside `GroqAudioStream::Config`; override them in the main sketch if necessary.

| Setting | Default |
|---|---:|
| INMP441 SD | GPIO 16 |
| INMP441 SCK/BCLK | GPIO 17 |
| INMP441 WS/LRCLK | GPIO 15 |
| Channel | Left (connect INMP441 L/R to GND) |
| Sample rate | 16000 Hz |
| Maximum recording | 10000 ms |
| Wait for first voice | 4000 ms |
| Silence before stop | 2000 ms |
| Microphone read timeout | 250 ms |
| Groq timeout | 30000 ms |
| VAD threshold | 700 |
| Audio gain | 8.0 |
| High-pass filter cutoff | 120 Hz |

Example overrides:

```cpp
GroqAudioStream::Config config;
config.maxRecordMs = 12000;
config.vadThreshold = 850;
config.audioGain = 6.0f;
config.sdPin = 4; // example only; choose a suitable GPIO
groqAudio.setConfig(config);
```

The values correspond to: `MAX_RECORD_MS=10000`, `WAIT_FOR_FIRST_VOICE_MS=4000`, `SILENCE_TO_STOP_MS=2000`, `MIC_READ_TIMEOUT_MS=250`, `GROQ_TIMEOUT_MS=30000`, `VAD_THRESHOLD=700`, and `AUDIO_GAIN=8.0f`.

## secrets.h template

```cpp
#pragma once
#define SEED_WIFI_SSID "your-wifi-name"
#define SEED_WIFI_PASS "your-wifi-password"
#define SEED_GROQ_KEY "gsk_your_groq_api_key"
```

## Important notes

- Call `transcribe()` only after checking `WiFi.status() == WL_CONNECTED` and stopping WakeNet's I2S driver. Restart WakeNet after it returns.
- The audio upload is streamed while recording, but Groq returns the transcript after processing; this is not live partial-transcription streaming.
- The implementation uses a streaming WAV header with unknown final length. Validate API acceptance with your firmware and current endpoint before production.
- TLS currently uses `WiFiClientSecure::setInsecure()` for compatibility, so server certificate identity is not verified. Configure trusted-CA validation for production.
