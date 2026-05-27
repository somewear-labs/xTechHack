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
  TARGET_STATE_UNKNOWN:     { icon: '○', color: '#5F666C', bg: 'rgba(95,102,108,0.15)',  label: 'UNKNOWN',     short: 'UNKN' },
  TARGET_STATE_CONFIRMED:   { icon: '◎', color: '#226FEE', bg: 'rgba(34,111,238,0.18)',  label: 'CONFIRMED',   short: 'CONF' },
  TARGET_STATE_NEUTRALIZED: { icon: '✕', color: '#E4591D', bg: 'rgba(228,89,29,0.18)',  label: 'NEUTRALIZED', short: 'NEUT' },
  TARGET_STATE_INACTIVE:    { icon: '■', color: '#9B9B9B', bg: 'rgba(155,155,155,0.15)', label: 'INACTIVE',    short: 'INAC' },
};

// Derived maps kept for Mapbox expressions and legacy use
const STATE_COLORS = Object.fromEntries(Object.entries(STATE_META).map(([k, v]) => [k, v.color]));
const STATE_LABELS = Object.fromEntries(Object.entries(STATE_META).map(([k, v]) => [k, v.label]));

// COCO-80 class names (index = class_id)
const COCO_CLASSES = [
  'person','bicycle','car','motorcycle','airplane','bus','train','truck','boat',
  'traffic light','fire hydrant','stop sign','parking meter','bench','bird','cat',
  'dog','horse','sheep','cow','elephant','bear','zebra','giraffe','backpack',
  'umbrella','handbag','tie','suitcase','frisbee','skis','snowboard','sports ball',
  'kite','baseball bat','baseball glove','skateboard','surfboard','tennis racket',
  'bottle','wine glass','cup','fork','knife','spoon','bowl','banana','apple',
  'sandwich','orange','broccoli','carrot','hot dog','pizza','donut','cake','chair',
  'couch','potted plant','bed','dining table','toilet','tv','laptop','mouse',
  'remote','keyboard','cell phone','microwave','oven','toaster','sink',
  'refrigerator','book','clock','vase','scissors','teddy bear','hair drier',
  'toothbrush',
];

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
let currentStyle = 'satellite';
let overlayState = null; // { id, lng, lat, color }
const targetImages = new Map(); // id → cropped-frame dataURL
const protoMarkers = new Map(); // id → { marker: mapboxgl.Marker, el: HTMLElement }
const targetLabels = new Map(); // id → {classId, objectId, label}
let rtmsCanvas = null;
const FRAME_BUFFER_SIZE = 60;   // ~4 s at 15 fps
const frameBuffers = [[], []];  // per-cam [{pts_ns, snap}] FIFO, oldest first
// Legacy alias — always points at the active cam's buffer for external callers
let frameBuffer = frameBuffers[0];
let activeCam = 0;
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
  style: MAP_STYLES['satellite'],
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
  initMapOverlay();
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
    filter: ['==', ['get', 'state'], 'TARGET_STATE_CONFIRMED'],
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
    'TARGET_STATE_CONFIRMED',   '#226FEE',
    'TARGET_STATE_NEUTRALIZED', '#E4591D',
    'TARGET_STATE_INACTIVE',    '#9B9B9B',
    /* default (UNKNOWN) */ '#5F666C',
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
  syncMarkers();
}

// ---------------------------------------------------------------------------
// Proto target map markers (Android box style)
// ---------------------------------------------------------------------------

function markerLngLat(t) {
  const display = displayPositions.get(t.id);
  const loc = t.tracking_location || {};
  const lng = display ? display.lng : (loc.longitude || 0) / 1e7;
  const lat = display ? display.lat : (loc.latitude  || 0) / 1e7;
  return (lng === 0 && lat === 0) ? null : [lng, lat];
}

