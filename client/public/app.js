// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

const MAPBOX_TOKEN = 'pk.eyJ1IjoibWF0dGxyb2JlcnRzIiwiYSI6ImNra2o3MWE3aDBic3cydnV6eTdqbDRtbmcifQ.aUiGrdGs4_Fy_x4cJinbDQ';
const WSS_URL = 'ws://localhost:8000';

const MAP_STYLES = {
  'dark-topo':  'mapbox://styles/mapbox/dark-v11',
  'outdoors':   'mapbox://styles/mapbox/outdoors-v12',
  'satellite':  'mapbox://styles/mapbox/satellite-streets-v12',
};

// Single source of truth for all state presentation
const STATE_META = {
  TARGET_STATE_ACTIVE:      { icon: '▲', color: '#226FEE', bg: 'rgba(34,111,238,0.18)',   label: 'ACTIVE',      short: 'ACTV' },
  TARGET_STATE_ACQUIRED:    { icon: '◎', color: '#1EB982', bg: 'rgba(30,185,130,0.18)',   label: 'ACQUIRED',    short: 'ACQD' },
  TARGET_STATE_INACTIVE:    { icon: '■', color: '#9B9B9B', bg: 'rgba(155,155,155,0.15)', label: 'INACTIVE',    short: 'INAC' },
  TARGET_STATE_LOST:        { icon: '◈', color: '#F8C100', bg: 'rgba(248,193,0,0.18)',    label: 'LOST',        short: 'LOST' },
  TARGET_STATE_NEUTRALIZED: { icon: '✕', color: '#E4591D', bg: 'rgba(228,89,29,0.18)',   label: 'NEUTRALIZED', short: 'NEUT' },
  TARGET_STATE_UNKNOWN:     { icon: '○', color: '#5F666C', bg: 'rgba(95,102,108,0.15)',  label: 'UNKNOWN',     short: 'UNKN' },
};

// Derived maps kept for Mapbox expressions and legacy use
const STATE_COLORS = Object.fromEntries(Object.entries(STATE_META).map(([k, v]) => [k, v.color]));
const STATE_LABELS = Object.fromEntries(Object.entries(STATE_META).map(([k, v]) => [k, v.label]));

const SOURCE_ID    = 'targets-source';
const LAYER_CIRCLE = 'targets-circle';
const LAYER_PULSE  = 'targets-pulse';
const LAYER_LABEL  = 'targets-label';

const WORKSPACE_ID = 'xtech-hackathon';

// Shack15, 99 Green St, San Francisco
const SIM_LAT = 37.7993;
const SIM_LNG = -122.3983;

const NATO = ['ALPHA', 'BRAVO', 'CHARLIE', 'DELTA', 'ECHO', 'FOXTROT', 'GOLF', 'HOTEL', 'INDIA', 'JULIET'];

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const repo      = new TargetRepo();
const assetRepo = new TargetRepo();
let messages = [];          // [{sender, content, timestamp}] newest-first, capped at 100
let unreadMessages = 0;
let selectedId = null;
let currentStyle = 'dark-topo';
let popup = null;
let map;
let ws;
let reconnectTimeout;
let simInterval = null;
let simTargetId = null;
let outSimRunning = false;
let activeDropdownId = null;
let activeTab = 'targets';

// ---------------------------------------------------------------------------
// Map init
// ---------------------------------------------------------------------------

mapboxgl.accessToken = MAPBOX_TOKEN;

const CAMERA_STORAGE_KEY = 'xtech_map_camera';

function loadCameraState() {
  try {
    const saved = localStorage.getItem(CAMERA_STORAGE_KEY);
    if (saved) return JSON.parse(saved);
  } catch (_) {}
  return null;
}

function saveCameraState() {
  try {
    localStorage.setItem(CAMERA_STORAGE_KEY, JSON.stringify({
      center:  map.getCenter().toArray(),
      zoom:    map.getZoom(),
      pitch:   map.getPitch(),
      bearing: map.getBearing(),
    }));
  } catch (_) {}
}

const savedCamera = loadCameraState();

map = new mapboxgl.Map({
  container: 'map',
  style: MAP_STYLES['dark-topo'],
  center:  savedCamera ? savedCamera.center  : [-98.5795, 39.8283],
  zoom:    savedCamera ? savedCamera.zoom    : 3,
  pitch:   savedCamera ? savedCamera.pitch   : 40,
  bearing: savedCamera ? savedCamera.bearing : 0,
  antialias: true,
});

