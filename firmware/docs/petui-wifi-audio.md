# PetUI Wi-Fi audio

A PetUI component can declare a device-side playlist in its media configuration.

```json
{
  "media": {
    "source": "wifi.audio",
    "playlist": [
      {"title": "Local forecast", "url": "https://media.example.org/forecast.wav"},
      {"title": "Live station", "url": "https://radio.example.org/live.mp3", "live": true}
    ]
  }
}
```

Each playlist has 1–16 entries. Compact playlist JSON can occupy up to 7,163 bytes. A title is 1–90 UTF-8 bytes and may contain printable text. `live` is optional and defaults to `false`. The component's media controls can open the playlist, play or pause, select a track, move to the previous or next track, and change the volume. Playback starts on a user action. Leaving or replacing the visible component stops playback. Starting a finite track again after pausing restarts it from the beginning.

Finite tracks support MP3 and RIFF WAV with signed 16-bit PCM, one or two channels, and sample rates from 8–48 kHz. The server must return HTTP 200 with `Content-Type: audio/mpeg`, `audio/wav` or `audio/x-wav` and frame finite bodies with `Content-Length` or chunked transfer encoding. Finite bodies are limited to 256 MiB. Live entries support MP3. AAC, FLAC, HLS and interleaved ICY metadata are unsupported.

The device connects over HTTPS port 443 to a public IPv4 address obtained from DNS, using the original hostname and the firmware CA bundle for TLS verification. Use a DNS hostname and a direct media URL. Credentials, explicit ports, IP literals, fragments and redirects are rejected.

A live connection can reopen once after a read interruption. If Wi-Fi drops, playback waits up to 30 seconds for Wi-Fi to return before reopening the stream. Pause, stop, component changes and voice takeover cancel pending playback. A finite track that loses its connection after delivering audio requires a new play action.