function updateMarkerEl(el, t) {
  const color = STATE_COLORS[t.state] || '#5F666C';
  const decoded = targetLabels.get(t.id);
  const label = decoded
    ? `${decoded.label.toUpperCase()} #${decoded.objectId}`
    : (t.label || String(t.id).slice(-4).toUpperCase());
  const selected = t.id === selectedId;
  el.innerHTML = `
    <div class="pm-box${selected ? ' pm-selected' : ''}" style="border-color:${color}">
      <div class="pm-corner pm-tl" style="background:${color}"></div>
      <div class="pm-corner pm-tr" style="background:${color}"></div>
      <div class="pm-corner pm-bl" style="background:${color}"></div>
      <div class="pm-corner pm-br" style="background:${color}"></div>
      <div class="pm-dot"></div>
    </div>
    <div class="pm-label" style="color:${color}">${label}</div>`;
}

function syncMarkers() {
  if (!map.isStyleLoaded()) return;
  const targets = repo.list();
  const liveIds = new Set(targets.map(t => String(t.id)));

  for (const [id, { marker }] of protoMarkers) {
    if (!liveIds.has(id)) { marker.remove(); protoMarkers.delete(id); }
  }

  for (const t of targets) {
    const ll = markerLngLat(t);
    if (!ll) continue;
    if (protoMarkers.has(t.id)) {
      const { marker, el } = protoMarkers.get(t.id);
      marker.setLngLat(ll);
      updateMarkerEl(el, t);
    } else {
      const el = document.createElement('div');
      el.className = 'proto-marker';
      updateMarkerEl(el, t);
      const marker = new mapboxgl.Marker({ element: el, anchor: 'center' })
        .setLngLat(ll)
        .addTo(map);
      el.addEventListener('click', (e) => { e.stopPropagation(); selectTarget(t.id, false); });
      protoMarkers.set(t.id, { marker, el });
    }
  }
}