map.addControl(new mapboxgl.NavigationControl({ visualizePitch: true }), 'top-left');
map.on('moveend', saveCameraState);

map.on('load', () => {
  addTerrainAndSky();
  initTargetLayers();
  connectWebSocket();
  renderMessages();
});

// ---------------------------------------------------------------------------
// Terrain + sky
// ---------------------------------------------------------------------------

function addTerrainAndSky() {
  if (!map.getSource('mapbox-dem')) {
    map.addSource('mapbox-dem', {
      type: 'raster-dem',
      url: 'mapbox://mapbox.mapbox-terrain-dem-v1',
      tileSize: 512,
      maxzoom: 14,
    });
  }
  map.setTerrain({ source: 'mapbox-dem', exaggeration: 1.5 });

  if (!map.getLayer('sky')) {
    map.addLayer({
      id: 'sky',
      type: 'sky',
      paint: {
        'sky-type': 'atmosphere',
        'sky-atmosphere-sun': [0.0, 90.0],
        'sky-atmosphere-sun-intensity': 15,
      },
    });
  }
}

// ---------------------------------------------------------------------------
// Target layers (GeoJSON source → circle + label)
// ---------------------------------------------------------------------------

function initTargetLayers() {
  map.addSource(SOURCE_ID, { type: 'geojson', data: emptyFeatureCollection() });

  // Outer pulse ring for active targets
  map.addLayer({
    id: LAYER_PULSE,
    type: 'circle',
    source: SOURCE_ID,
    filter: ['==', ['get', 'state'], 'TARGET_STATE_ACTIVE'],
    paint: {
      'circle-radius': 18,
      'circle-color': 'transparent',
      'circle-stroke-width': 1.5,
      'circle-stroke-color': '#226FEE',
      'circle-stroke-opacity': 0.5,
    },
  });

  // Main dot
  map.addLayer({
    id: LAYER_CIRCLE,
    type: 'circle',
    source: SOURCE_ID,
    paint: {
      'circle-radius': ['case', ['boolean', ['feature-state', 'selected'], false], 11, 9],
      'circle-color': stateColorExpression(),
      'circle-stroke-width': ['case', ['boolean', ['feature-state', 'selected'], false], 2, 1.5],
      'circle-stroke-color': [
        'case',
        ['boolean', ['feature-state', 'selected'], false], '#FFFFFF',
        'rgba(255,255,255,0.4)',
      ],
    },
  });

  // Label (short ID)
  map.addLayer({
    id: LAYER_LABEL,
    type: 'symbol',
    source: SOURCE_ID,
    layout: {
      'text-field': ['get', 'shortId'],
      'text-size': 10,
      'text-offset': [0, 1.6],
      'text-anchor': 'top',
      'text-font': ['DIN Pro Medium', 'Arial Unicode MS Regular'],
    },
    paint: {
      'text-color': '#FFFFFF',
      'text-halo-color': 'rgba(0,0,0,0.6)',
      'text-halo-width': 1.5,
    },
  });

  map.on('click', LAYER_CIRCLE, (e) => selectTarget(e.features[0].properties.id, true));
  map.on('mouseenter', LAYER_CIRCLE, () => { map.getCanvas().style.cursor = 'pointer'; });
  map.on('mouseleave', LAYER_CIRCLE, () => { map.getCanvas().style.cursor = ''; });
}

function stateColorExpression() {
  return [
    'match', ['get', 'state'],
    'TARGET_STATE_ACTIVE',      '#226FEE',
    'TARGET_STATE_ACQUIRED',    '#1EB982',
    'TARGET_STATE_INACTIVE',    '#9B9B9B',
    'TARGET_STATE_LOST',        '#F8C100',
    'TARGET_STATE_NEUTRALIZED', '#E4591D',
    /* default */ '#5F666C',
  ];
}

// ---------------------------------------------------------------------------
// GeoJSON helpers
// ---------------------------------------------------------------------------

function emptyFeatureCollection() {
  return { type: 'FeatureCollection', features: [] };
}

