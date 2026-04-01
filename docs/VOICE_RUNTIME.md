# MimicLaw Voice Runtime

This document describes the Xiaozhi-like voice path added for ESP32-S3:

1. Local mic PCM feeds ESP-SR WakeNet on-device.
2. Wake word route selects language, local acknowledgement clip, and preferred session language.
3. WakeNet is disabled for the active session to avoid self-triggering.
4. The device plays a local acknowledgement immediately.
5. User speech is streamed over WebSocket to an external ASR/TTS gateway.
6. Final ASR text is submitted into the existing MimicLaw agent loop as `channel="voice"`.
7. The agent response text is sent back to the gateway for TTS.
8. Streamed TTS audio is played locally.
9. WakeNet is re-enabled when the session ends or errors out.

## Module Layout

Runtime/system modules:

- `main/audio/audio_hal.*`: I2S microphone and speaker access, 16 kHz mono PCM, mic fan-out for multiple consumers.
- `main/wakeword/wakeword_service.*`: ESP-SR AFE/WakeNet lifecycle, local detection callback, enable/disable control.
- `main/wakeword/wakeword_router.*`: Explicit wake word to language and acknowledgement routing table.
- `main/voice/voice_session.*`: Half-duplex session coordinator and state machine owner.
- `main/voice/capture_controller.*`: Streams microphone PCM to the gateway only during active listening.
- `main/voice/playback_controller.*`: Queues local acknowledgements and streamed TTS chunks for speaker playback.
- `main/voice/voice_ws_client.*`: Device-side WebSocket client for the external gateway.
- `main/voice/voice_protocol.*`: JSON control message builders for the gateway protocol.
- `main/bridge/agent_bridge.*`: Pushes final ASR text into the existing MimicLaw inbound bus.
- `main/app/app_voice_main.*`: App-level startup and integration glue.

Agent/tool modules remain unchanged in role:

- The LLM agent loop still handles context building, tools, memory, and final response text.
- Microphone capture, speaker playback, wake word detection, WebSocket session control, and playback state are not LLM tools.

## State Machine

The first implementation is intentionally half-duplex:

- `VOICE_STATE_IDLE`
- `VOICE_STATE_WAKE_DETECTED`
- `VOICE_STATE_ACK_PLAYING`
- `VOICE_STATE_LISTENING`
- `VOICE_STATE_UPLOADING`
- `VOICE_STATE_THINKING`
- `VOICE_STATE_SPEAKING`
- `VOICE_STATE_ERROR`

Rules:

- WakeNet is only enabled in `IDLE`.
- The local acknowledgement is played once per wake event.
- Capture starts after the local acknowledgement by default.
- Any gateway or playback failure returns the device to `IDLE` and re-enables WakeNet.

## Wake Word Routing

Wake word routing is explicit in `main/wakeword/wakeword_router.c`.

Current default routes:

- Chinese route:
  - label: `你好小智`
  - preferred session language: `zh-CN`
  - local acknowledgement clip: `AUDIO_ACK_CLIP_ZH_WO_ZAI`
- English route:
  - label: `Hey Xiaozhi`
  - preferred session language: `en-US`
  - local acknowledgement clip: `AUDIO_ACK_CLIP_EN_IM_HERE`
  - marked as requiring a custom or explicitly selected packaged WakeNet model

The router resolves by:

- `wakenet_model_index`
- `wake_word_index`

Both values come directly from `afe_fetch_result_t`.

## Gateway Flow

The first transport is WebSocket.

Control messages are JSON:

- `hello`
- `listen_start`
- `listen_stop`
- `stt_partial`
- `stt_final`
- `llm_thinking`
- `tts_start`
- `tts_sentence_start`
- `tts_stop`
- `abort`
- `error`

Audio transport is binary websocket frames.

Current defaults:

- upstream audio format: `pcm_s16le`
- downstream audio format: `pcm_s16le`
- sample rate: `16000`
- channels: `1`
- frame duration: `60 ms`

This keeps the first implementation stable. If you want Opus later, add the codec layer behind the same `voice_protocol` and `capture_controller` seams.

## Agent Loop Integration

When the gateway sends a final transcript:

