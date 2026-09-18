// ============================================================
// ESP32-CAM MEDIA CONTROL PANEL - CLIENT APPLICATION
// ============================================================

(function () {
  'use strict';

  // Application State
  const state = {
    status: null,
    items: [],
    selectedItem: null,
    currentView: 'dashboard',
    activityFilter: 'all',
    lanIps: []
  };

  // Helper DOM selectors
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => Array.from(document.querySelectorAll(selector));

  // Utility formatters
  function formatBytes(bytes) {
    if (bytes === 0 || !bytes) return '0 B';
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / 1048576).toFixed(2)} MB`;
  }

  function formatTime(isoString) {
    if (!isoString) return '—';
    try {
      const date = new Date(isoString);
      return date.toLocaleTimeString([], { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
    } catch {
      return '—';
    }
  }

  function formatDuration(ms) {
    if (ms === null || ms === undefined) return '—';
    const totalSecs = Math.floor(ms / 1000);
    const hrs = Math.floor(totalSecs / 3600);
    const mins = Math.floor((totalSecs % 3600) / 60);
    const secs = totalSecs % 60;
    if (hrs > 0) return `${hrs}h ${mins}m ${secs}s`;
    if (mins > 0) return `${mins}m ${secs}s`;
    return `${secs}s`;
  }

  function escapeHtml(str) {
    if (str === null || str === undefined) return '';
    const div = document.createElement('div');
    div.textContent = String(str);
    return div.innerHTML;
  }

  // Floating Toast Notification
  function showToast(message, isError = false) {
    const toast = $('#toast');
    toast.textContent = message;
    toast.className = isError ? 'show error' : 'show';
    setTimeout(() => {
      toast.className = '';
    }, 4000);
  }

  // Generic JSON API fetch wrapper
  async function fetchApi(url, options = {}) {
    const res = await fetch(url, options);
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      const errorMsg = data.error?.message || `HTTP Error ${res.status}: ${res.statusText}`;
      throw new Error(errorMsg);
    }
    return { data, status: res.status, headers: res.headers };
  }

  // View Navigation
  function switchView(viewName) {
    state.currentView = viewName;

    // Toggle views
    $$('.view').forEach((view) => {
      view.classList.toggle('active', view.id === viewName);
    });

    // Toggle nav buttons
    $$('.nav-btn').forEach((btn) => {
      btn.classList.toggle('active', btn.dataset.view === viewName);
    });

    const titles = {
      dashboard: ['SYSTEM OVERVIEW', 'Dashboard'],
      library: ['CATALOGUE', 'Media Library'],
      add: ['UPLOAD', 'Add Media'],
      activity: ['SERVER LOG', 'ESP Activity Log'],
      api: ['DEVELOPER TOOLS', 'API Test Panel'],
      settings: ['CONFIGURATION', 'Server & Network Info'],
      details: ['MEDIA DETAILS', 'Media Details']
    };

    const titleInfo = titles[viewName] || ['SYSTEM', 'Media Control'];
    $('#viewLabel').textContent = titleInfo[0];
    $('#viewTitle').textContent = titleInfo[1];

    if (viewName === 'activity') {
      renderActivityTable();
    } else if (viewName === 'api') {
      populateApiTestDropdown();
    }
  }

  // Render Dashboard
  function renderDashboard() {
    const s = state.status;
    if (!s) return;

    const totals = s.totals || {};
    const act = s.activity || {};
    const esp = s.esp || {};

    // Uptime & Transfers
    $('#serverUptime').textContent = formatDuration(s.server?.uptime_ms);
    $('#totalDownloadsCount').textContent = act.downloadsCount || 0;

    // ESP Live Status
    const isEspActive = esp.active;
    const pulseDot = $('#espPulseDot');
    const statusTitle = $('#espStatusTitle');
    const lastActionText = $('#espLastActionText');

    if (isEspActive) {
      pulseDot.className = 'live-dot';
      statusTitle.textContent = 'ESP32 ACTIVE';
      statusTitle.style.color = 'var(--accent)';
      lastActionText.textContent = `Active transfer / request detected at ${formatTime(esp.last_activity)}`;
    } else {
      pulseDot.className = 'live-dot idle';
      statusTitle.textContent = 'ESP32 IDLE';
      statusTitle.style.color = 'var(--text-muted)';
      if (act.lastEspActivityTime) {
        lastActionText.textContent = `Last ESP request: ${formatTime(act.lastEspActivityTime)} (${act.lastMedia ? 'media ' + act.lastMedia : 'catalogue'})`;
      } else {
        lastActionText.textContent = 'Waiting for ESP32-CAM requests...';
      }
    }

    // 6 Metrics
    $('#metricItems').textContent = totals.items || 0;
    $('#metricStorage').textContent = totals.storage_display || formatBytes(totals.storage_bytes || 0);
    $('#metricVideoFiles').textContent = totals.video_files || 0;
    $('#metricAudioFiles').textContent = totals.audio_files || 0;
    $('#metricRequests').textContent = act.requestCount || 0;
    $('#metricLastMedia').textContent = act.lastMedia || '—';
    $('#metricLastMediaSub').textContent = act.lastMediaRequest ? `At ${formatTime(act.lastMediaRequest)}` : 'Not requested yet';

    // Telemetry list
    $('#telLastCatalogue').textContent = act.lastCatalogueFetch ? formatTime(act.lastCatalogueFetch) : 'None';
    $('#telLastMedia').textContent = act.lastMediaRequest ? `${formatTime(act.lastMediaRequest)} (${act.lastMedia || 'item'})` : 'None';
    $('#telLastVideo').textContent = act.lastVideoRequest ? formatTime(act.lastVideoRequest) : 'None';
    $('#telLastAudio').textContent = act.lastAudioRequest ? formatTime(act.lastAudioRequest) : 'None';
    $('#telLastSuccess').textContent = act.lastSuccessfulTransfer ? formatTime(act.lastSuccessfulTransfer) : 'None';

    if (act.lastError) {
      $('#telLastError').textContent = `${formatTime(act.lastError.timestamp)} (HTTP ${act.lastError.status})`;
      $('#telLastError').className = 'status-tag s500';
    } else {
      $('#telLastError').textContent = 'None';
      $('#telLastError').className = 'ok-text';
    }

    // Recent Activity Mini Table
    const recent = s.recent_activity || [];
    if (recent.length === 0) {
      $('#recentActivityList').innerHTML = '<p class="empty-cell">No requests recorded yet. Calls to the server will appear here.</p>';
    } else {
      $('#recentActivityList').innerHTML = `
        <table class="data-table">
          <thead>
            <tr>
              <th>Time</th>
              <th>Client</th>
              <th>Method</th>
              <th>Path</th>
              <th>Status</th>
              <th>Size</th>
            </tr>
          </thead>
          <tbody>
            ${recent.slice(0, 6).map((r) => `
              <tr>
                <td>${formatTime(r.timestamp)}</td>
                <td><span class="client-badge ${r.esp_identified ? 'esp' : ''}">${escapeHtml(r.client)}</span></td>
                <td><span class="method-tag">${escapeHtml(r.method)}</span></td>
                <td><code>${escapeHtml(r.path)}</code></td>
                <td><span class="status-tag s${r.status >= 500 ? '500' : r.status >= 400 ? '400' : '200'}">${r.status}</span></td>
                <td>${formatBytes(r.bytes)}</td>
              </tr>
            `).join('')}
          </tbody>
        </table>
      `;
    }

    // Side and Settings LAN IPs
    state.lanIps = s.server?.lan_ips || [];
    if (state.lanIps.length > 0) {
      $('#sideLanIp').textContent = `LAN: ${state.lanIps[0]}`;
      $('#espServerCmd').textContent = `server http://${state.lanIps[0]}:${s.server?.port || 3000}`;

      $('#lanIpList').innerHTML = state.lanIps.map((ip) => `
        <div class="lan-item">
          <code>http://${ip}:${s.server?.port || 3000}</code>
          <button class="ghost-btn copy-ip-btn" data-url="http://${ip}:${s.server?.port || 3000}">Copy Base URL</button>
        </div>
      `).join('');

      $$('.copy-ip-btn').forEach((btn) => {
        btn.onclick = () => {
          navigator.clipboard.writeText(btn.dataset.url);
          showToast(`Copied to clipboard: ${btn.dataset.url}`);
        };
      });
    }
  }

  // Render Media Library Cards Grid
  function renderLibrary() {
    const grid = $('#libraryGrid');
    if (state.items.length === 0) {
      grid.innerHTML = '<p class="empty-cell" style="grid-column: 1/-1;">No media catalogue entries. Click "＋ Add Media" to upload an animation.</p>';
      return;
    }

    grid.innerHTML = state.items.map((item) => {
      const hasVideo = item.availability?.video;
      const hasAudio = item.availability?.audio;
      const totalSize = (item.sizes?.video || 0) + (item.sizes?.audio || 0);

      return `
        <article class="media-card">
          <div class="card-top">
            <span class="card-id-badge">${escapeHtml(item.id)}</span>
            <h3>${escapeHtml(item.name)}</h3>
            <p class="card-desc">${escapeHtml(item.description || item.metadata?.description || 'No description provided.')}</p>
          </div>

          <div class="file-status-rows">
            <div class="status-row">
              <span>🎬 Video Binary:</span>
              <span class="status-badge ${hasVideo ? 'ok' : 'missing'}">
                ${hasVideo ? `${formatBytes(item.sizes?.video)} ✓` : 'Missing'}
              </span>
            </div>
            <div class="status-row">
              <span>🔊 Audio Binary:</span>
              <span class="status-badge ${hasAudio ? 'ok' : 'missing'}">
                ${hasAudio ? `${formatBytes(item.sizes?.audio)} ✓` : 'Missing'}
              </span>
            </div>
            <div class="status-row">
              <span>💾 Storage Tier:</span>
              <span class="status-badge ${item.offline?.compatible ? 'ok' : ''}">
                ${item.offline?.compatible ? 'SPIFFS Offline ✓' : 'Stream Only'}
              </span>
            </div>
          </div>

          <div class="card-footer">
            <small>Total: <strong>${formatBytes(totalSize)}</strong></small>
            <div class="card-actions">
              <button class="ghost-btn detail-btn" data-id="${escapeHtml(item.id)}">Details</button>
              <button class="ghost-btn edit-btn" data-id="${escapeHtml(item.id)}">Edit</button>
              <button class="danger-btn delete-btn" data-id="${escapeHtml(item.id)}">Delete</button>
            </div>
          </div>
        </article>
      `;
    }).join('');

    // Attach card action handlers
    $$('.detail-btn').forEach((b) => b.onclick = () => loadMediaDetail(b.dataset.id));
    $$('.edit-btn').forEach((b) => b.onclick = () => openEditModal(b.dataset.id));
    $$('.delete-btn').forEach((b) => b.onclick = () => openDeleteModal(b.dataset.id));
  }

  // Render Full Activity Table with filters
  function renderActivityTable() {
    const tbody = $('#activityTableBody');
    const logs = state.status?.recent_activity || [];
    const filter = state.activityFilter;

    const filtered = logs.filter((log) => {
      if (filter === 'esp') return log.esp_identified;
      if (filter === 'media') return log.type === 'media' || log.path.startsWith('/media/');
      if (filter === 'api') return log.path.startsWith('/api/');
      return true;
    });

    if (filtered.length === 0) {
      tbody.innerHTML = '<tr><td colspan="8" class="empty-cell">No matching activity logged yet.</td></tr>';
      return;
    }

    tbody.innerHTML = filtered.map((r) => `
      <tr>
        <td>${formatTime(r.timestamp)}</td>
        <td><span class="client-badge ${r.esp_identified ? 'esp' : ''}">${escapeHtml(r.client)}</span></td>
        <td><span class="method-tag">${escapeHtml(r.method)}</span></td>
        <td><code>${escapeHtml(r.path)}</code></td>
        <td><span class="status-tag s${r.status >= 500 ? '500' : r.status >= 400 ? '400' : '200'}">${r.status}</span></td>
        <td>${formatBytes(r.bytes)}</td>
        <td>${r.media_id ? `<code>${escapeHtml(r.media_id)}</code>` : '—'}</td>
        <td>${r.duration_ms} ms</td>
      </tr>
    `).join('');
  }

  // Load and Render Media Details
  async function loadMediaDetail(id) {
    try {
      const { data: item } = await fetchApi(`/api/videos/${encodeURIComponent(id)}`);
      state.selectedItem = item;

      const container = $('#detailContainer');
      const stik = item.stik || {};
      const audio = item.audio || {};

      container.innerHTML = `
        <div class="detail-header">
          <div>
            <span class="card-id-badge">${escapeHtml(item.id)}</span>
            <h2>${escapeHtml(item.name)}</h2>
            <p class="muted-desc">${escapeHtml(item.description || 'No description provided.')}</p>
          </div>
          <div class="detail-actions">
            <button id="detailEditBtn" class="primary-btn">Edit Metadata</button>
            <button id="detailDeleteBtn" class="danger-btn">Delete</button>
          </div>
        </div>

        <div class="info-cards-grid">
          <!-- STIK Video Info -->
          <div class="panel">
            <p class="eyebrow">VIDEO.BIN · STIK VALIDATION</p>
            <h3>${item.availability.video ? formatBytes(item.sizes.video) : 'Missing'}</h3>
            <p class="sub-lead">
              ${stik.valid
                ? `<span class="ok-text">✓ Valid STIK Binary</span>: ${stik.width}×${stik.height} resolution, ${stik.fps} FPS, ${stik.frameCount} total frames`
                : `<span class="status-tag s500">✗ INVALID STIK FILE</span>: ${escapeHtml(stik.reason || 'Binary not provided')}`
              }
            </p>
            <div style="margin-top: 16px;">
              <button class="ghost-btn replace-file-btn" data-type="video">Replace video.bin</button>
            </div>
          </div>

          <!-- PCM Audio Info -->
          <div class="panel">
            <p class="eyebrow">AUDIO.BIN · PCM FORMAT</p>
            <h3>${item.availability.audio ? formatBytes(item.sizes.audio) : 'Missing'}</h3>
            <p class="sub-lead">
              ${item.availability.audio
                ? `<span class="ok-text">✓ Raw PCM Binary</span>: ${escapeHtml(audio.format || 'Unsigned 8-bit mono @ 5000Hz')}`
                : `<span class="status-tag s400">Missing audio binary</span>`
              }
            </p>
            <div style="margin-top: 16px;">
              <button class="ghost-btn replace-file-btn" data-type="audio">Replace audio.bin</button>
            </div>
          </div>
        </div>

        <!-- Exact URLs for ESP32 -->
        <div class="panel" style="margin-bottom: 24px;">
          <p class="eyebrow">ESP32 STREAMING ENDPOINTS</p>
          <h3>Exact Media URLs</h3>
          <div class="telemetry-list" style="margin-top: 14px;">
            <div class="telemetry-item">
              <span>Video URL (STIK):</span>
              <code>${escapeHtml(item.video_url)}</code>
              <a href="${escapeHtml(item.video_url)}" target="_blank" class="link-btn">Download</a>
            </div>
            <div class="telemetry-item">
              <span>Audio URL (PCM):</span>
              <code>${escapeHtml(item.audio_url)}</code>
              <a href="${escapeHtml(item.audio_url)}" target="_blank" class="link-btn">Download</a>
            </div>
          </div>
        </div>

        <!-- ESP32 API Representation Preview -->
        <div class="panel">
          <p class="eyebrow">API CONTRACT REPRESENTATION</p>
          <h3>JSON payload returned to ESP32-CAM via /api/videos</h3>
          <pre class="code-box" style="margin-top: 14px;">${escapeHtml(JSON.stringify(item.api_item || {}, null, 2))}</pre>
        </div>
      `;

      // Attach button actions in Detail view
      $('#detailEditBtn').onclick = () => openEditModal(item.id);
      $('#detailDeleteBtn').onclick = () => openDeleteModal(item.id);
      $$('.replace-file-btn').forEach((btn) => {
        btn.onclick = () => triggerFileReplacement(item.id, btn.dataset.type);
      });

      switchView('details');
    } catch (err) {
      showToast(err.message, true);
    }
  }

  // File replacement logic
  function triggerFileReplacement(id, fileType) {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.bin,application/octet-stream';

    input.onchange = async () => {
      if (!input.files || !input.files[0]) return;
      const file = input.files[0];
      const formData = new FormData();
      formData.append(fileType, file);

      showToast(`Uploading replacement ${fileType}.bin...`);
      try {
        await fetchApi(`/api/videos/${encodeURIComponent(id)}/${fileType}`, {
          method: 'POST',
          body: formData
        });
        showToast(`${fileType}.bin replaced & validated successfully!`);
        await refreshData();
        loadMediaDetail(id);
      } catch (err) {
        showToast(err.message, true);
      }
    };

    input.click();
  }

  // Edit metadata modal
  function openEditModal(id) {
    const item = state.items.find((i) => i.id === id) || state.selectedItem;
    if (!item) return;

    $('#editNameInput').value = item.name || '';
    $('#editDescInput').value = item.description || item.metadata?.description || '';
    $('#editMetaInput').value = JSON.stringify(item.metadata || {}, null, 2);

    const modal = $('#editModal');
    modal.showModal();

    $('#cancelEditBtn').onclick = () => modal.close();

    $('#editMetaForm').onsubmit = async (e) => {
      e.preventDefault();
      let metaObj = {};
      try {
        metaObj = JSON.parse($('#editMetaInput').value || '{}');
      } catch {
        showToast('Metadata must be valid JSON!', true);
        return;
      }

      try {
        await fetchApi(`/api/videos/${encodeURIComponent(id)}`, {
          method: 'PUT',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            name: $('#editNameInput').value,
            description: $('#editDescInput').value,
            metadata: metaObj
          })
        });

        modal.close();
        showToast('Metadata updated successfully!');
        await refreshData();
        if (state.currentView === 'details') {
          loadMediaDetail(id);
        }
      } catch (err) {
        showToast(err.message, true);
      }
    };
  }

  // Delete modal
  function openDeleteModal(id) {
    const modal = $('#confirmModal');
    $('#confirmModalDesc').textContent = `This will permanently delete media slot '${id}', remove its video.bin and audio.bin, and update catalogue.json.`;
    modal.showModal();

    $('#executeDeleteBtn').onclick = async (e) => {
      e.preventDefault();
      try {
        await fetchApi(`/api/videos/${encodeURIComponent(id)}`, { method: 'DELETE' });
        modal.close();
        showToast(`Media slot '${id}' deleted.`);
        await refreshData();
        switchView('library');
      } catch (err) {
        modal.close();
        showToast(err.message, true);
      }
    };
  }

  // Add Media Form Submission
  $('#addMediaForm').onsubmit = async (e) => {
    e.preventDefault();
    const form = e.target;
    const formData = new FormData(form);

    // Validate metadata JSON
    const metaStr = formData.get('metadata') || '{}';
    try {
      JSON.parse(metaStr);
    } catch {
      showToast('Metadata must be valid JSON!', true);
      return;
    }

    const progressWrap = $('#uploadProgressWrap');
    const submitBtn = $('#submitAddBtn');

    progressWrap.style.display = 'block';
    submitBtn.disabled = true;

    try {
      await fetchApi('/api/videos', {
        method: 'POST',
        body: formData
      });

      showToast('Media added & validated! Available to ESP32-CAM.');
      form.reset();
      progressWrap.style.display = 'none';
      submitBtn.disabled = false;
      await refreshData();
      switchView('library');
    } catch (err) {
      progressWrap.style.display = 'none';
      submitBtn.disabled = false;
      showToast(err.message, true);
    }
  };

  // Populate API test dropdown with existing items
  function populateApiTestDropdown() {
    const select = $('#testMediaSelect');
    select.innerHTML = '<option value="">Select a media slot...</option>' +
      state.items.map((i) => `<option value="${escapeHtml(i.id)}">${escapeHtml(i.id)} — ${escapeHtml(i.name)}</option>`).join('');
  }

  // Run Catalogue API Test
  $('#runCatalogueTestBtn').onclick = async () => {
    const page = $('#testApiPage').value || 0;
    const limit = $('#testApiLimit').value || 10;
    const metaBox = $('#catalogueResultMeta');
    const jsonBox = $('#apiJsonViewer');

    metaBox.textContent = 'Sending GET /api/videos...';
    const startTime = performance.now();

    try {
      const { data, status } = await fetchApi(`/api/videos?page=${page}&limit=${limit}`);
      const duration = Math.round(performance.now() - startTime);

      metaBox.innerHTML = `<span class="status-tag s200">HTTP ${status} OK</span> · ${duration} ms latency · Received ${data.items?.length || 0} items`;
      jsonBox.textContent = JSON.stringify(data, null, 2);
      refreshData();
    } catch (err) {
      const duration = Math.round(performance.now() - startTime);
      metaBox.innerHTML = `<span class="status-tag s500">FAILED</span> · ${duration} ms · ${escapeHtml(err.message)}`;
      jsonBox.textContent = JSON.stringify({ error: err.message }, null, 2);
    }
  };

  // Run Media Download Test
  async function testMediaFile(type) {
    const select = $('#testMediaSelect');
    const id = select.value;
    const metaBox = $('#mediaResultMeta');
    const jsonBox = $('#apiJsonViewer');

    if (!id) {
      showToast('Please select a media slot first.', true);
      return;
    }

    const url = `/media/${encodeURIComponent(id)}/${type}.bin`;
    metaBox.textContent = `Testing GET ${url}...`;
    const startTime = performance.now();

    try {
      const res = await fetch(url);
      const duration = Math.round(performance.now() - startTime);
      const contentLength = res.headers.get('content-length');
      const contentType = res.headers.get('content-type');

      if (!res.ok) {
        const errorJson = await res.json().catch(() => ({}));
        metaBox.innerHTML = `<span class="status-tag s${res.status}">HTTP ${res.status}</span> · ${duration} ms · File Absent`;
        jsonBox.textContent = JSON.stringify(errorJson, null, 2);
        return;
      }

      metaBox.innerHTML = `<span class="status-tag s200">HTTP 200 OK</span> · ${duration} ms · Content-Length: ${formatBytes(Number(contentLength))} · Type: ${contentType}`;
      jsonBox.textContent = JSON.stringify({
        url,
        status: res.status,
        content_type: contentType,
        content_length_bytes: Number(contentLength),
        content_length_display: formatBytes(Number(contentLength)),
        headers: Object.fromEntries(res.headers.entries())
      }, null, 2);

      refreshData();
    } catch (err) {
      metaBox.innerHTML = `<span class="status-tag s500">ERROR</span> · ${escapeHtml(err.message)}`;
      jsonBox.textContent = JSON.stringify({ error: err.message }, null, 2);
    }
  }

  $('#testVideoBtn').onclick = () => testMediaFile('video');
  $('#testAudioBtn').onclick = () => testMediaFile('audio');

  $('#copyJsonBtn').onclick = () => {
    navigator.clipboard.writeText($('#apiJsonViewer').textContent);
    showToast('Response JSON copied to clipboard!');
  };

  // Activity filter change
  $('#activityFilter').onchange = (e) => {
    state.activityFilter = e.target.value;
    renderActivityTable();
  };

  // Refresh All Data
  async function refreshData() {
    try {
      const [statusRes, catalogueRes] = await Promise.all([
        fetchApi('/api/status'),
        fetchApi('/api/videos?page=0&limit=50')
      ]);

      state.status = statusRes.data;
      state.items = catalogueRes.data.items || [];

      renderDashboard();
      renderLibrary();
      if (state.currentView === 'activity') {
        renderActivityTable();
      }
    } catch (err) {
      console.error('Refresh failed:', err);
    }
  }

  // Setup Event Listeners
  $$('.nav-btn').forEach((btn) => {
    btn.onclick = () => switchView(btn.dataset.view);
  });

  $$('[data-view]').forEach((elem) => {
    elem.onclick = () => switchView(elem.dataset.view);
  });

  $('#refreshBtn').onclick = async () => {
    showToast('Refreshing server telemetry...');
    await refreshData();
  };

  // Hash Navigation support
  window.onhashchange = () => {
    const hash = window.location.hash.replace('#', '');
    if (hash && ['dashboard', 'library', 'add', 'activity', 'api', 'settings'].includes(hash)) {
      switchView(hash);
    }
  };

  // Initialize
  const initialHash = window.location.hash.replace('#', '');
  if (initialHash && ['dashboard', 'library', 'add', 'activity', 'api', 'settings'].includes(initialHash)) {
    switchView(initialHash);
  } else {
    switchView('dashboard');
  }

  refreshData();
  // Live polling every 3 seconds for real-time ESP activity
  setInterval(refreshData, 3000);
})();