function targetToFeature(t) {
  const display = displayPositions.get(t.id);
  const loc = t.tracking_location || {};
  const lng = display ? display.lng : (loc.longitude  || 0) / 1e7;
  const lat = display ? display.lat : (loc.latitude   || 0) / 1e7;
  const shortId = t.label || (t.id ? t.id.split('-')[0].toUpperCase() : '???');
  return {
    type: 'Feature',
    id: t.id,
    geometry: { type: 'Point', coordinates: [lng, lat] },
    properties: {
      id:       t.id,
      state:    t.state || 'TARGET_STATE_UNKNOWN',
      shortId,
      label:    t.label || '',
      altitude: loc.altitude || 0,
      speed:    loc.speed_over_ground || 0,
      course:   loc.course_over_ground || 0,
      updated:  (t.updated_date || {}).seconds || 0,
    },
  };
}

function rebuildSource() {
  const src = map.getSource(SOURCE_ID);
  if (!src) return;
  src.setData({ type: 'FeatureCollection', features: repo.list().map(targetToFeature) });
}

// ---------------------------------------------------------------------------
// Position animation
// ---------------------------------------------------------------------------

const displayPositions = new Map(); // id → { lng, lat }
const activeAnimations = new Map(); // id → { startLng, startLat, endLng, endLat, startTime }
const ANIM_MS = 700;

function lerp(a, b, t) { return a + (b - a) * t; }
function easeInOut(t) { return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t; }

function setDisplayPosition(id, lng, lat) {
  displayPositions.set(id, { lng, lat });
}

function animateToPosition(id, toLng, toLat) {
  const current = displayPositions.get(id) || { lng: toLng, lat: toLat };
  activeAnimations.set(id, {
    startLng: current.lng, startLat: current.lat,
    endLng: toLng, endLat: toLat,
    startTime: performance.now(),
  });
  if (activeAnimations.size === 1) requestAnimationFrame(animationTick);
}

