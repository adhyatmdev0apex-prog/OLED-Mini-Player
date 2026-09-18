const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { createServer } = require('../server');

let root, server, base;
const stik = () => { const b = Buffer.alloc(16); b.write('STIK'); b.writeUInt16LE(128, 4); b.writeUInt16LE(64, 6); b.writeUInt8(29, 8); b.writeUInt32LE(12, 9); return b; };
test.before(async () => { root = await fs.mkdtemp(path.join(os.tmpdir(), 'esp-media-test-')); await fs.mkdir(path.join(root, 'data'), { recursive: true }); await fs.mkdir(path.join(root, 'media'), { recursive: true }); await fs.mkdir(path.join(root, 'public'), { recursive: true }); await fs.writeFile(path.join(root, 'config.json'), JSON.stringify({ port: 0, pagination: { defaultLimit: 10, maxLimit: 50 }, upload: { maxBytes: 1024 * 1024 } })); await fs.writeFile(path.join(root, 'data', 'catalogue.json'), JSON.stringify({ videos: [{ id: 'v0001', name: 'Example', video_file: 'video.bin', audio_file: 'audio.bin', metadata: {} }] })); await fs.writeFile(path.join(root, 'public', 'index.html'), '<!doctype html><title>Control panel</title>'); await fs.writeFile(path.join(root, 'public', 'app.js'), 'console.log("app")'); server = createServer({ root }); await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); base = `http://127.0.0.1:${server.address().port}`; });
test.after(async () => { await new Promise(resolve => server.close(resolve)); await fs.rm(root, { recursive: true, force: true }); });
test('root renders HTML and compatible catalogue returns relative media URLs', async () => { const home = await fetch(base); assert.equal(home.status, 200); assert.match(home.headers.get('content-type'), /^text\/html/); const r = await fetch(`${base}/api/videos?page=0&limit=10`); const b = await r.json(); assert.equal(r.status, 200); assert.equal(b.page, 0); assert.equal(b.total, 1); assert.equal(b.items[0].video_url, '/media/v0001/video.bin'); assert.equal(b.items[0].audio_url, '/media/v0001/audio.bin'); });
test('pagination, absent media, and invalid parameters have JSON errors', async () => { let r = await fetch(`${base}/api/videos?page=1&limit=1`); assert.deepEqual((await r.json()).items, []); r = await fetch(`${base}/api/videos?page=no&limit=1`); assert.equal(r.status, 400); r = await fetch(`${base}/media/v0001/video.bin`); assert.equal(r.status, 404); assert.equal((await r.json()).error.code, 'MEDIA_NOT_FOUND'); });
test('upload persists a valid STIK/video pair and records ESP-labelled media activity', async () => { const form = new FormData(); form.set('id', 'v0002'); form.set('name', 'Uploaded animation'); form.set('description', 'Created by test'); form.set('metadata', '{"tag":"test"}'); form.set('video', new Blob([stik()]), 'anything.bin'); form.set('audio', new Blob([Buffer.from([1, 2, 3, 4])]), 'sound.any'); let r = await fetch(`${base}/api/videos`, { method: 'POST', body: form }); assert.equal(r.status, 201); const item = await r.json(); assert.equal(item.stik.valid, true); assert.equal(item.stik.width, 128); const stored = await fs.readFile(path.join(root, 'media', 'v0002', 'video.bin')); assert.deepEqual(stored, stik()); r = await fetch(`${base}/media/v0002/video.bin`, { headers: { 'X-ESP-CLIENT': 'ESP32-CAM' } }); assert.equal(r.status, 200); assert.equal(r.headers.get('content-type'), 'application/octet-stream'); assert.deepEqual(Buffer.from(await r.arrayBuffer()), stik()); const status = await (await fetch(`${base}/api/status`)).json(); assert.equal(status.activity.lastMedia, 'v0002'); assert.equal(status.recent_activity[0].esp_identified, true); });
test('validated replacement swaps only the selected binary', async () => { const newer = stik(); newer.writeUInt32LE(99, 9); const form = new FormData(); form.set('video', new Blob([newer]), 'replacement.bin'); const r = await fetch(`${base}/api/videos/v0002/video`, { method: 'POST', body: form }); assert.equal(r.status, 200); const disk = await fs.readFile(path.join(root, 'media', 'v0002', 'video.bin')); assert.deepEqual(disk, newer); const audio = await fs.readFile(path.join(root, 'media', 'v0002', 'audio.bin')); assert.deepEqual(audio, Buffer.from([1, 2, 3, 4])); });
test('invalid STIK is rejected without a catalogue entry; delete removes valid media', async () => { const bad = new FormData(); bad.set('id', 'bad'); bad.set('name', 'Broken'); bad.set('video', new Blob([Buffer.from('not-stik')]), 'video.bin'); bad.set('audio', new Blob([Buffer.from([1])]), 'audio.bin'); let r = await fetch(`${base}/api/videos`, { method: 'POST', body: bad }); assert.equal(r.status, 422); assert.equal((await r.json()).error.code, 'INVALID_STIK'); r = await fetch(`${base}/api/videos/v0002`, { method: 'DELETE' }); assert.equal(r.status, 200); await assert.rejects(fs.stat(path.join(root, 'media', 'v0002'))); const catalogue = JSON.parse(await fs.readFile(path.join(root, 'data', 'catalogue.json'), 'utf8')); assert.equal(catalogue.videos.some(v => v.id === 'v0002'), false); });

test('metadata edit PUT and activity endpoint work correctly', async () => {
  const form = new FormData();
  form.set('id', 'v0003');
  form.set('name', 'Edit test');
  form.set('video', new Blob([stik()]), 'video.bin');
  form.set('audio', new Blob([Buffer.from([10, 20])]), 'audio.bin');
  await fetch(`${base}/api/videos`, { method: 'POST', body: form });

  const putRes = await fetch(`${base}/api/videos/v0003`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'Updated Title', description: 'New description', metadata: { tag: 'cool' } })
  });
  assert.equal(putRes.status, 200);
  const updated = await putRes.json();
  assert.equal(updated.name, 'Updated Title');
  assert.equal(updated.description, 'New description');

  const actRes = await fetch(`${base}/api/activity`);
  assert.equal(actRes.status, 200);
  const actData = await actRes.json();
  assert.ok(Array.isArray(actData.requests));
  assert.ok(actData.total > 0);

  // Clean up
  await fetch(`${base}/api/videos/v0003`, { method: 'DELETE' });
});
