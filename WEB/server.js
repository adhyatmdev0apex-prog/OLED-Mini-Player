'use strict';

const http = require('node:http');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');

const ID_REGEX = /^[A-Za-z0-9_-]+$/;
const MAX_ACTIVITY_LOG = 200;

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function getLocalIpAddresses() {
  const interfaces = os.networkInterfaces();
  const ips = [];
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name] || []) {
      if (iface.family === 'IPv4' && !iface.internal) {
        ips.push(iface.address);
      }
    }
  }
  return ips;
}

function formatBytes(bytes) {
  if (bytes === 0 || !bytes) return '0 B';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(2)} MB`;
}

function parseInteger(value, name, fallback, max) {
  if (value === null || value === undefined) return fallback;
  if (!/^\d+$/.test(value)) {
    throw Object.assign(new Error(`${name} must be a non-negative integer.`), { code: 'INVALID_QUERY', status: 400 });
  }
  const num = Number(value);
  if (!Number.isSafeInteger(num) || num > max) {
    throw Object.assign(new Error(`${name} is out of valid range (0 - ${max}).`), { code: 'INVALID_QUERY', status: 400 });
  }
  return num;
}

function inspectStikHeader(buffer) {
  if (!buffer || buffer.length < 16) {
    return { valid: false, reason: 'File is smaller than the 16-byte STIK header.' };
  }
  const magic = buffer.subarray(0, 4).toString('ascii');
  if (magic !== 'STIK') {
    return { valid: false, reason: `Invalid signature '${magic}' (expected 'STIK').` };
  }
  const width = buffer.readUInt16LE(4);
  const height = buffer.readUInt16LE(6);
  const fps = buffer.readUInt8(8);
  const frameCount = buffer.readUInt32LE(9);

  if (width !== 128 || height !== 64) {
    return {
      valid: false,
      width,
      height,
      fps,
      frameCount,
      reason: `Unsupported resolution ${width}x${height} (must be 128x64).`
    };
  }
  if (fps === 0) {
    return { valid: false, width, height, fps, frameCount, reason: 'FPS must be greater than 0.' };
  }
  if (frameCount === 0) {
    return { valid: false, width, height, fps, frameCount, reason: 'Frame count must be greater than 0.' };
  }

  return {
    valid: true,
    width,
    height,
    fps,
    frameCount,
    reason: null
  };
}

function parseMultipartFormData(buffer, contentType) {
  const match = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(contentType || '');
  if (!match) {
    throw Object.assign(new Error('Content-Type must be multipart/form-data with a boundary.'), { status: 400, code: 'INVALID_UPLOAD' });
  }
  const boundaryStr = match[1] || match[2];
  const boundary = Buffer.from(`--${boundaryStr}`);
  const fields = {};
  const files = {};

  let pos = buffer.indexOf(boundary) + boundary.length + 2;
  while (pos > boundary.length + 1 && pos < buffer.length) {
    const next = buffer.indexOf(boundary, pos);
    if (next < 0) break;

    const part = buffer.subarray(pos, next - 2);
    const splitIndex = part.indexOf(Buffer.from('\r\n\r\n'));
    if (splitIndex >= 0) {
      const headers = part.subarray(0, splitIndex).toString('utf8');
      const value = part.subarray(splitIndex + 4);
      const nameMatch = /name="([^"]+)"/i.exec(headers);
      const filenameMatch = /filename="([^"]*)"/i.exec(headers);

      if (nameMatch) {
        const fieldName = nameMatch[1];
        if (filenameMatch !== null && filenameMatch !== undefined) {
          files[fieldName] = {
            filename: filenameMatch[1],
            data: value
          };
        } else {
          fields[fieldName] = value.toString('utf8');
        }
      }
    }

    pos = next + boundary.length + 2;
    if (buffer[pos] === 45 && buffer[pos + 1] === 45) {
      break; // Trailing -- signifies end of multipart
    }
  }

  return { fields, files };
}

function loadConfig(root) {
  const configFile = path.join(root, 'config.json');
  let raw = {};
  try {
    raw = readJson(configFile);
  } catch {
    raw = {};
  }
  return {
    host: process.env.HOST || raw.host || '0.0.0.0',
    port: Number(process.env.PORT || raw.port || 3000),
    cors: {
      enabled: process.env.CORS_ENABLED ? process.env.CORS_ENABLED === 'true' : raw.cors?.enabled !== false,
      origin: process.env.CORS_ORIGIN || raw.cors?.origin || '*'
    },
    pagination: raw.pagination || { defaultLimit: 10, maxLimit: 50 },
    upload: raw.upload || { maxBytes: 32 * 1024 * 1024 } // 32MB max
  };
}

function createServer(options = {}) {
  const root = options.root || __dirname;
  const publicDir = path.join(root, 'public');
  const mediaDir = path.join(root, 'media');
  const catalogueFile = path.join(root, 'data', 'catalogue.json');
  const config = loadConfig(root);
  const startedAt = Date.now();
  const activityLog = [];

  const activityStats = {
    requestCount: 0,
    downloadsCount: 0,
    lastRequestTime: null,
    lastEspActivityTime: null,
    lastCatalogueFetch: null,
    lastMediaRequest: null,
    lastVideoRequest: null,
    lastAudioRequest: null,
    lastSuccessfulTransfer: null,
    lastError: null,
    lastMedia: null
  };

  function readCatalogue() {
    const source = readJson(catalogueFile);
    if (!Array.isArray(source.videos)) {
      throw new Error('catalogue.json must contain a videos array.');
    }
    return source;
  }

  async function saveCatalogue(source) {
    const tempFile = `${catalogueFile}.${process.pid}.${Date.now()}.tmp`;
    await fsp.writeFile(tempFile, `${JSON.stringify(source, null, 2)}\n`, 'utf8');
    await fsp.rename(tempFile, catalogueFile);
  }

  function formatPublicItem(item) {
    const base = `/media/${encodeURIComponent(item.id)}`;
    const isOffline = Boolean(item.offline_download === true || item.metadata?.offline_download === true);
    return {
      id: item.id,
      name: item.name,
      video_url: `${base}/video.bin`,
      audio_url: `${base}/audio.bin`,
      offline_download: isOffline,
      ...(item.metadata ? { metadata: item.metadata } : {})
    };
  }

  function sendJson(res, status, data) {
    const payload = Buffer.from(JSON.stringify(data, null, 2));
    res.writeHead(status, {
      'Content-Type': 'application/json; charset=utf-8',
      'Content-Length': payload.length,
      'Cache-Control': 'no-store'
    });
    res.end(payload);
  }

  function sendError(res, status, code, message) {
    sendJson(res, status, { error: { code, message } });
  }

  function applyCorsHeaders(res) {
    if (config.cors.enabled) {
      res.setHeader('Access-Control-Allow-Origin', config.cors.origin);
      res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-ESP-CLIENT');
    }
  }

  function identifyClient(req) {
    const customHeader = req.headers['x-esp-client'];
    if (customHeader) {
      return { client: String(customHeader), isEsp: true };
    }
    const userAgent = req.headers['user-agent'] || '';
    if (/ESP32/i.test(userAgent)) {
      return { client: 'ESP32-CAM', isEsp: true };
    }
    // If accessing /media/ without standard browser identifiers
    if (req.url?.startsWith('/media/') && !/Mozilla|Chrome|Safari|Edge|Firefox/i.test(userAgent)) {
      return { client: 'ESP32-CAM', isEsp: true };
    }
    const ip = req.socket.remoteAddress || '127.0.0.1';
    const cleanIp = ip.replace(/^::ffff:/, '');
    return { client: cleanIp === '::1' || cleanIp === '127.0.0.1' ? 'Local Browser' : cleanIp, isEsp: false };
  }

  function recordActivity(req, res, startTime, tracker) {
    const { client, isEsp } = identifyClient(req);
    const timestamp = new Date().toISOString();
    const duration = Date.now() - startTime;
    const bytes = tracker.bytes || Number(res.getHeader('Content-Length')) || 0;

    const entry = {
      timestamp,
      client,
      esp_identified: isEsp,
      method: req.method,
      path: tracker.path || req.url,
      url: req.url,
      status: res.statusCode,
      bytes,
      bytes_sent: bytes,
      duration_ms: duration,
      type: tracker.type || 'api',
      media_id: tracker.mediaId || null,
      file_type: tracker.fileType || null
    };

    activityLog.unshift(entry);
    if (activityLog.length > MAX_ACTIVITY_LOG) {
      activityLog.length = MAX_ACTIVITY_LOG;
    }

    activityStats.requestCount++;
    activityStats.lastRequestTime = timestamp;

    if (isEsp) {
      activityStats.lastEspActivityTime = timestamp;
    }

    if (tracker.type === 'catalogue') {
      activityStats.lastCatalogueFetch = timestamp;
    }

    if (tracker.type === 'media' || tracker.type === 'video' || tracker.type === 'audio') {
      activityStats.downloadsCount++;
      activityStats.lastMediaRequest = timestamp;
      activityStats.lastMedia = tracker.mediaId || null;
      if (tracker.file === 'video.bin' || tracker.type === 'video') activityStats.lastVideoRequest = timestamp;
      if (tracker.file === 'audio.bin' || tracker.type === 'audio') activityStats.lastAudioRequest = timestamp;
      if (res.statusCode === 200 || res.statusCode === 206) {
        activityStats.lastSuccessfulTransfer = timestamp;
      }
    }

    if (res.statusCode >= 400) {
      activityStats.lastError = {
        timestamp,
        status: res.statusCode,
        path: req.url,
        message: tracker.errorMessage || `HTTP ${res.statusCode}`
      };
    }
  }

  async function getInventory() {
    const source = readCatalogue();
    let videoFiles = 0;
    let audioFiles = 0;
    let storageBytes = 0;

    const items = await Promise.all(source.videos.map(async (item) => {
      const itemDir = path.join(mediaDir, item.id);
      const safeStat = async (filePath) => {
        try {
          const stat = await fsp.stat(filePath);
          return stat.isFile() ? stat : null;
        } catch {
          return null;
        }
      };
      const videoStat = await safeStat(path.join(itemDir, item.video_file || 'video.bin'));
      const audioStat = await safeStat(path.join(itemDir, item.audio_file || 'audio.bin'));

      if (videoStat) {
        videoFiles++;
        storageBytes += videoStat.size;
      }
      if (audioStat) {
        audioFiles++;
        storageBytes += audioStat.size;
      }

      const totalBytes = (videoStat?.size || 0) + (audioStat?.size || 0);
      const offlineLimit = config.offline_storage_limit || 2050000;
      const isOffline = Boolean(item.offline_download === true || item.metadata?.offline_download === true);
      const isSizeValid = totalBytes > 0 && (totalBytes + 4096) <= offlineLimit;

      return {
        ...formatPublicItem(item),
        offline_download: isOffline,
        description: item.description || item.metadata?.description || '',
        created_at: item.created_at || null,
        updated_at: item.updated_at || null,
        metadata: {
          ...(item.metadata || {}),
          spiffs_compatible: isOffline,
          offline_download: isOffline
        },
        offline: {
          compatible: isOffline,
          eligible: isOffline,
          size_fits: isSizeValid,
          size: totalBytes,
          limit: offlineLimit
        },
        availability: {
          video: Boolean(videoStat),
          audio: Boolean(audioStat)
        },
        sizes: {
          video: videoStat?.size || 0,
          audio: audioStat?.size || 0,
          total: totalBytes
        }
      };
    }));

    return {
      items,
      totals: {
        items: source.videos.length,
        video_files: videoFiles,
        audio_files: audioFiles,
        storage_bytes: storageBytes,
        storage_display: formatBytes(storageBytes)
      }
    };
  }

  async function getMediaDetail(id) {
    const source = readCatalogue();
    const item = source.videos.find(v => v.id === id);
    if (!item) return null;

    const inv = await getInventory();
    const viewItem = inv.items.find(v => v.id === id);
    if (!viewItem) return null;

    let stikDetails = { valid: false, reason: 'Video file missing.' };
    const videoPath = path.join(mediaDir, id, 'video.bin');
    try {
      const handle = await fsp.open(videoPath, 'r');
      try {
        const header = Buffer.alloc(16);
        const { bytesRead } = await handle.read(header, 0, 16, 0);
        if (bytesRead >= 16) {
          stikDetails = inspectStikHeader(header);
        } else {
          stikDetails = { valid: false, reason: 'File smaller than 16 bytes.' };
        }
      } finally {
        await handle.close();
      }
    } catch {
      stikDetails = { valid: false, reason: 'Video file is missing or unreadable.' };
    }

    return {
      ...viewItem,
      stik: stikDetails,
      audio: {
        format: 'Raw unsigned 8-bit PCM @ 5000 Hz (mono)',
        bytes: viewItem.sizes.audio,
        available: viewItem.availability.audio
      },
      api_item: formatPublicItem(item)
    };
  }

  async function readRequestBody(req, maxBytes) {
    const chunks = [];
    let received = 0;
    for await (const chunk of req) {
      received += chunk.length;
      if (received > maxBytes) {
        throw Object.assign(new Error(`Upload exceeds maximum allowed size of ${formatBytes(maxBytes)}.`), {
          status: 413,
          code: 'UPLOAD_TOO_LARGE'
        });
      }
      chunks.push(chunk);
    }
    return Buffer.concat(chunks);
  }

  async function saveMediaFile(id, type, fileObj) {
    if (!fileObj || !fileObj.data || !fileObj.data.length) {
      throw Object.assign(new Error(`${type}.bin file is required.`), { status: 400, code: 'MISSING_FILE' });
    }

    const itemDir = path.join(mediaDir, id);
    const destination = path.join(itemDir, `${type}.bin`);
    const tempFile = `${destination}.${process.pid}.${Date.now()}.new`;
    const backupFile = `${destination}.${process.pid}.${Date.now()}.backup`;

    await fsp.mkdir(itemDir, { recursive: true });
    await fsp.writeFile(tempFile, fileObj.data);

    try {
      // Validate video STIK header before accepting
      if (type === 'video') {
        const stik = inspectStikHeader(fileObj.data);
        if (!stik.valid) {
          throw Object.assign(new Error(`INVALID STIK FILE: ${stik.reason}`), { status: 422, code: 'INVALID_STIK' });
        }
      }

      let hadBackup = false;
      try {
        await fsp.rename(destination, backupFile);
        hadBackup = true;
      } catch (err) {
        if (err.code !== 'ENOENT') throw err;
      }

      try {
        await fsp.rename(tempFile, destination);
      } catch (err) {
        if (hadBackup) await fsp.rename(backupFile, destination);
        throw err;
      }

      if (hadBackup) {
        await fsp.rm(backupFile, { force: true });
      }
    } catch (err) {
      await fsp.rm(tempFile, { force: true });
      throw err;
    }
  }

  async function serveStaticFile(res, filePath) {
    try {
      const stat = await fsp.stat(filePath);
      if (!stat.isFile()) throw new Error();

      const ext = path.extname(filePath).toLowerCase();
      const mimeTypes = {
        '.html': 'text/html; charset=utf-8',
        '.css': 'text/css; charset=utf-8',
        '.js': 'text/javascript; charset=utf-8',
        '.json': 'application/json; charset=utf-8',
        '.svg': 'image/svg+xml',
        '.png': 'image/png',
        '.ico': 'image/x-icon',
        '.bin': 'application/octet-stream'
      };

      const contentType = mimeTypes[ext] || 'application/octet-stream';
      res.writeHead(200, {
        'Content-Type': contentType,
        'Content-Length': stat.size,
        'Cache-Control': 'no-cache'
      });
      fs.createReadStream(filePath).pipe(res);
    } catch {
      sendError(res, 404, 'NOT_FOUND', 'Requested file was not found.');
    }
  }

  async function requestHandler(req, res) {
    const startTime = Date.now();
    const tracker = {};

    applyCorsHeaders(res);
    res.on('finish', () => recordActivity(req, res, startTime, tracker));

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      return res.end();
    }

    const parsedUrl = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    const pathname = parsedUrl.pathname;

    try {
      // 1. GET /api/videos - ESP32 Catalogue Endpoint
      if (req.method === 'GET' && pathname === '/api/videos') {
        tracker.type = 'catalogue';
        const page = parseInteger(parsedUrl.searchParams.get('page'), 'page', 0, Number.MAX_SAFE_INTEGER);
        const limit = parseInteger(parsedUrl.searchParams.get('limit'), 'limit', config.pagination.defaultLimit, config.pagination.maxLimit);

        if (limit < 1) {
          return sendError(res, 400, 'INVALID_QUERY', 'limit must be at least 1.');
        }

        const inv = await getInventory();
        const slice = inv.items.slice(page * limit, page * limit + limit);

        return sendJson(res, 200, {
          page,
          limit,
          total: inv.items.length,
          items: slice
        });
      }

      // 2. GET /api/status - Live Server & ESP Activity Status
      if (req.method === 'GET' && pathname === '/api/status') {
        const inv = await getInventory();
        const nowMs = Date.now();
        const lastEspMs = activityStats.lastEspActivityTime ? new Date(activityStats.lastEspActivityTime).getTime() : 0;
        const isEspActive = (nowMs - lastEspMs) < 15000; // active if request seen within last 15 seconds

        return sendJson(res, 200, {
          server: {
            online: true,
            port: config.port,
            uptime_ms: nowMs - startedAt,
            media_root: 'WEB/media',
            catalogue: 'WEB/data/catalogue.json',
            lan_ips: getLocalIpAddresses()
          },
          esp: {
            active: isEspActive,
            status_text: isEspActive ? 'ACTIVE' : 'IDLE',
            last_activity: activityStats.lastEspActivityTime
          },
          totals: inv.totals,
          activity: activityStats,
          recent_activity: activityLog.slice(0, 15)
        });
      }

      // 3. GET /api/activity - Detailed Request Log
      if (req.method === 'GET' && (pathname === '/api/activity' || pathname === '/api/activity/recent')) {
        return sendJson(res, 200, {
          max_entries: MAX_ACTIVITY_LOG,
          total: activityLog.length,
          requests: activityLog
        });
      }

      // 4. GET /api/videos/:id - Single Media Item Detail
      const idMatch = pathname.match(/^\/api\/videos\/([^/]+)$/);
      if (req.method === 'GET' && idMatch) {
        const id = decodeURIComponent(idMatch[1]);
        const itemDetail = await getMediaDetail(id);
        if (!itemDetail) {
          return sendError(res, 404, 'VIDEO_NOT_FOUND', `Video '${id}' does not exist.`);
        }
        return sendJson(res, 200, itemDetail);
      }

      // 5. POST /api/videos - Upload / Add New Media Item
      if (req.method === 'POST' && pathname === '/api/videos') {
        tracker.type = 'management';
        const bodyBuf = await readRequestBody(req, config.upload.maxBytes);
        const parsed = parseMultipartFormData(bodyBuf, req.headers['content-type']);
        const { id, name, description = '', metadata = '{}', offline_download } = parsed.fields;

        if (typeof id !== 'string' || !ID_REGEX.test(id) || id.length > 64) {
          return sendError(res, 400, 'INVALID_ID', 'ID must contain only alphanumeric characters, underscores, or hyphens (max 64 chars).');
        }
        if (!name || !name.trim()) {
          return sendError(res, 400, 'INVALID_NAME', 'Name is required.');
        }

        let parsedMeta = {};
        try {
          parsedMeta = metadata ? JSON.parse(metadata) : {};
        } catch {
          return sendError(res, 400, 'INVALID_METADATA', 'Metadata must be valid JSON.');
        }

        const isOfflineRequested = offline_download === 'true' || offline_download === true || offline_download === '1' || offline_download === 'on';
        parsedMeta.offline_download = isOfflineRequested;

        const source = readCatalogue();
        if (source.videos.some(v => v.id === id)) {
          return sendError(res, 409, 'DUPLICATE_ID', `Media with ID '${id}' already exists.`);
        }

        // Save video.bin and audio.bin
        await saveMediaFile(id, 'video', parsed.files.video);
        try {
          await saveMediaFile(id, 'audio', parsed.files.audio);
        } catch (err) {
          await fsp.rm(path.join(mediaDir, id), { recursive: true, force: true });
          throw err;
        }

        const timestamp = new Date().toISOString();
        source.videos.push({
          id,
          name: name.trim(),
          video_file: 'video.bin',
          audio_file: 'audio.bin',
          description: description.trim(),
          offline_download: isOfflineRequested,
          metadata: parsedMeta,
          created_at: timestamp,
          updated_at: timestamp
        });

        await saveCatalogue(source);
        const newDetail = await getMediaDetail(id);
        return sendJson(res, 201, newDetail);
      }

      // 6. PUT /api/videos/:id - Update Media Metadata
      if (req.method === 'PUT' && idMatch) {
        tracker.type = 'management';
        const id = decodeURIComponent(idMatch[1]);
        const source = readCatalogue();
        const item = source.videos.find(v => v.id === id);
        if (!item) {
          return sendError(res, 404, 'VIDEO_NOT_FOUND', `Video '${id}' does not exist.`);
        }

        const bodyBuf = await readRequestBody(req, 1048576);
        const parsed = JSON.parse(bodyBuf.toString('utf8'));

        if (!parsed.name || !parsed.name.trim()) {
          return sendError(res, 400, 'INVALID_NAME', 'Name is required.');
        }

        item.name = parsed.name.trim();
        item.description = String(parsed.description || '').trim();
        item.metadata = parsed.metadata && typeof parsed.metadata === 'object' ? parsed.metadata : {};
        if (typeof parsed.offline_download !== 'undefined') {
          item.offline_download = Boolean(parsed.offline_download);
          item.metadata.offline_download = Boolean(parsed.offline_download);
        }
        item.updated_at = new Date().toISOString();

        await saveCatalogue(source);
        return sendJson(res, 200, await getMediaDetail(id));
      }

      // 7. POST /api/videos/:id/video or /api/videos/:id/audio - Replace Media File
      const replaceMatch = pathname.match(/^\/api\/videos\/([^/]+)\/(video|audio)$/);
      if (req.method === 'POST' && replaceMatch) {
        tracker.type = 'management';
        const id = decodeURIComponent(replaceMatch[1]);
        const fileType = replaceMatch[2];
        const source = readCatalogue();
        const item = source.videos.find(v => v.id === id);

        if (!item) {
          return sendError(res, 404, 'VIDEO_NOT_FOUND', `Video '${id}' does not exist.`);
        }

        const bodyBuf = await readRequestBody(req, config.upload.maxBytes);
        const parsed = parseMultipartFormData(bodyBuf, req.headers['content-type']);

        await saveMediaFile(id, fileType, parsed.files[fileType]);
        item.updated_at = new Date().toISOString();
        await saveCatalogue(source);

        return sendJson(res, 200, await getMediaDetail(id));
      }

      // 8. DELETE /api/videos/:id - Delete Media
      if (req.method === 'DELETE' && idMatch) {
        tracker.type = 'management';
        const id = decodeURIComponent(idMatch[1]);
        const source = readCatalogue();
        const index = source.videos.findIndex(v => v.id === id);

        if (index < 0) {
          return sendError(res, 404, 'VIDEO_NOT_FOUND', `Video '${id}' does not exist.`);
        }

        source.videos.splice(index, 1);
        await saveCatalogue(source);
        await fsp.rm(path.join(mediaDir, id), { recursive: true, force: true });

        return sendJson(res, 200, { deleted: id, success: true });
      }

      // 9. GET /media/:id/:file - Binary Media Serving for ESP32
      const mediaMatch = pathname.match(/^\/media\/([^/]+)\/(video\.bin|audio\.bin)$/);
      if (req.method === 'GET' && mediaMatch) {
        const id = decodeURIComponent(mediaMatch[1]);
        const filename = mediaMatch[2];

        tracker.type = 'media';
        tracker.mediaId = id;
        tracker.file = filename;

        if (!ID_REGEX.test(id) || !readCatalogue().videos.some(v => v.id === id)) {
          return sendError(res, 404, 'VIDEO_NOT_FOUND', `Video '${id}' does not exist in catalogue.`);
        }

        const filePath = path.join(mediaDir, id, filename);
        try {
          const stat = await fsp.stat(filePath);
          if (!stat.isFile()) throw new Error();

          tracker.bytes = stat.size;
          res.writeHead(200, {
            'Content-Type': 'application/octet-stream',
            'Content-Length': stat.size,
            'Content-Disposition': `attachment; filename="${filename}"`,
            'Cache-Control': 'public, max-age=3600'
          });
          return fs.createReadStream(filePath).pipe(res);
        } catch {
          return sendError(res, 404, 'MEDIA_NOT_FOUND', `File '${filename}' for video '${id}' is absent.`);
        }
      }

      // 10. Web UI Static Files
      if (req.method !== 'GET') {
        return sendError(res, 405, 'METHOD_NOT_ALLOWED', 'Only GET and OPTIONS are supported for this path.');
      }

      const relativeFile = pathname === '/' ? 'index.html' : pathname.slice(1);
      const safeStaticPath = path.resolve(publicDir, relativeFile);

      // Path traversal security check
      if (!safeStaticPath.startsWith(`${path.resolve(publicDir)}${path.sep}`)) {
        return sendError(res, 403, 'FORBIDDEN', 'Access denied.');
      }

      return serveStaticFile(res, safeStaticPath);
    } catch (err) {
      tracker.errorMessage = err.message;
      return sendError(res, err.status || 400, err.code || 'BAD_REQUEST', err.message || 'Unable to process request.');
    }
  }

  return http.createServer(requestHandler);
}

if (require.main === module) {
  const config = loadConfig(__dirname);
  const server = createServer();
  server.listen(config.port, config.host, () => {
    const ips = getLocalIpAddresses();
    console.log('====================================================');
    console.log(`  ESP32 Media Control Server`);
    console.log(`  Local URL:   http://localhost:${config.port}`);
    if (ips.length > 0) {
      console.log(`  LAN Base URL: http://${ips[0]}:${config.port} (Use this on ESP32-CAM)`);
    }
    console.log('====================================================');
  });
}

module.exports = { createServer };
