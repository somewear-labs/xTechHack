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

// Matches Somewear color palette from colors.xml / Colors.kt
const STATE_COLORS = {
  TARGET_STATE_ACTIVE:      '#226FEE',  // primaryAccent
  TARGET_STATE_ACQUIRED:    '#1EB982',  // tertiaryAccent
  TARGET_STATE_INACTIVE:    '#9B9B9B',  // tertiaryOnSurface
  TARGET_STATE_LOST:        '#E4591D',  // error
  TARGET_STATE_NEUTRALIZED: '#E4591D',  // error
  TARGET_STATE_UNKNOWN:     '#5F666C',  // secondaryOnSurface
};

const STATE_LABELS = {
  TARGET_STATE_ACTIVE:      'ACTIVE',
  TARGET_STATE_ACQUIRED:    'ACQUIRED',
  TARGET_STATE_INACTIVE:    'INACTIVE',
  TARGET_STATE_LOST:        'LOST',
  TARGET_STATE_NEUTRALIZED: 'NEUTRALIZED',
  TARGET_STATE_UNKNOWN:     'UNKNOWN',
};

const SOURCE_ID = 'targets-source';
const LAYER_CIRCLE = 'targets-circle';
const LAYER_PULSE  = 'targets-pulse';
const LAYER_LABEL  = 'targets-label';

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let targets = {};           // id → target
let selectedId = null;
let currentStyle = 'dark-topo';
let popup = null;
let map;
let ws;
let reconnectTimeout;

// ---------------------------------------------------------------------------
// Map init
// ---------------------------------------------------------------------------

mapboxgl.accessToken = MAPBOX_TOKEN;

map = new mapboxgl.Map({
  container: 'map',
  style: MAP_STYLES['dark-topo'],
  center: [-98.5795, 39.8283],
  zoom: 3,
  pitch: 40,
  bearing: 0,
  antialias: true,
});

map.addControl(new mapboxgl.NavigationControl({ visualizePitch: true }), 'top-left');