function syncMarkerPositions() {
  for (const t of repo.list()) {
    const entry = protoMarkers.get(t.id);
    if (!entry) continue;
    const ll = markerLngLat(t);
    if (ll) entry.marker.setLngLat(ll);
  }
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
  const _src = map.getSource(SOURCE_ID);
  if (_src) _src.setData({ type: 'FeatureCollection', features: repo.list().map(targetToFeature) });
  syncMarkerPositions();
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

function normalizeTarget(t) {
  if (!t || t.id == null) return t;
  t = { ...t, id: String(t.id) };
  const decoded = decodeProtoId(t.id);
  if (decoded) targetLabels.set(t.id, decoded);
  return t;
}

function handleMessage(msg) {
  if (msg.status === 'success' && msg.action === 'list') {
    repo.reset((msg.data || []).map(normalizeTarget));
    for (const t of repo.list()) {
      const loc = t.tracking_location || {};
      setDisplayPosition(t.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    }
    rebuildSource();
    renderList();
    return;
  }

  if (msg.event === 'target_created') {
    const t = normalizeTarget(msg.data);
    if (!repo.upsert(t)) return;
    const loc = t.tracking_location || {};
    setDisplayPosition(t.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    rebuildSource();
    renderList();
    if (selectedId === t.id) renderMapOverlay(t);
    return;
  }

  if (msg.event === 'target_updated') {
    const t = normalizeTarget(msg.data);
    if (!repo.upsert(t)) return;
    const loc = t.tracking_location || {};
    animateToPosition(t.id, (loc.longitude || 0) / 1e7, (loc.latitude || 0) / 1e7);
    renderList();
    if (selectedId === t.id) renderMapOverlay(t);
    return;
  }

  if (msg.event === 'target_deleted') {
    const id = String(msg.data.id);
    repo.delete(id);
    displayPositions.delete(id);
    activeAnimations.delete(id);
    targetImages.delete(id);
    targetLabels.delete(id);
    protoMarkers.get(id)?.marker.remove();
    protoMarkers.delete(id);
    if (selectedId === id) {
      overlayState = null;
      selectedId = null;
      const _p = document.getElementById('overlay-panel');
      const _s = document.getElementById('overlay-svg');
      if (_p) _p.style.display = 'none';
      if (_s) _s.innerHTML = '';
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

  if (msg.event === 'frame_detection') {
    handleFrameDetection(msg.data);
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
    <span class="state-icon">${meta.icon}</span>${meta.label}
  </span>`;
}

function stateDropdownHtml(currentState) {
  return Object.entries(STATE_META)
    .filter(([key]) => key === currentState || isValidStateTransition(currentState, key))
    .map(([key, m]) => `
      <div class="state-drop-item${currentState === key ? ' current' : ''}" data-state="${key}"
           style="--item-color:${m.color}">
        <span class="state-icon">${m.icon}</span>${m.label}
      </div>`).join('');
}

function updateTargetState(id, newState) {
  const current = repo.get(id);
  if (!current) return;

  // Optimistic in-memory update — preserve existing location
  const updated = { ...current, state: newState, updated_date: { seconds: Math.floor(Date.now() / 1000), nanos: 0 } };
  if (!repo.upsert(updated)) return; // rejected by state machine
  rebuildSource();
  renderList();
  document.querySelector(`#target-list .target-card[data-id="${id}"]`)
    ?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  if (selectedId === id) renderMapOverlay(updated);

  // Emit to server with zeroed coords to signal state-only update
  if (ws?.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({
      action: 'update',
      payload: {
        id,
        state: newState,
        tracking_location: { longitude: 0, latitude: 0, timestamp: Math.floor(Date.now() / 1000), altitude: 0, speed_over_ground: 0, course_over_ground: 0 },
      },
    }));
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
    const updSecs = (t.updated_date || {}).seconds || 0;
    const updated = updSecs ? new Date(updSecs * 1000).toLocaleTimeString() : '';
    const sel = t.id === selectedId ? ' selected' : '';
    const dropOpen = activeDropdownId === t.id ? ' open' : '';
    const imgUrl = targetImages.get(t.id);
    const thumbHtml = imgUrl
      ? `<div class="target-thumb-wrap"><img class="target-thumb" src="${imgUrl}" alt=""></div>`
      : '';
    const decoded = targetLabels.get(idStr);
    const cardName = decoded
      ? `${decoded.label.toUpperCase()} #${decoded.objectId}`
      : (t.label || (idStr ? idStr.split('-')[0].toUpperCase() : '???'));
    const cardSubId = decoded ? `id: ${idStr}` : (t.label ? 'id: ' + idStr : '');
    return `
      <div class="target-card${sel}" data-id="${t.id}">
        ${thumbHtml}
        <div class="target-main">
          <div class="target-row">
            <div class="target-id">${cardName}<span class="target-short-id">${cardSubId}</span></div>
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
      selectTarget(id, true);
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
    document.getElementById('target-list').style.display  = activeTab === 'targets'  ? '' : 'none';
    document.getElementById('asset-list').style.display   = activeTab === 'assets'   ? '' : 'none';
    document.getElementById('message-list').style.display = activeTab === 'messages' ? '' : 'none';
    document.getElementById('sidebar-toolbar').style.display = activeTab === 'targets' ? '' : 'none';
    if (activeTab === 'messages') {
      unreadMessages = 0;
      renderMessages();
    }
  });
});

// ---------------------------------------------------------------------------
// Messages feed
// ---------------------------------------------------------------------------

function renderMessages() {
  const container = document.getElementById('message-list');
  const badge = document.getElementById('messages-unread');

  if (activeTab === 'messages') unreadMessages = 0;

  if (unreadMessages > 0) {
    badge.textContent = unreadMessages > 99 ? '99+' : unreadMessages;
    badge.classList.remove('hidden');
  } else {
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
  const prevId = selectedId;
  if (prevId && prevId !== id) {
    try { map.setFeatureState({ source: SOURCE_ID, id: prevId }, { selected: false }); } catch (_) {}
    const prev = repo.get(prevId);
    if (prev) { const e = protoMarkers.get(prevId)?.el; if (e) updateMarkerEl(e, prev); }
  }

  selectedId = id;
  try { map.setFeatureState({ source: SOURCE_ID, id }, { selected: true }); } catch (_) {}
  const cur = repo.get(id);
  if (cur) { const e = protoMarkers.get(id)?.el; if (e) updateMarkerEl(e, cur); }
  renderList();

  const t = repo.get(id);
  if (!t) return;

  if (flyTo) {
    const display = displayPositions.get(id);
    const loc = t.tracking_location || {};
    const lng = display ? display.lng : (loc.longitude || 0) / 1e7;
    const lat = display ? display.lat : (loc.latitude  || 0) / 1e7;
    if (lng !== 0 || lat !== 0) {
      map.flyTo({ center: [lng, lat], zoom: map.getZoom(), duration: 800, pitch: map.getPitch(), bearing: map.getBearing() });
    }
  }

  renderMapOverlay(t);
}

// ---------------------------------------------------------------------------
// HUD overlay (replaces Mapbox popup)
// ---------------------------------------------------------------------------

function initMapOverlay() {
  const mc = document.getElementById('map-container');

  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.id = 'overlay-svg';
  mc.appendChild(svg);

  const panel = document.createElement('div');
  panel.id = 'overlay-panel';
  panel.innerHTML = `
    <span class="ol-c ol-tl"></span><span class="ol-c ol-tr"></span>
    <span class="ol-c ol-bl"></span><span class="ol-c ol-br"></span>
    <div id="overlay-inner"></div>`;
  mc.appendChild(panel);

  map.on('move', positionMapOverlay);
}

function closeMapOverlay() {
  overlayState = null;
  const panel = document.getElementById('overlay-panel');
  const svg   = document.getElementById('overlay-svg');
  if (panel) panel.style.display = 'none';
  if (svg)   svg.innerHTML = '';
  if (selectedId) {
    try { map.setFeatureState({ source: SOURCE_ID, id: selectedId }, { selected: false }); } catch (_) {}
    selectedId = null;
  }
  renderList();
}

function renderMapOverlay(t) {
  const loc = t.tracking_location || {};
  const lng = (loc.longitude || 0) / 1e7;
  const lat = (loc.latitude  || 0) / 1e7;
  if (lng === 0 && lat === 0) return;

  const meta = STATE_META[t.state] || STATE_META.TARGET_STATE_UNKNOWN;

  const statePicker = Object.entries(STATE_META).map(([key, m]) => {
    const active   = t.state === key;
    const allowed  = active || isValidStateTransition(t.state, key);
    if (!allowed) return '';
    return `<button class="ol-state-btn${active ? ' active' : ''}" data-state="${key}" data-id="${t.id}"
      style="color:${m.color}${active ? `;border-color:${m.color};background:${m.bg}` : ''}">
      <span>${m.icon}</span><span>${m.label}</span>
    </button>`;
  }).join('');

  const inner = document.getElementById('overlay-inner');
  if (!inner) return;
  const decoded = targetLabels.get(t.id);
  const headerTitle = decoded
    ? `${decoded.label.toUpperCase()} #${decoded.objectId}`
    : (t.label || String(t.id).slice(-4).toUpperCase());
  inner.innerHTML = `
    <div class="ol-header">
      <span class="ol-title">${headerTitle}</span>
      <span class="ol-state-badge" style="color:${meta.color}">${meta.icon} ${meta.short}</span>
      <button class="ol-close" onclick="closeMapOverlay()">✕</button>
    </div>
    <div class="ol-data">
      <div class="ol-row"><span class="ol-key">LAT / LNG</span><span class="ol-val">${lat.toFixed(5)},&thinsp;${lng.toFixed(5)}</span></div>
    </div>
    <div class="ol-divider"></div>
    <div class="ol-state-label">SET STATE</div>
    <div class="ol-state-grid">${statePicker}</div>`;

  inner.querySelectorAll('.ol-state-btn').forEach(btn => {
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      updateTargetState(btn.dataset.id, btn.dataset.state);
    });
  });

  overlayState = { id: t.id, lng, lat, color: meta.color };
  const panel = document.getElementById('overlay-panel');
  if (panel) panel.style.display = 'block';
  positionMapOverlay();
}

function positionMapOverlay() {
  if (!overlayState) return;
  const panel = document.getElementById('overlay-panel');
  const svg   = document.getElementById('overlay-svg');
  if (!panel || !svg || panel.style.display === 'none') return;

  const display = displayPositions.get(overlayState.id);
  const lng = display ? display.lng : overlayState.lng;
  const lat = display ? display.lat : overlayState.lat;
  const pt  = map.project([lng, lat]);

  const PW  = panel.offsetWidth  || 272;
  const PH  = panel.offsetHeight || 200;
  const GAP = 72;
  const mc  = document.getElementById('map-container');
  const MCW = mc.offsetWidth;
  const MCH = mc.offsetHeight;

  const color  = overlayState.color || '#226FEE';
  const tx     = pt.x;
  const ty     = pt.y;

  // Diagonal segment length (~45° / 2 o'clock), then horizontal right to panel
  const DIAG   = 72;
  const d      = DIAG / Math.SQRT2;   // ≈ 51px each axis

  // Panel positioned so its bottom aligns with the joint y
  let px = tx + GAP + 100;
  let py = ty - d - PH;
  px = Math.max(4, Math.min(px, MCW - PW - 4));
  py = Math.max(4, Math.min(py, MCH - PH - 4));

  panel.style.left = `${px}px`;
  panel.style.top  = `${py}px`;

  const bx  = px;        // panel bottom-left x
  const by  = py + PH;   // panel bottom y
  // Joint: same y as panel bottom; x offset equals vertical rise → 45°
  const jx  = tx + (ty - by);
  const jy  = by;

  // Path: diagonal up-right from target center → joint → horizontal right to panel bottom-left
  const stemPath = `M ${tx} ${ty} L ${jx} ${jy} L ${bx} ${jy}`;

  // Perpendicular tick at stem start (rotated 90° from 45° diagonal = ±135° direction)
  const tk  = 10;
  const tkx = tk / Math.SQRT2;   // ≈ 7.1
  const tky = tk / Math.SQRT2;

  svg.innerHTML = `
    <defs>
      <filter id="ol-glow" x="-80%" y="-80%" width="260%" height="260%">
        <feGaussianBlur in="SourceGraphic" stdDeviation="3.5" result="b"/>
        <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
      </filter>
    </defs>
    <!-- Stem: outer halo -->
    <path d="${stemPath}" fill="none" stroke="${color}" stroke-width="10"
          opacity="0.10" stroke-linecap="square" stroke-linejoin="miter"/>
    <!-- Stem: mid glow -->
    <path d="${stemPath}" fill="none" stroke="${color}" stroke-width="5"
          opacity="0.20" stroke-linecap="square" stroke-linejoin="miter"/>
    <!-- Stem: core -->
    <path d="${stemPath}" fill="none" stroke="${color}" stroke-width="2"
          opacity="0.95" stroke-linecap="square" stroke-linejoin="miter"
          filter="url(#ol-glow)"/>
    <!-- Target-end node -->
    <circle cx="${tx}" cy="${ty}" r="4.5" fill="${color}" opacity="0.95" filter="url(#ol-glow)"/>
    <circle cx="${tx}" cy="${ty}" r="2"   fill="#fff"     opacity="0.6"/>
    <!-- Perpendicular tick at stem start (45° rotated) -->
    <line x1="${tx - tkx}" y1="${ty - tky}" x2="${tx + tkx}" y2="${ty + tky}"
          stroke="${color}" stroke-width="2.5" opacity="0.95" stroke-linecap="round"
          filter="url(#ol-glow)"/>
    <!-- Joint node (diagonal → horizontal turn) -->
    <circle cx="${jx}" cy="${jy}" r="4" fill="${color}" opacity="0.9" filter="url(#ol-glow)"/>
    <circle cx="${jx}" cy="${jy}" r="2" fill="#fff"     opacity="0.6"/>
    <!-- Panel bottom-left attach node -->
    <circle cx="${bx}" cy="${by}" r="5"   fill="${color}" opacity="0.95" filter="url(#ol-glow)"/>
    <circle cx="${bx}" cy="${by}" r="2.5" fill="#fff"    opacity="0.6"/>
    <!-- Outer reticle ring (dashed) -->
    <circle cx="${tx}" cy="${ty}" r="22" fill="none" stroke="${color}"
            stroke-width="1" stroke-dasharray="5 4" opacity="0.5"/>
    <!-- Inner reticle ring -->
    <circle cx="${tx}" cy="${ty}" r="11" fill="none" stroke="${color}"
            stroke-width="1" opacity="0.35"/>
    <!-- Cardinal tick marks -->
    <line x1="${tx}"      y1="${ty - 30}" x2="${tx}"      y2="${ty - 24}" stroke="${color}" stroke-width="1.5" opacity="0.75"/>
    <line x1="${tx}"      y1="${ty + 24}" x2="${tx}"      y2="${ty + 30}" stroke="${color}" stroke-width="1.5" opacity="0.75"/>
    <line x1="${tx - 30}" y1="${ty}"      x2="${tx - 24}" y2="${ty}"      stroke="${color}" stroke-width="1.5" opacity="0.75"/>
    <line x1="${tx + 24}" y1="${ty}"      x2="${tx + 30}" y2="${ty}"      stroke="${color}" stroke-width="1.5" opacity="0.75"/>`;
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
// RTMS frame buffer + bbox crop
// ---------------------------------------------------------------------------

function captureFrame(pts_ns, camIdx) {
  const cvs = camIdx === 1
    ? document.getElementById('pip-canvas-2')
    : document.getElementById('pip-canvas');
  if (!cvs || !cvs.width || !cvs.height) return;
  const snap = document.createElement('canvas');
  snap.width  = cvs.width;
  snap.height = cvs.height;
  snap.getContext('2d').drawImage(cvs, 0, 0);
  const buf = frameBuffers[camIdx];
  buf.push({ pts_ns, snap });
  if (buf.length > FRAME_BUFFER_SIZE) buf.shift();
}

// Returns the buffered frame with the closest pts_ns, or null if buffer is empty.
function findFrame(pts_ns, camIdx = activeCam) {
  const buf = frameBuffers[camIdx] || frameBuffers[0];
  if (!buf.length) return null;
  let best = buf[0];
  let bestDiff = Math.abs(buf[0].pts_ns - pts_ns);
  for (let i = 1; i < buf.length; i++) {
    const diff = Math.abs(buf[i].pts_ns - pts_ns);
    if (diff < bestDiff) { bestDiff = diff; best = buf[i]; }
  }
  return best;
}

function cropBboxFromCanvas(srcCanvas, bx, by, bw, bh) {
  if (!srcCanvas || bw <= 0 || bh <= 0) return null;
  const scaleX = srcCanvas.width  / 1280;
  const scaleY = srcCanvas.height / 720;
  const sx = Math.max(0, Math.round(bx * scaleX));
  const sy = Math.max(0, Math.round(by * scaleY));
  const sw = Math.max(1, Math.min(Math.round(bw * scaleX), srcCanvas.width  - sx));
  const sh = Math.max(1, Math.min(Math.round(bh * scaleY), srcCanvas.height - sy));
  const off = document.createElement('canvas');
  off.width = sw; off.height = sh;
  off.getContext('2d').drawImage(srcCanvas, sx, sy, sw, sh, 0, 0, sw, sh);
  try { return off.toDataURL('image/jpeg', 0.82); } catch { return null; }
}

// Decode proto target id: ((category+1) << 8) | track_number. C++ side adds
// +1 to the class lane so the packed id is never 0 (proto3 omits zero-valued
// scalars on the wire). Subtract it back here to land on the COCO index.
function decodeProtoId(id) {
  const n = Number(id);
  if (!Number.isFinite(n) || n < 0) return null;
  const classId  = ((n >> 8) & 0xFF) - 1;
  const objectId = n & 0xFF;
  const label = COCO_CLASSES[classId] ?? `class_${classId}`;
  return { classId, objectId, label };
}


function handleFrameDetection(data) {
  if (!data) return;
  console.log('frame_detection', data);
  const camIdx = (data.src === 1) ? 1 : 0;
  const frame = findFrame(data.pts_ns, camIdx);
  if (!frame) {
    console.error('handleFrameDetection: frame buffer empty for cam' + camIdx + ' (pts_ns=' + data.pts_ns + ')');
    return;
  }

  let updated = false;
  for (const det of (data.targets || [])) {
    const [bx, by, bw, bh] = det.bbox;
    const img = cropBboxFromCanvas(frame.snap, bx, by, bw, bh);
    if (!img) continue;
    const targetId = String(det.id);
    targetImages.set(targetId, img);
    const decoded = decodeProtoId(targetId);
    if (decoded) targetLabels.set(targetId, decoded);
    updated = true;
  }
  if (updated) renderList();
}

// ---------------------------------------------------------------------------
// RTMS PiP viewer (dual-cam)
// ---------------------------------------------------------------------------

(function initPip() {
  const CAM_WS_URLS = [
    `ws://${location.hostname}:9999`,
    `ws://${location.hostname}:9998`,
  ];
  const CAM_LABELS = ['CAM 1', 'CAM 2'];

  const container = document.getElementById('pip-container');
  const canvases  = [
    document.getElementById('pip-canvas'),
    document.getElementById('pip-canvas-2'),
  ];
  rtmsCanvas = canvases[0];

  const statusEl  = document.getElementById('pip-status');
  const titleEl   = document.getElementById('pip-title');
  const toggleBtn = document.getElementById('pip-toggle');

  let collapsed = false;

  function setPipStatus(state) {
    statusEl.className = `pip-status-${state}`;
    statusEl.textContent = { live: 'LIVE', connecting: 'CONNECTING', error: 'ERROR' }[state];
  }

  function switchCam(idx) {
    activeCam = idx;
    frameBuffer = frameBuffers[idx];
    rtmsCanvas  = canvases[idx];
    canvases.forEach((c, i) => { c.style.display = i === idx ? '' : 'none'; });
    titleEl.textContent = CAM_LABELS[idx];
    document.querySelectorAll('.cam-btn').forEach(b => {
      b.classList.toggle('active', Number(b.dataset.cam) === idx);
    });
    // Status reflects the active cam; force connecting briefly to give JSMpeg time to report
    setPipStatus('connecting');
  }

  setPipStatus('connecting');

  // Both players run continuously — only the active canvas is visible.
  // decoder.currentTime = source PTS (seconds) when ffmpeg -copyts -fps_mode passthrough is used.
  canvases.forEach((canvas, i) => {
    new JSMpeg.Player(CAM_WS_URLS[i], {
      canvas,
      autoplay: true,
      audio: false,
      disableGl: true,
      reconnectInterval: 5,
      onSourceEstablished: () => { if (i === activeCam) setPipStatus('live'); },
      onSourceCompleted:   () => { if (i === activeCam) setPipStatus('connecting'); },
      onVideoDecode: (decoder) => {
        const t = Number(decoder && decoder.currentTime);
        if (!Number.isFinite(t)) return;
        captureFrame(Math.round(t * 1e9), i);
      },
    });
  });

  document.querySelectorAll('.cam-btn').forEach(btn => {
    btn.addEventListener('click', () => switchCam(Number(btn.dataset.cam)));
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
