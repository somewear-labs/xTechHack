const express = require('express');
const path = require('path');
const { spawn } = require('child_process');
const http = require('http');
const WebSocket = require('ws');

const app = express();
const PORT = process.env.PORT || 3000;
const RTSP_WS_PORT = process.env.RTSP_WS_PORT || 9999;

const RTSP_URL   = process.env.RTSP_URL   || 'rtsp://100.68.81.224:9554/ds-test';
const RTSP_URL_2 = process.env.RTSP_URL_2 || 'rtsp://100.68.81.224:9555/ds-test';
const RTSP_WS_PORT_2 = process.env.RTSP_WS_PORT_2 || 9998;

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
// RTSP → MPEG1 WebSocket relay
// ---------------------------------------------------------------------------

const rtspServer = http.createServer();
const wss = new WebSocket.Server({ server: rtspServer });

let ffmpeg = null;
let clientCount = 0;

function startFfmpeg() {
  if (ffmpeg) return;

  console.log(`[rtsp] Starting ffmpeg for ${RTSP_URL}`);
  ffmpeg = spawn('ffmpeg', [
    // === input options (mpv-equivalent flags via libavformat) ===
    // mpv --rtsp-transport=tcp
    '-rtsp_transport', 'tcp',
    // mpv --demuxer-lavf-o=stimeout=5000000   (socket I/O timeout in μs; 5s)
    '-stimeout', '5000000',
    // mpv --demuxer-lavf-o=reconnect=1,reconnect_streamed=1,reconnect_delay_max=2
    '-reconnect', '1',
    '-reconnect_streamed', '1',
    '-reconnect_delay_max', '2',
    // Small input thread queue — matches mpv --cache-secs=2 spirit (don't buffer
    // a huge backlog when source is briefly slow; keep latency low).
    '-thread_queue_size', '64',
    '-i', RTSP_URL,
    // Preserve source PTS through the relay so the client can sync WSS bbox
    // events (carry source `buf_pts` as `pts_ns`) to the matching decoded
    // video frame. Without these flags ffmpeg rebases output PTS to start
    // at 0 and the client falls back to oldest-frame thumbnail crops.
    '-fps_mode', 'passthrough',
    '-copyts',
    // === output: MPEG-1 over MPEG-TS to the WS relay ===
    '-f', 'mpegts',
    '-codec:v', 'mpeg1video',
    '-b:v', '1000k',
    '-r', '25',
    '-vf', 'scale=640:360',
    '-bf', '0',
    'pipe:1',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  ffmpeg.stderr.on('data', (d) => {
    if (process.env.RTSP_DEBUG) process.stderr.write(d);
  });

  ffmpeg.stdout.on('data', (data) => {
    wss.clients.forEach((client) => {
      if (client.readyState === WebSocket.OPEN) {
        try { client.send(data); } catch (_) {}
      }
    });
  });

  ffmpeg.on('close', (code) => {
    console.log(`[rtsp] ffmpeg exited (${code})`);
    ffmpeg = null;
    if (clientCount > 0) {
      console.log('[rtsp] Restarting ffmpeg in 2s...');
      setTimeout(startFfmpeg, 2000);
    }
  });
}

function stopFfmpeg() {
  if (ffmpeg) {
    console.log('[rtsp] Stopping ffmpeg');
    ffmpeg.kill('SIGTERM');
    ffmpeg = null;
  }
}

wss.on('connection', (ws) => {
  clientCount++;
  console.log(`[rtsp] Client connected (${clientCount} total)`);
  if (clientCount === 1) startFfmpeg();

  ws.on('close', () => {
    clientCount--;
    console.log(`[rtsp] Client disconnected (${clientCount} remaining)`);
    if (clientCount === 0) stopFfmpeg();
  });
});

rtspServer.listen(RTSP_WS_PORT, () => {
  console.log(`RTSP relay WebSocket listening on ws://localhost:${RTSP_WS_PORT}`);
});

// ---------------------------------------------------------------------------
// Cam 2 relay (port 9998 → :9555)
// ---------------------------------------------------------------------------

const rtspServer2 = http.createServer();
const wss2 = new WebSocket.Server({ server: rtspServer2 });

let ffmpeg2 = null;
let clientCount2 = 0;

function startFfmpeg2() {
  if (ffmpeg2) return;
  console.log(`[rtsp2] Starting ffmpeg for ${RTSP_URL_2}`);
  ffmpeg2 = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-stimeout', '5000000',
    '-reconnect', '1',
    '-reconnect_streamed', '1',
    '-reconnect_delay_max', '2',
    '-thread_queue_size', '64',
    '-i', RTSP_URL_2,
    '-f', 'mpegts',
    '-codec:v', 'mpeg1video',
    '-b:v', '1000k',
    '-r', '25',
    '-vf', 'scale=640:360',
    '-bf', '0',
    'pipe:1',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  ffmpeg2.stderr.on('data', (d) => {
    if (process.env.RTSP_DEBUG) process.stderr.write(d);
  });
  ffmpeg2.stdout.on('data', (data) => {
    wss2.clients.forEach((client) => {
      if (client.readyState === WebSocket.OPEN) {
        try { client.send(data); } catch (_) {}
      }
    });
  });
  ffmpeg2.on('close', (code) => {
    console.log(`[rtsp2] ffmpeg exited (${code})`);
    ffmpeg2 = null;
    if (clientCount2 > 0) {
      console.log('[rtsp2] Restarting ffmpeg in 2s...');
      setTimeout(startFfmpeg2, 2000);
    }
  });
}

function stopFfmpeg2() {
  if (ffmpeg2) { ffmpeg2.kill('SIGTERM'); ffmpeg2 = null; }
}

wss2.on('connection', (ws) => {
  clientCount2++;
  console.log(`[rtsp2] Client connected (${clientCount2} total)`);
  if (clientCount2 === 1) startFfmpeg2();
  ws.on('close', () => {
    clientCount2--;
    console.log(`[rtsp2] Client disconnected (${clientCount2} remaining)`);
    if (clientCount2 === 0) stopFfmpeg2();
  });
});

rtspServer2.listen(RTSP_WS_PORT_2, () => {
  console.log(`RTSP relay 2 WebSocket listening on ws://localhost:${RTSP_WS_PORT_2}`);
});