map.on('load', () => {
  addTerrainAndSky();
  initTargetLayers();
  connectWebSocket();
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
  map.addSource(SOURCE_ID, {
    type: 'geojson',
    data: emptyFeatureCollection(),
  });

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
      'circle-radius': [
        'case',
        ['boolean', ['feature-state', 'selected'], false], 11,
        9,
      ],
      'circle-color': stateColorExpression(),
      'circle-stroke-width': [
        'case',
        ['boolean', ['feature-state', 'selected'], false], 2,
        1.5,
      ],
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

  // Click handler
  map.on('click', LAYER_CIRCLE, (e) => {
    const id = e.features[0].properties.id;
    selectTarget(id, true);
  });

  map.on('mouseenter', LAYER_CIRCLE, () => {
    map.getCanvas().style.cursor = 'pointer';
  });
  map.on('mouseleave', LAYER_CIRCLE, () => {
    map.getCanvas().style.cursor = '';
  });
}

function stateColorExpression() {
  return [
    'match',
    ['get', 'state'],
    'TARGET_STATE_ACTIVE',      '#226FEE',
    'TARGET_STATE_ACQUIRED',    '#1EB982',
    'TARGET_STATE_INACTIVE',    '#9B9B9B',
    'TARGET_STATE_LOST',        '#E4591D',
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
  const loc = t.tracking_location || {};
  const lng = (loc.longitude  || 0) / 1e7;
  const lat = (loc.latitude   || 0) / 1e7;
  // Prefer label (set by Beam ingest) over truncated UUID
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
  const features = Object.values(targets).map(targetToFeature);
  src.setData({ type: 'FeatureCollection', features });
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
    ws.send(JSON.stringify({ action: 'list', payload: {} }));
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
  // Response to our requests
  if (msg.status === 'success' && msg.action === 'list') {
    targets = {};
    (msg.data || []).forEach(t => { targets[t.id] = t; });
    rebuildSource();
    renderList();
    return;
  }

  // Broadcast events
  if (msg.event === 'target_created' || msg.event === 'target_updated') {
    targets[msg.data.id] = msg.data;
    rebuildSource();
    renderList();
    if (selectedId === msg.data.id) renderPopup(msg.data);
    return;
  }

  if (msg.event === 'target_deleted') {
    const id = msg.data.id;
    delete targets[id];
    if (selectedId === id) {
      selectedId = null;
      if (popup) { popup.remove(); popup = null; }
    }
    rebuildSource();
    renderList();
  }
}

// ---------------------------------------------------------------------------
// Sidebar list
// ---------------------------------------------------------------------------

function renderList() {
  const container = document.getElementById('target-list');
  const list = Object.values(targets);

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
    const color = STATE_COLORS[t.state] || STATE_COLORS.TARGET_STATE_UNKNOWN;
    const stateLabel = STATE_LABELS[t.state] || t.state;
    const displayName = t.label || (t.id ? t.id.split('-')[0].toUpperCase() : '???');
    const updated = formatAge((t.updated_date || {}).seconds || 0);
    const sel = t.id === selectedId ? ' selected' : '';
    return `
      <div class="target-card${sel}" data-id="${t.id}">
        <div class="target-dot" style="background:${color}"></div>
        <div class="target-info">
          <div class="target-id">${displayName}</div>
          <div class="target-state" style="color:${color}">${stateLabel}</div>
          <div class="target-coords">${lat}, ${lng}</div>
        </div>
        <div class="target-updated">${updated}</div>
      </div>`;
  }).join('');

  container.querySelectorAll('.target-card').forEach(card => {
    card.addEventListener('click', () => selectTarget(card.dataset.id, true));
  });
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
  // Clear previous selection feature-state
  if (selectedId && selectedId !== id) {
    map.setFeatureState({ source: SOURCE_ID, id: selectedId }, { selected: false });
  }

  selectedId = id;
  map.setFeatureState({ source: SOURCE_ID, id }, { selected: true });

  renderList();

  const t = targets[id];
  if (!t) return;

  if (flyTo) {
    const loc = t.tracking_location || {};
    const lng = (loc.longitude || 0) / 1e7;
    const lat = (loc.latitude  || 0) / 1e7;
    if (lng !== 0 || lat !== 0) {
      map.flyTo({ center: [lng, lat], zoom: Math.max(map.getZoom(), 8), duration: 800 });
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

  const color = STATE_COLORS[t.state] || STATE_COLORS.TARGET_STATE_UNKNOWN;
  const label = STATE_LABELS[t.state] || t.state;
  const altM  = ((loc.altitude || 0) / 1000).toFixed(0);
  const speedKph = ((loc.speed_over_ground || 0) * 0.0036).toFixed(1);
  const courseRaw = (loc.course_over_ground || 0) / 1000;
  const updated = t.updated_date
    ? new Date((t.updated_date.seconds || 0) * 1000).toLocaleTimeString()
    : '--';

  const html = `
    <div class="popup-target-id">${t.id}</div>
    <div class="popup-row">
      <span class="popup-label">STATE</span>
      <span class="popup-value" style="color:${color}">${label}</span>
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
      <span class="popup-value">${courseRaw.toFixed(0)}°</span>
    </div>
    <div class="popup-row">
      <span class="popup-label">UPDATED</span>
      <span class="popup-value">${updated}</span>
    </div>`;

  popup = new mapboxgl.Popup({ closeButton: true, maxWidth: '260px', offset: 14 })
    .setLngLat([lng, lat])
    .setHTML(html)
    .addTo(map);

  popup.on('close', () => {
    if (selectedId) {
      map.setFeatureState({ source: SOURCE_ID, id: selectedId }, { selected: false });
    }
    selectedId = null;
    renderList();
  });
}

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

    // Re-add layers after style loads
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
