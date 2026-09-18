# ESP32-CAM Media Server API Contract

This is the complete server contract for an ESP32-CAM client. The server is permanent content storage; the ESP32-CAM downloads a selected item directly into runtime memory. There is no upload, OTA, or server-side device state.

## 1. Server base URL

Configure the ESP32-CAM with one base URL, without a trailing slash:

```text
http://192.168.1.10:3000
```

For local browser testing use `http://localhost:3000`. An ESP32-CAM cannot use `localhost`; it needs the computer's LAN IP or a deployed public hostname. The ESP32-CAM and server must be reachable over the same network (or the server must be internet-accessible).

The server uses plain HTTP by default. Use HTTPS only if the ESP32 firmware is explicitly built to validate/use TLS.

## 2. Catalogue request

```http
GET /api/videos?page=0&limit=10 HTTP/1.1
Host: 192.168.1.10:3000
Accept: application/json
```

Parameters:

| Parameter | Type | Required | Rules |
| --- | --- | --- | --- |
| `page` | unsigned integer | no | Zero-based page number. Default `0`. |
| `limit` | unsigned integer | no | Items per page. Default `10`; configured maximum is `50`; minimum is `1`. |

Successful response: `200 OK`, `Content-Type: application/json; charset=utf-8`.

```json
{
  "page": 0,
  "limit": 10,
  "total": 1,
  "items": [
    {
      "id": "v0001",
      "name": "Example media slot",
      "video_url": "/media/v0001/video.bin",
      "audio_url": "/media/v0001/audio.bin",
      "metadata": {
        "description": "Optional data",
        "format": "STIK video and raw PCM audio"
      }
    }
  ]
}
```

Contract guarantees:

- `id` is stable and identifies the media directory.
- `total` is the total number of videos, not the number on this page.
- `items` is an empty array when the requested page is past the last item.
- `video_url` and `audio_url` are exact, server-relative download paths.
- `metadata` is optional and must not be required for playback.

## 3. Build absolute media URLs

The media URL fields are relative by design. Concatenate the configured base URL with the returned value:

```text
base URL:  http://192.168.1.10:3000
video_url: /media/v0001/video.bin
result:    http://192.168.1.10:3000/media/v0001/video.bin
```

Do not construct URLs from the ID or filenames in firmware. Use the exact returned URL fields so future catalogue entries can use supported alternate filenames.

## 4. Media downloads

```http
GET /media/v0001/video.bin HTTP/1.1
Host: 192.168.1.10:3000
```

```http
GET /media/v0001/audio.bin HTTP/1.1
Host: 192.168.1.10:3000
```

For a present file, both endpoints return:

- `200 OK`
- `Content-Type: application/octet-stream`
- `Content-Length: <exact byte count>`
- `Content-Disposition: attachment; filename="video.bin"` or `audio.bin`

The client must use `Content-Length` before allocating PSRAM. Reject a missing, zero, oversized, or unavailable length according to the firmware's memory limits. Read exactly that many bytes and treat a short read as a failed download. Do not send the bytes through a text/JSON parser.

## 5. Error responses

All API and media errors are JSON:

```json
{
  "error": {
    "code": "MEDIA_NOT_FOUND",
    "message": "Media file was not found."
  }
}
```

| Status | Code | Meaning | ESP32 action |
| --- | --- | --- | --- |
| 400 | `INVALID_QUERY` | Bad page/limit value | Correct request; do not retry unchanged. |
| 400 | `INVALID_MEDIA_PATH` | Malformed media URL | Reject the catalogue item. |
| 404 | `VIDEO_NOT_FOUND` | ID no longer exists | Refresh catalogue. |
| 404 | `MEDIA_NOT_FOUND` | Catalogue item exists but binary is absent | Show unavailable; do not play. |
| 405 | `METHOD_NOT_ALLOWED` | Not a GET/OPTIONS request | Firmware bug; use GET. |
| 500 | `MEDIA_READ_ERROR` | Server could not read a real file | Retry later or show error. |

Network failures, timeouts, DNS failures, and Wi-Fi disconnects occur before an HTTP response; handle them separately from these JSON errors.

## 6. Required ESP32-CAM client flow

1. Connect Wi-Fi.
2. Build `baseUrl + "/api/videos?page=0&limit=10"`.
3. Perform HTTP GET with a finite timeout.
4. Require HTTP 200 and JSON content type; read/parse the catalogue response.
5. Validate `page`, `limit`, `total`, and each chosen item's non-empty `id`, `video_url`, and `audio_url`.
6. Display the item names/IDs and let the user choose one.
7. Build absolute URLs using the returned URL fields.
8. GET the video URL; require HTTP 200 and a safe positive `Content-Length`; allocate/download into PSRAM.
9. GET the audio URL using the same checks; allocate/download into PSRAM.
10. Only after both downloads fully succeed, hand the in-memory buffers to the existing local player.
11. On any failure, release any partial buffers and return to selection. Preserve any currently playing media until the replacement is fully valid if the firmware supports that behavior.

For pagination, request `page + 1` only while `(page + 1) * limit < total`. A device UI can cache one fetched page, but it should refresh the catalogue after a server error or a user-initiated refresh.

## 7. Media compatibility owned by firmware/content pipeline

The server treats both files as opaque binary streams. The content pipeline must ensure that every catalogue entry is compatible with the existing player:

- `video.bin`: the existing STIK binary video format expected by the player.
- `audio.bin`: the exact raw PCM format expected by the player's audio path.
- Video/audio pair: same intended start time and compatible duration.
- Combined `Content-Length` values: must fit the ESP32-CAM's available PSRAM after all other allocations.

The server does not transcode, alter bytes, change audio sample rate, or resynchronize files. The management upload path checks the STIK magic and header fields for video, while the client should still validate its own downloaded media before playback.

## 8. Content author workflow

For ID `v0002`:

1. Use the browser control panel's **Add Media** form.
2. Supply a valid ID, name, and your real converted files.
3. The server stores them as `WEB/media/<id>/video.bin` and `audio.bin`, then updates the catalogue.
4. Test both exact returned URLs in a browser or with `curl -I` / `curl -O`.
5. Confirm the actual device can allocate, download, validate, and play the pair.

## 9. Browser and command-line tests

```sh
curl "http://localhost:3000/api/videos?page=0&limit=10"
curl -i "http://localhost:3000/media/v0001/video.bin"
```

The browser UI at `/` exposes the exact API response and direct links for every catalogue item. The included example intentionally has no media binaries, so its media request correctly returns `404 MEDIA_NOT_FOUND` until real files are supplied.