function animationTick(now) {
  let hasActive = false;
  for (const [id, anim] of activeAnimations) {
    const t = Math.min((now - anim.startTime) / ANIM_MS, 1);
    const et = easeInOut(t);
    displayPositions.set(id, {
      lng: lerp(anim.startLng, anim.endLng, et),
      lat: lerp(anim.startLat, anim.endLat, et),
    });
    if (t >= 1) activeAnimations.delete(id);
    else hasActive = true;
  }
  rebuildSource();
  if (hasActive) requestAnimationFrame(animationTick);
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

function connectWebSocket() {
  clearTimeout(reconnectTimeout);
  setConnectionStatus('connecting');

  ws = new WebSocket(WSS_URL);

  ws.onopen = () => {
    setConnectionStatus('connected');
    ws.send(JSON.stringify({ action: 'list',        payload: {} }));
    ws.send(JSON.stringify({ action: 'list_assets', payload: {} }));
  };

  ws.onmessage = (e) => {
    let msg;
    try { msg = JSON.parse(e.data); } catch { return; }
    handleMessage(msg);
  };

  ws.onerror = () => setConnectionStatus('disconnected');

  ws.onclose = () => {
    setConnectionStatus('disconnected');
    reconnectTimeout = setTimeout(connectWebSocket, 3000);
  };
}

function handleMessage(msg) {
  if (msg.status === 'success' && msg.action === 'list') {
    repo.reset(msg.data || []);
    for (const t of repo.list()) {
      const loc = t.tracking_location || {};
      setDisplayPosition(t.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    }
    rebuildSource();
    renderList();
    return;
  }

  if (msg.event === 'target_created') {
    if (!repo.upsert(msg.data)) return;
    const loc = msg.data.tracking_location || {};
    setDisplayPosition(msg.data.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    rebuildSource();
    renderList();
    if (selectedId === msg.data.id) renderPopup(msg.data);
    return;
  }

  if (msg.event === 'target_updated') {
    if (!repo.upsert(msg.data)) return;
    const loc = msg.data.tracking_location || {};
    animateToPosition(msg.data.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    renderList();
    if (selectedId === msg.data.id) renderPopup(msg.data);
    return;
  }

  if (msg.event === 'target_deleted') {
    const id = msg.data.id;
    repo.delete(id);
    displayPositions.delete(id);
    activeAnimations.delete(id);
    if (selectedId === id) {
      selectedId = null;
      if (popup) { popup.remove(); popup = null; }
    }
    rebuildSource();
    renderList();
    return;
  }

  if (msg.status === 'success' && msg.action === 'list_assets') {
    assetRepo.reset(msg.data || []);
    renderAssets();
    return;
  }

  if (msg.event === 'asset_created' || msg.event === 'asset_updated') {
    assetRepo.upsert(msg.data);
    renderAssets();
    return;
  }

  if (msg.event === 'asset_deleted') {
    assetRepo.delete(msg.data.id);
    renderAssets();
    return;
  }

  if (msg.event === 'sim_state' || (msg.status === 'success' && (msg.action === 'sim_start' || msg.action === 'sim_stop'))) {
    const running = msg.event === 'sim_state' ? msg.data.running : (msg.action === 'sim_start');
    outSimRunning = running;
    const btn = document.getElementById('out-sim-btn');
    btn.textContent = running ? 'STOP OUT' : 'OUT SIM';
    btn.classList.toggle('active', running);
    return;
  }

  if (msg.event === 'beam_message') {
    const identity = msg.data.identity || {};
    const sender = identity.name || identity.id || msg.data.account_id || 'Unknown';
    messages.unshift({ sender, content: msg.data.content || '', timestamp: msg.data.timestamp || '' });
    if (messages.length > 100) messages.pop();
    unreadMessages++;
    renderMessages();
  }
}

// ---------------------------------------------------------------------------
// Sidebar list
// ---------------------------------------------------------------------------

function stateBadgeHtml(state) {
  const meta = STATE_META[state] || STATE_META.TARGET_STATE_UNKNOWN;
  return `<span class="state-badge" style="color:${meta.color};background:${meta.bg};border-color:${meta.color}44">
    <span class="state-icon">${meta.icon}</span>${meta.short}
  </span>`;
}

function stateDropdownHtml(currentState) {
  return Object.entries(STATE_META).map(([key, m]) => `
    <div class="state-drop-item${currentState === key ? ' current' : ''}" data-state="${key}"
         style="--item-color:${m.color}">
      <span class="state-icon">${m.icon}</span>${m.label}
    </div>`).join('');
}

function updateTargetState(id, newState) {
  const current = repo.get(id);
  if (current) {
    const updated = { ...current, state: newState, updated_date: { seconds: Math.floor(Date.now() / 1000), nanos: 0 } };
    repo.upsert(updated);
    rebuildSource();
    renderList();
    if (selectedId === id) renderPopup(updated);
  }
  if (ws?.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ action: 'update', payload: { id, state: newState } }));
  }
}

function renderList() {
  const container = document.getElementById('target-list');
  const list = repo.list();

  if (list.length === 0) {
    container.innerHTML = '<div class="empty-state">No targets</div>';
    return;
  }

  list.sort((a, b) => {
    const sa = (a.updated_date || {}).seconds || 0;
    const sb = (b.updated_date || {}).seconds || 0;
    return sb - sa;
  });

  container.innerHTML = list.map(t => {
    const loc = t.tracking_location || {};
    const lng = ((loc.longitude || 0) / 1e7).toFixed(4);
    const lat = ((loc.latitude  || 0) / 1e7).toFixed(4);
    const idStr = t.id != null ? String(t.id) : '';
    const displayName = t.label || (idStr ? idStr.split('-')[0].toUpperCase() : '???');
    const shortId = idStr ? idStr.split('-')[0].toUpperCase() : '';
    const updSecs = (t.updated_date || {}).seconds || 0;
    const updated = updSecs ? new Date(updSecs * 1000).toLocaleTimeString() : '';
    const sel = t.id === selectedId ? ' selected' : '';
    const dropOpen = activeDropdownId === t.id ? ' open' : '';
    return `
      <div class="target-card${sel}" data-id="${t.id}">
        <div class="target-main">
          <div class="target-row">
            <div class="target-id">${displayName}<span class="target-short-id">${t.label ? 'id: ' + shortId : ''}</span></div>
            <div class="target-updated">${updated}</div>
          </div>
          <div class="target-row">
            <div class="target-coords">${lat}, ${lng}</div>
            <div class="state-badge-wrap${dropOpen}" data-id="${t.id}">
              ${stateBadgeHtml(t.state)}
              <div class="state-dropdown">${stateDropdownHtml(t.state)}</div>
            </div>
          </div>
        </div>
      </div>`;
  }).join('');

  container.querySelectorAll('.target-card').forEach(card => {
    card.addEventListener('click', () => selectTarget(card.dataset.id, true));
  });

  container.querySelectorAll('.state-badge-wrap').forEach(wrap => {
    wrap.addEventListener('click', (e) => {
      e.stopPropagation();
      const id = wrap.dataset.id;
      activeDropdownId = activeDropdownId === id ? null : id;
      renderList();
    });
  });

  container.querySelectorAll('.state-drop-item').forEach(item => {
    item.addEventListener('click', (e) => {
      e.stopPropagation();
      const id = item.closest('.state-badge-wrap').dataset.id;
      activeDropdownId = null;
      updateTargetState(id, item.dataset.state);
    });
  });
}

document.addEventListener('click', () => {
  if (activeDropdownId !== null) {
    activeDropdownId = null;
    renderList();
  }
});

function renderAssets() {
  const container = document.getElementById('asset-list');
  const list = assetRepo.list();

  if (list.length === 0) {
    container.innerHTML = '<div class="empty-state">No assets</div>';
    return;
  }

  list.sort((a, b) => ((b.updated_date || {}).seconds || 0) - ((a.updated_date || {}).seconds || 0));

  container.innerHTML = list.map(a => {
    const loc = a.tracking_location || {};
    const lng = ((loc.longitude || 0) / 1e7).toFixed(4);
    const lat = ((loc.latitude  || 0) / 1e7).toFixed(4);
    const name    = a.label || (a.id ? a.id.split('-')[0].toUpperCase() : '???');
    const updSecs = (a.updated_date || {}).seconds || 0;
    const updated = updSecs ? new Date(updSecs * 1000).toLocaleTimeString() : '';
    return `
      <div class="asset-card">
        <div class="target-main">
          <div class="target-row">
            <div class="target-id">${name}</div>
            <div class="target-updated">${updated}</div>
          </div>
          <div class="target-row">
            <div class="target-coords">${lat}, ${lng}</div>
            <span class="asset-badge">LOCATION</span>
          </div>
        </div>
      </div>`;
  }).join('');
}

// ---------------------------------------------------------------------------
// Tab switching
// ---------------------------------------------------------------------------

document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    activeTab = btn.dataset.tab;
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b === btn));
    document.getElementById('target-list').style.display = activeTab === 'targets' ? '' : 'none';
    document.getElementById('asset-list').style.display  = activeTab === 'assets'  ? '' : 'none';
    // Also hide toolbar (populate) on assets tab
    document.getElementById('sidebar-toolbar').style.display = activeTab === 'targets' ? '' : 'none';
  });
});

