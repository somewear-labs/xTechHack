const express = require('express');
const path = require('path');
const { spawn } = require('child_process');
const http = require('http');
const WebSocket = require('ws');

const app = express();
const PORT = process.env.PORT || 3000;
const RTSP_WS_PORT = process.env.RTSP_WS_PORT || 9999;

const RTSP_URLS = [
  process.env.RTSP_URL   || 'rtsp://100.68.91.72:9554/ds-test',
  process.env.RTSP_URL_2 || 'rtsp://100.68.91.72:9555/ds-test',
];

app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json());

app.post('/emit', (req, res) => {
  const extraArgs = req.body?.id ? [`--id=${req.body.id}`] : [];
  const child = spawn('node', [path.join(__dirname, 'emit-target.js'), '1', ...extraArgs], { cwd: __dirname });
  child.stdout.on('data', (d) => process.stdout.write(d));
  child.stderr.on('data', (d) => process.stderr.write(d));
  child.on('close', (code) => {
    res.json({ ok: code === 0 });
  });
});

app.listen(PORT, () => {
  console.log(`Client server running at http://localhost:${PORT}`);
});

// ---------------------------------------------------------------------------
// RTSP → MPEG1 WebSocket relay — single port, path-based routing
// ws://host:9999/0  → RTSP_URL   (cam 1, :9554)
// ws://host:9999/1  → RTSP_URL_2 (cam 2, :9555)
// ---------------------------------------------------------------------------

function makeFfmpegArgs(rtspUrl) {
  return [
    '-rtsp_transport', 'tcp',
    '-stimeout', '5000000',
    '-reconnect', '1',
    '-reconnect_streamed', '1',
    '-reconnect_delay_max', '2',
    '-thread_queue_size', '64',
    '-i', rtspUrl,
    '-fps_mode', 'passthrough',
    '-copyts',
    '-f', 'mpegts',
    '-codec:v', 'mpeg1video',
    '-b:v', '1000k',
    '-r', '25',
    '-vf', 'scale=640:360',
    '-bf', '0',
    'pipe:1',
  ];
}

const rtspServer = http.createServer();
const wss = new WebSocket.Server({ noServer: true });

// Per-cam state
const cams = RTSP_URLS.map((url, i) => ({
  url,
  idx: i,
  ffmpeg: null,
  clients: new Set(),
}));

function startFfmpeg(cam) {
  if (cam.ffmpeg) return;
  console.log(`[rtsp${cam.idx}] Starting ffmpeg for ${cam.url}`);
  cam.ffmpeg = spawn('ffmpeg', makeFfmpegArgs(cam.url), { stdio: ['ignore', 'pipe', 'pipe'] });

  cam.ffmpeg.on('error', (err) => {
    console.error(`[rtsp${cam.idx}] ffmpeg error: ${err.message}`);
    cam.ffmpeg = null;
    if (cam.clients.size > 0) setTimeout(() => startFfmpeg(cam), 2000);
  });
  cam.ffmpeg.stderr.on('data', (d) => {
    if (process.env.RTSP_DEBUG) process.stderr.write(d);
  });
  cam.ffmpeg.stdout.on('data', (data) => {
    cam.clients.forEach((ws) => {
      if (ws.readyState === WebSocket.OPEN) {
        try { ws.send(data); } catch (_) {}
      }
    });
  });
  cam.ffmpeg.on('close', (code) => {
    console.log(`[rtsp${cam.idx}] ffmpeg exited (${code})`);
    cam.ffmpeg = null;
    if (cam.clients.size > 0) {
      console.log(`[rtsp${cam.idx}] Restarting ffmpeg in 2s...`);
      setTimeout(() => startFfmpeg(cam), 2000);
    }
  });
}

function stopFfmpeg(cam) {
  if (cam.ffmpeg) {
    cam.ffmpeg.kill('SIGTERM');
    cam.ffmpeg = null;
  }
}

rtspServer.on('upgrade', (request, socket, head) => {
  const camIdx = parseInt(request.url.replace(/^\//, ''), 10);
  const cam = cams[camIdx];
  if (!cam) {
    socket.destroy();
    return;
  }
  wss.handleUpgrade(request, socket, head, (ws) => {
    cam.clients.add(ws);
    console.log(`[rtsp${camIdx}] Client connected (${cam.clients.size} total)`);
    if (cam.clients.size === 1) startFfmpeg(cam);

    ws.on('close', () => {
      cam.clients.delete(ws);
      console.log(`[rtsp${camIdx}] Client disconnected (${cam.clients.size} remaining)`);
      if (cam.clients.size === 0) stopFfmpeg(cam);
    });
  });
});

rtspServer.listen(RTSP_WS_PORT, () => {
  console.log(`RTSP relay WebSocket listening on ws://localhost:${RTSP_WS_PORT}/{0,1}`);
});