- `agent_bridge_submit_voice_text()` creates a bus message with:
  - `channel = "voice"`
  - `chat_id = session_id`
  - `session_id = session_id`
  - `msg_type = MIMI_MSG_TYPE_VOICE_TEXT_FINAL`
  - `preferred_language = route preferred language`

The existing agent loop then:

- builds prompt/context
- runs tools/memory as usual
- emits a normal text response

The outbound dispatcher intercepts `channel="voice"` and routes it back into `voice_session_handle_agent_response()` instead of Telegram, Feishu, or the inbound WebSocket server.

## Configuration Guide

Most voice settings live in `main/mimi_config.h`.

Required board wiring:

- `MIMI_AUDIO_MIC_BCLK_PIN`
- `MIMI_AUDIO_MIC_WS_PIN`
- `MIMI_AUDIO_MIC_DIN_PIN`
- `MIMI_AUDIO_SPK_BCLK_PIN`
- `MIMI_AUDIO_SPK_WS_PIN`
- `MIMI_AUDIO_SPK_DOUT_PIN`

Required gateway config:

- `MIMI_SECRET_VOICE_GATEWAY_URL` in `main/mimi_secrets.h`

Chinese wake word route:

- `MIMI_WAKEWORD_ZH_LABEL`
- `MIMI_WAKEWORD_ZH_LANGUAGE`
- `MIMI_WAKEWORD_ZH_MODEL_INDEX`
- `MIMI_WAKEWORD_ZH_WORD_INDEX`
- `MIMI_WAKEWORD_ZH_AVAILABLE`

English wake word route:

- `MIMI_WAKEWORD_EN_LABEL`
- `MIMI_WAKEWORD_EN_LANGUAGE`
- `MIMI_WAKEWORD_EN_MODEL_INDEX`
- `MIMI_WAKEWORD_EN_WORD_INDEX`
- `MIMI_WAKEWORD_EN_AVAILABLE`
- `MIMI_WAKEWORD_SECONDARY_MODEL_HINT`

WakeNet model selection:

- `MIMI_WAKEWORD_MODEL_PARTITION_LABEL`
- `MIMI_WAKEWORD_PRIMARY_MODEL_HINT`
- `MIMI_WAKEWORD_SECONDARY_MODEL_HINT`

Local acknowledgement mapping:

- `AUDIO_ACK_CLIP_ZH_WO_ZAI` maps to `main/assets/ack_wozai.c`
- `AUDIO_ACK_CLIP_EN_IM_HERE` maps to `main/assets/ack_im_here.c`

Session behavior:

- `MIMI_VOICE_START_UPLOAD_AFTER_ACK`
- `MIMI_VOICE_DEFAULT_LANGUAGE`
- `MIMI_VOICE_FRAME_DURATION_MS`
- `MIMI_VOICE_AUDIO_INPUT_FORMAT`
- `MIMI_VOICE_AUDIO_OUTPUT_FORMAT`

## Replacing Local Ack Assets

The current acknowledgement assets are placeholder PCM clips so the runtime path is self-contained and compilable.

To replace them with real local speech:

1. Prepare 16 kHz, mono, 16-bit PCM samples for:
   - `我在`
   - `I'm here`
2. Convert them into `int16_t` arrays.
3. Replace the arrays in:
   - `main/assets/ack_wozai.c`
   - `main/assets/ack_im_here.c`
4. Keep the clip IDs unchanged so the router mapping does not need to change.

## Build And Flash

From the repo root:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

The component manifest now includes:

- `espressif/esp_websocket_client`
- `espressif/esp-sr`

If you swap wake word models, rebuild after changing `main/mimi_config.h` or your model partition contents.

## Current Limits / TODO

- The voice runtime is single-session and half-duplex by design.
- The packaged ESP-SR bundle may not contain the desired English wake word. The route and config path are ready, but you may need to add a custom WakeNet model and update `MIMI_WAKEWORD_SECONDARY_MODEL_HINT`.
- The local acknowledgement assets should be replaced with real speech recordings for production use.
- The first implementation uses PCM over WebSocket. Opus can be added later behind the existing runtime seams.