// ---------------------------------------------------------------------------
// Messages feed
// ---------------------------------------------------------------------------

function renderMessages() {
  const container = document.getElementById('message-list');
  const badge = document.getElementById('messages-unread');

  if (unreadMessages > 0) {
    badge.textContent = unreadMessages > 99 ? '99+' : unreadMessages;
    badge.classList.remove('hidden');
  } else {
    badge.classList.add('hidden');
  }

  const section = document.getElementById('messages-section');
  if (section.getBoundingClientRect().height > 0) {
    unreadMessages = 0;
    badge.classList.add('hidden');
  }

  if (messages.length === 0) {
    container.innerHTML = '<div class="messages-empty">No messages</div>';
    return;
  }

  container.innerHTML = messages.map(m => {
    const time = m.timestamp
      ? new Date(m.timestamp).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
      : '';
    return `
      <div class="message-row">
        <div class="message-meta">
          <span class="message-sender">${escapeHtml(m.sender)}</span>
          <span class="message-time">${time}</span>
        </div>
        <div class="message-content">${escapeHtml(m.content)}</div>
      </div>`;
  }).join('');
}

function escapeHtml(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function formatAge(unixSeconds) {
  if (!unixSeconds) return '';
  const delta = Math.floor(Date.now() / 1000) - unixSeconds;
  if (delta < 60)   return `${delta}s`;
  if (delta < 3600) return `${Math.floor(delta / 60)}m`;
  return `${Math.floor(delta / 3600)}h`;
}

// ---------------------------------------------------------------------------
// Selection + popup
// ---------------------------------------------------------------------------

function selectTarget(id, flyTo) {
  if (selectedId && selectedId !== id) {
    map.setFeatureState({ source: SOURCE_ID, id: selectedId }, { selected: false });
  }

  selectedId = id;
  map.setFeatureState({ source: SOURCE_ID, id }, { selected: true });
  renderList();

  const t = repo.get(id);
  if (!t) return;

  if (flyTo) {
    const display = displayPositions.get(id);
    const loc = t.tracking_location || {};
    const lng = display ? display.lng : (loc.longitude || 0) / 1e7;
    const lat = display ? display.lat : (loc.latitude  || 0) / 1e7;
    if (lng !== 0 || lat !== 0) {
      map.flyTo({ center: [lng, lat], zoom: Math.max(map.getZoom(), 14), duration: 800 });
    }
  }

  renderPopup(t);
}

function renderPopup(t) {
  if (popup) popup.remove();

  const loc = t.tracking_location || {};
  const lng = (loc.longitude || 0) / 1e7;
  const lat = (loc.latitude  || 0) / 1e7;
  if (lng === 0 && lat === 0) return;

  const meta    = STATE_META[t.state] || STATE_META.TARGET_STATE_UNKNOWN;
  const altM    = ((loc.altitude || 0) / 1000).toFixed(0);
  const speedKph = ((loc.speed_over_ground || 0) * 0.0036).toFixed(1);
  const course  = ((loc.course_over_ground || 0) / 1000).toFixed(0);
  const updated = t.updated_date
    ? new Date((t.updated_date.seconds || 0) * 1000).toLocaleTimeString()
    : '--';

  const statePicker = Object.entries(STATE_META).map(([key, m]) => {
    const isActive = t.state === key;
    const style = isActive
      ? `color:${m.color};border-color:${m.color};background:${m.bg}`
      : `color:${m.color}`;
    return `<button class="state-pick-btn${isActive ? ' active' : ''}" data-state="${key}" style="${style}" title="${m.label}">
      <span class="pick-icon">${m.icon}</span>
      <span class="pick-label">${m.short}</span>
    </button>`;
  }).join('');

  const html = `
    <div class="popup-target-id">${t.label || t.id}</div>
    <div class="popup-row">
      <span class="popup-label">STATE</span>
      <span class="popup-value" style="color:${meta.color}">${meta.icon} ${meta.label}</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">LAT / LNG</span>
      <span class="popup-value">${lat.toFixed(5)}, ${lng.toFixed(5)}</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">ALT</span>
      <span class="popup-value">${altM} m</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">SPEED</span>
      <span class="popup-value">${speedKph} km/h</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">COURSE</span>
      <span class="popup-value">${course}°</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">UPDATED</span>
      <span class="popup-value">${updated}</span>
    </div>
    <div class="popup-divider"></div>
    <div class="popup-section-label">SET STATE</div>
    <div class="popup-state-picker">${statePicker}</div>`;

  popup = new mapboxgl.Popup({ closeButton: true, maxWidth: '320px', offset: 14 })
    .setLngLat([lng, lat])
    .setHTML(html);

  popup.on('open', () => {
    popup.getElement().querySelector('.popup-state-picker').addEventListener('click', (e) => {
      const btn = e.target.closest('.state-pick-btn');
      if (!btn) return;
      const newState = btn.dataset.state;

      // Optimistic update: push to repo immediately so the UI reflects the change now.
      const current = repo.get(t.id);
      if (current) {
        const optimistic = { ...current, state: newState, updated_date: { seconds: Math.floor(Date.now() / 1000), nanos: 0 } };
        if (repo.upsert(optimistic)) {
          rebuildSource();
          renderList();
          renderPopup(optimistic);
        }
      }

      if (ws?.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'update', payload: { id: t.id, state: newState } }));
      }
    });
  });

  popup.on('close', () => {
    if (selectedId) map.setFeatureState({ source: SOURCE_ID, id: selectedId }, { selected: false });
    selectedId = null;
    renderList();
  });

  popup.addTo(map);
}

