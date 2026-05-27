const express = require('express');
const path = require('path');
const { spawn } = require('child_process');
const http = require('http');
const WebSocket = require('ws');

const app = express();
const PORT = process.env.PORT || 3000;

const RTSP_URL   = process.env.RTSP_URL   || 'rtsp://100.68.91.72:9554/ds-test';
const RTSP_URL_2 = process.env.RTSP_URL_2 || 'rtsp://100.68.91.72:9555/ds-test';
const RTSP_WS_PORT   = process.env.RTSP_WS_PORT   || 9999;
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

function makeRelay(label, rtspUrl, wsPort) {
  const server = http.createServer();
  const wss = new WebSocket.Server({ server });
  let ffmpeg = null;
  let clients = 0;

  const ffmpegArgs = [
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

  function start() {
    if (ffmpeg) return;
    console.log(`[${label}] Starting ffmpeg → ${rtspUrl}`);
    ffmpeg = spawn('ffmpeg', ffmpegArgs, { stdio: ['ignore', 'pipe', 'pipe'] });

    ffmpeg.on('error', (err) => {
      console.error(`[${label}] ffmpeg spawn error: ${err.message}`);
      ffmpeg = null;
      if (clients > 0) setTimeout(start, 2000);
    });
    ffmpeg.stderr.on('data', (d) => process.stderr.write(d));
    ffmpeg.stdout.on('data', (data) => {
      wss.clients.forEach((ws) => {
        if (ws.readyState === WebSocket.OPEN) {
          try { ws.send(data); } catch (_) {}
        }
      });
    });
    ffmpeg.on('close', (code) => {
      console.log(`[${label}] ffmpeg exited (${code})`);
      ffmpeg = null;
      if (clients > 0) {
        console.log(`[${label}] Restarting in 2s...`);
        setTimeout(start, 2000);
      }
    });
  }

  function stop() {
    if (ffmpeg) { ffmpeg.kill('SIGTERM'); ffmpeg = null; }
  }

  wss.on('connection', (ws) => {
    clients++;
    console.log(`[${label}] WS client connected (${clients} total)`);
    if (clients === 1) start();
    ws.on('close', () => {
      clients--;
      console.log(`[${label}] WS client disconnected (${clients} remaining)`);
      if (clients === 0) stop();
    });
  });

  server.listen(wsPort, () => {
    console.log(`[${label}] WebSocket relay on ws://localhost:${wsPort} → ${rtspUrl}`);
  });
}

makeRelay('cam0', RTSP_URL,   RTSP_WS_PORT);
makeRelay('cam1', RTSP_URL_2, RTSP_WS_PORT_2);
