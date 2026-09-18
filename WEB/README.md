# ESP32 Media Library

A dependency-free Node.js content server and browser control panel. It holds permanent media, exposes the ESP32-compatible catalogue, and manages uploads, metadata, and recent request activity. It does not implement OTA or ESP32 filesystem storage.

## Run locally

Requires Node.js 18+.

```sh
cd WEB
npm start
```

Open `http://localhost:3000`. To make it available to a device on your LAN, find your computer's LAN IP and use `http://YOUR_LAN_IP:3000` on the ESP32-CAM. Make sure the firewall permits the selected port.

Configuration is in `config.json`. Environment overrides: `PORT`, `HOST`, `CORS_ENABLED=true|false`, and `CORS_ORIGIN`.

## API

The firmware-ready request/response specification and download sequence are in [API.md](API.md).

```sh
curl "http://localhost:3000/api/videos?page=0&limit=10"
curl -i "http://localhost:3000/media/v0001/video.bin"
```

`GET /api/videos?page=0&limit=10` returns `page`, `limit`, `total`, and catalogue `items`. `page` is zero-based. The configured maximum page size is 50. Invalid parameters return HTTP 400 and JSON error details. Unknown IDs and absent files return HTTP 404 JSON errors.

The ESP32-CAM should request the catalogue URL, choose an item, prepend its server base URL to the returned relative `video_url` / `audio_url`, then fetch those URLs directly. For example, `/media/v0001/video.bin` becomes `http://192.168.1.10:3000/media/v0001/video.bin`.

## Manage media in the browser

Open `http://localhost:3000`, choose **Add Media**, select your already-converted files, and submit. The server validates the ID, verifies the STIK video header, stores the exact supplied bytes as `media/<id>/video.bin` and `audio.bin`, and atomically updates `data/catalogue.json`.

IDs must be unique and use letters, numbers, `_`, or `-`. Do not manually edit the catalogue for normal operation.

## Management API

- `GET /api/status` — server totals and live in-memory activity summary.
- `GET /api/activity` — bounded recent HTTP request log.
- `GET /api/videos/:id` — management details, sizes, STIK inspection, and ESP API item.
- `POST /api/videos` — multipart add (`id`, `name`, `description`, `metadata`, `video`, `audio`).
- `PUT /api/videos/:id` — edit JSON metadata.
- `POST /api/videos/:id/video` or `/audio` — multipart replacement.
- `DELETE /api/videos/:id` — remove catalogue entry and media directory.

To make an ESP request identifiable in the dashboard, optionally send `X-ESP-CLIENT: ESP32-CAM`. Existing clients work without it; their requests are recorded as HTTP-client activity.

## Tests

```sh
npm test
```

The test suite verifies catalogue JSON, pagination, invalid-query handling, missing-ID handling, and missing-media handling. After placing actual files, test downloads with the browser or `curl -O` using the exact URL returned by the API.