// ---------------------------------------------------------------------------
// Populate fake targets
// ---------------------------------------------------------------------------

function randomNearSim(radiusM = 800) {
  const latDeg = radiusM / 111000;
  const lngDeg = radiusM / (111000 * Math.cos(SIM_LAT * Math.PI / 180));
  const angle = Math.random() * 2 * Math.PI;
  const r = Math.sqrt(Math.random());
  return {
    lat: SIM_LAT + r * latDeg * Math.cos(angle),
    lng: SIM_LNG + r * lngDeg * Math.sin(angle),
  };
}

function populateFakeTargets() {
  const states = Object.keys(STATE_META);
  const count = Math.min(NATO.length, states.length);

  for (let i = 0; i < count; i++) {
    const { lat, lng } = randomNearSim();
    const now = Math.floor(Date.now() / 1000);
    const target = {
      id:           crypto.randomUUID(),
      updated_date: { seconds: now, nanos: 0 },
      tracking_location: {
        longitude:          Math.round(lng * 1e7),
        latitude:           Math.round(lat * 1e7),
        timestamp:          now,
        altitude:           0,
        speed_over_ground:  0,
        course_over_ground: 0,
      },
      state:        states[i],
      workspace_id: WORKSPACE_ID,
      label:        NATO[i],
    };

    // Always push locally so the UI works without a server connection
    if (repo.create(target)) {
      setDisplayPosition(target.id, lng, lat);
    }

    // Also sync to server if connected so other clients see these targets
    if (ws?.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'create', payload: target }));
    }
  }

  rebuildSource();
  renderList();
}

