#!/usr/bin/env node
'use strict';

/**
 * Emit a mock TargetResponse proto as a Beam Message to the webhook handler.
 *
 * Usage:
 *   node emit-target.js          # emit one target
 *   node emit-target.js 5        # emit five targets with 200ms spacing
 *
 * The target is serialized as a TargetResponse protobuf, base64-encoded,
 * and sent as the content of a Beam Message event to http://localhost:8080/beam.
 */

const protobuf = require('protobufjs');
const http = require('http');
const crypto = require('crypto');
const path = require('path');

// Shack15 coworking, 99 Green St, San Francisco (near Embarcadero)
const SHACK15_LAT = 37.7993;
const SHACK15_LNG = -122.3983;
const SCATTER_RADIUS_M = 400;
const BEAM_HOST = 'localhost';
const BEAM_PORT = 8080;
const WORKSPACE_ID = 'xtech-hackathon';

function randomNearShack15() {
  const latDeg = SCATTER_RADIUS_M / 111000;
  const lngDeg = SCATTER_RADIUS_M / (111000 * Math.cos(SHACK15_LAT * Math.PI / 180));
  const angle = Math.random() * 2 * Math.PI;
  const r = Math.sqrt(Math.random()); // uniform disk distribution
  return {
    lat: SHACK15_LAT + r * latDeg * Math.cos(angle),
    lng: SHACK15_LNG + r * lngDeg * Math.sin(angle),
  };
}

function postJson(body) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(body);
    const req = http.request(
      {
        host: BEAM_HOST,
        port: BEAM_PORT,
        path: '/beam',
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(data),
        },
      },
      (res) => {
        let buf = '';
        res.on('data', (chunk) => (buf += chunk));
        res.on('end', () => resolve({ status: res.statusCode, body: buf }));
      }
    );
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

async function loadProto() {
  const root = await protobuf.load(path.join(__dirname, '../wss/target_proto.proto'));
  return root.lookupType('TargetResponse');
}

async function emitTarget(TargetResponse, fixedId) {
  const { lat, lng } = randomNearShack15();
  const now = Math.floor(Date.now() / 1000);
  const targetId = fixedId || crypto.randomUUID();

  const payload = {
    id: targetId,
    updatedDate: { seconds: now, nanos: 0 },
    trackingLocation: {
      longitude:       Math.round(lng * 1e7),
      latitude:        Math.round(lat * 1e7),
      timestamp:       now,
      altitude:        0,
      speedOverGround:  0,
      courseOverGround: 0,
    },
    state:       1, // TARGET_STATE_ACTIVE
    workspaceId: WORKSPACE_ID,
  };

  const verifyErr = TargetResponse.verify(payload);
  if (verifyErr) throw new Error(`Proto verify: ${verifyErr}`);

  const buffer = TargetResponse.encode(TargetResponse.create(payload)).finish();
  const base64Content = buffer.toString('base64');

  console.log(`→ target  ${targetId}`);
  console.log(`  location ${lat.toFixed(6)}° N, ${Math.abs(lng).toFixed(6)}° W`);
  console.log(`  proto    ${buffer.length} bytes → ${base64Content.length} char base64`);

  const beamPayload = {
    requestId: `node-emit-${Date.now()}`,
    payloads: [
      {
        identity: { id: 'node-emitter', name: 'Shack15 Sim', type: 'device', email: '' },
        account:  { id: WORKSPACE_ID },
        events: [
          {
            type:      'Message',
            content:   base64Content,
            timestamp: new Date().toISOString(),
          },
        ],
      },
    ],
  };

  const { status, body } = await postJson(beamPayload);
  console.log(`  webhook  HTTP ${status} ${body.trim()}`);
}

(async () => {
  const args = process.argv.slice(2);
  const countArg = args.find(a => !a.startsWith('--'));
  const count = Math.max(1, parseInt(countArg || '1', 10));
  const idFlag = args.find(a => a.startsWith('--id='));
  const fixedId = idFlag ? idFlag.slice(5) : null;

  const TargetResponse = await loadProto();

  for (let i = 0; i < count; i++) {
    await emitTarget(TargetResponse, fixedId);
    if (i < count - 1) await new Promise((r) => setTimeout(r, 200));
  }
})().catch((err) => {
  console.error('Error:', err.message);
  process.exit(1);
});
