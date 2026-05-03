const express = require('express');
const path = require('path');
const { spawn } = require('child_process');

const app = express();
const PORT = process.env.PORT || 3000;

app.use(express.static(path.join(__dirname, 'public')));

app.post('/emit', (_req, res) => {
  const child = spawn('node', [path.join(__dirname, 'emit-target.js'), '1'], { cwd: __dirname });
  child.stdout.on('data', (d) => process.stdout.write(d));
  child.stderr.on('data', (d) => process.stderr.write(d));
  child.on('close', (code) => {
    res.json({ ok: code === 0 });
  });
});

app.listen(PORT, () => {
  console.log(`Client server running at http://localhost:${PORT}`);
});