document.getElementById('populate-btn').addEventListener('click', populateFakeTargets);

// ---------------------------------------------------------------------------
// Sim loop
// ---------------------------------------------------------------------------

function emitSim() {
  fetch('/emit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: simTargetId }),
  });
}

function toggleSim() {
  const btn = document.getElementById('sim-btn');
  if (simInterval) {
    clearInterval(simInterval);
    simInterval = null;
    simTargetId = null;
    btn.textContent = 'SIM';
    btn.classList.remove('active');
  } else {
    simTargetId = crypto.randomUUID();
    emitSim();
    simInterval = setInterval(emitSim, 2000);
    btn.textContent = 'STOP';
    btn.classList.add('active');
  }
}

document.getElementById('sim-btn').addEventListener('click', toggleSim);

function toggleOutSim() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify({ action: outSimRunning ? 'sim_stop' : 'sim_start', payload: {} }));
}

document.getElementById('out-sim-btn').addEventListener('click', toggleOutSim);

// ---------------------------------------------------------------------------
// RTMS PiP viewer
// ---------------------------------------------------------------------------

(function initPip() {
  const RTSP_WS_URL = `ws://${location.hostname}:9999`;

  const container = document.getElementById('pip-container');
  const canvas    = document.getElementById('pip-canvas');
  const statusEl  = document.getElementById('pip-status');
  const toggleBtn = document.getElementById('pip-toggle');

  let collapsed = false;

  function setPipStatus(state) {
    statusEl.className = `pip-status-${state}`;
    statusEl.textContent = { live: 'LIVE', connecting: 'CONNECTING', error: 'ERROR' }[state];
  }

  setPipStatus('connecting');

  // JSMpeg handles reconnection internally (reconnectInterval defaults to 5s).
  // Don't manage the WebSocket manually — just use the provided callbacks.
  new JSMpeg.Player(RTSP_WS_URL, {
    canvas,
    autoplay: true,
    audio: false,
    disableGl: true,
    reconnectInterval: 5,
    onSourceEstablished: () => setPipStatus('live'),
    onSourceCompleted:   () => setPipStatus('connecting'),
  });

  toggleBtn.addEventListener('click', () => {
    collapsed = !collapsed;
    container.classList.toggle('collapsed', collapsed);
    toggleBtn.textContent = collapsed ? '▲' : '▼';
  });
})();

// ---------------------------------------------------------------------------
// Basemap selector
// ---------------------------------------------------------------------------

document.querySelectorAll('.basemap-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    const style = btn.dataset.style;
    if (style === currentStyle) return;
    currentStyle = style;

    document.querySelectorAll('.basemap-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');

    map.setStyle(MAP_STYLES[style]);

    map.once('style.load', () => {
      addTerrainAndSky();
      initTargetLayers();
      rebuildSource();
    });
  });
});

// ---------------------------------------------------------------------------
// Connection status UI
// ---------------------------------------------------------------------------

function setConnectionStatus(state) {
  const el = document.getElementById('connection-status');
  el.className = `status-${state}`;
  el.textContent = state.toUpperCase();
}
