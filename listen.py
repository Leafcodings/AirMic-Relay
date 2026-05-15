#!/usr/bin/env python3
import argparse
import base64
import datetime as dt
import hashlib
import json
import os
import re
import selectors
import socket
import struct
import sys
import time
import urllib.parse

MAGIC = b"AUD0"
HEADER = struct.Struct("<4sHHBBh")
CODEC_PCM16 = 0
CODEC_IMA_ADPCM = 1
RECORDING_NAME = re.compile(r"^\d{4}-\d{2}-\d{2}\.wav$")

IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

WEB_PAGE = """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>LeOSListener</title>
  <style>
    :root{color-scheme:light;--bg:#f4f6f8;--panel:#fff;--ink:#1f2937;--muted:#667085;--line:#d7dde5;--accent:#1167d8;--ok:#0f8a5f;--bad:#b42318}
    *{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:linear-gradient(180deg,#eef4fb 0,#f7f8fa 44%,#f4f6f8 100%);color:var(--ink)}
    main{width:min(720px,100%);margin:0 auto;padding:28px 16px}.brand{font-size:30px;font-weight:780}.sub{color:var(--muted);margin-top:5px}
    .panel{background:rgba(255,255,255,.95);border:1px solid var(--line);border-radius:8px;box-shadow:0 14px 38px rgba(31,41,55,.08);padding:18px;margin-top:18px}
    .status{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}.tile{border:1px solid #e4e8ee;border-radius:8px;padding:12px;background:#fbfcfd}.key{font-size:12px;color:var(--muted);text-transform:uppercase}.val{font-weight:740;margin-top:5px;overflow-wrap:anywhere}
    button,.button,select{border:0;border-radius:8px;padding:12px 16px;background:var(--accent);color:white;font-size:16px;font-weight:740;text-decoration:none;display:inline-flex;align-items:center}
    select{appearance:none;background:#eef2f7;color:#1f2937;border:1px solid #d7dde5;padding-right:34px}
    .secondary{background:#eef2f7;color:#1f2937}
    .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.meter{height:10px;background:#e8edf4;border-radius:999px;overflow:hidden;margin-top:14px}.bar{height:100%;width:0;background:linear-gradient(90deg,#0f8a5f,#1167d8)}
    .recording{border-top:1px solid var(--line);padding:14px 0}.recording:first-child{border-top:0}.rec-head{display:flex;justify-content:space-between;gap:10px;align-items:center;flex-wrap:wrap}.rec-title{font-weight:760}.rec-meta{color:var(--muted);font-size:13px}audio{width:100%;margin-top:10px}.empty{padding:14px;border:1px dashed #c9d1dc;border-radius:8px;color:var(--muted);text-align:center}
    .slider-wrap{display:flex;align-items:center;gap:10px;color:var(--muted);font-size:14px}.slider-wrap input{width:160px}
    .muted{color:var(--muted);font-size:14px}.ok{color:var(--ok)}.bad{color:var(--bad)}@media(max-width:560px){.status{grid-template-columns:1fr}.brand{font-size:26px}}
  </style>
</head>
<body>
  <main>
    <div class="brand">LeOSListener</div>
    <div class="sub">Live ESP32-S3 microphone monitor</div>
    <section class="panel">
      <div class="row">
        <button id="start">Start listening</button>
        <button id="stop" class="secondary">Stop</button>
        <button id="talk" class="secondary">Talk to ESP32</button>
        <select id="mode">
          <option value="live">Live</option>
          <option value="balanced" selected>Balanced</option>
          <option value="stable">Stable</option>
        </select>
        <label class="slider-wrap">Browser gain <input id="gain" type="range" min="0.6" max="2.0" step="0.1" value="1.0"><span id="gainVal">1.0x</span></label>
      </div>
      <div class="meter"><div id="bar" class="bar"></div></div>
      <p id="message" class="muted">Tap Start listening. Keep this page open while monitoring.</p>
    </section>
    <section class="panel status">
      <div class="tile"><div class="key">Connection</div><div id="conn" class="val">idle</div></div>
      <div class="tile"><div class="key">Source</div><div id="source" class="val">unknown</div></div>
      <div class="tile"><div class="key">Audio</div><div id="audio" class="val">waiting</div></div>
      <div class="tile"><div class="key">Latency buffer</div><div id="buffer" class="val">0 ms</div></div>
    </section>
    <section class="panel">
      <div class="row" style="justify-content:space-between">
        <div>
          <div class="key">History</div>
          <div class="val">Daily recordings</div>
        </div>
        <button id="refresh" class="secondary">Refresh</button>
      </div>
      <div id="recordings" style="margin-top:10px"><div class="empty">Loading recordings...</div></div>
    </section>
  </main>
  <script>
    const MAGIC = [65, 85, 68, 48];
    const HEADER_BYTES = 12;
    const CODEC_PCM16 = 0;
    const CODEC_IMA_ADPCM = 1;
    const IMA_INDEX_TABLE = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];
    const IMA_STEP_TABLE = [
      7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
      50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
      337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
      1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
      5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
      18500,20350,22385,24623,27086,29794,32767
    ];
    const MODES = {
      live: {targetMs: 110, maxMs: 220, label: 'lowest delay'},
      balanced: {targetMs: 220, maxMs: 420, label: 'best default'},
      stable: {targetMs: 380, maxMs: 760, label: 'most resistant to jitter'}
    };
    let ws, ctx, gainNode, filterNode, compressorNode, workletNode, scriptNode, stash = new Uint8Array(0), running = false;
    let micStream = null, talkCtx = null, talkSource = null, talkNode = null, talking = false;
    let talkRemainder = new Float32Array(0);
    let streamRate = 16000, underruns = 0, packetCount = 0;
    let fallbackQueue = [], fallbackOffset = 0, fallbackQueuedSamples = 0;
    const conn = document.getElementById('conn'), sourceEl = document.getElementById('source'), audio = document.getElementById('audio'), buffer = document.getElementById('buffer'), msg = document.getElementById('message'), bar = document.getElementById('bar');
    const modeEl = document.getElementById('mode'), gainEl = document.getElementById('gain'), gainVal = document.getElementById('gainVal'), talkBtn = document.getElementById('talk');
    let statusTimer = null;

    function merge(a, b) {
      const out = new Uint8Array(a.length + b.length);
      out.set(a, 0);
      out.set(b, a.length);
      return out;
    }

    function findMagic(buf) {
      for (let i = 0; i <= buf.length - 4; i++) {
        if (buf[i] === 65 && buf[i + 1] === 85 && buf[i + 2] === 68 && buf[i + 3] === 48) return i;
      }
      return -1;
    }

    function profile() {
      return MODES[modeEl.value] || MODES.balanced;
    }

    function updateGain() {
      const value = Number(gainEl.value);
      gainVal.textContent = value.toFixed(1) + 'x';
      if (gainNode) gainNode.gain.value = value;
    }

    function resampleLinear(input, fromRate, toRate) {
      if (fromRate === toRate) return input;
      const outputLength = Math.max(1, Math.round(input.length * toRate / fromRate));
      const output = new Float32Array(outputLength);
      const step = fromRate / toRate;
      for (let i = 0; i < outputLength; i++) {
        const index = i * step;
        const left = Math.floor(index);
        const right = Math.min(left + 1, input.length - 1);
        const frac = index - left;
        output[i] = input[left] * (1 - frac) + input[right] * frac;
      }
      return output;
    }

    function resetFallbackState() {
      fallbackQueue = [];
      fallbackOffset = 0;
      fallbackQueuedSamples = 0;
    }

    function trimFallbackQueue(maxSamples) {
      while (fallbackQueuedSamples > maxSamples && fallbackQueue.length > 1) {
        const chunk = fallbackQueue.shift();
        fallbackQueuedSamples -= chunk.length - fallbackOffset;
        fallbackOffset = 0;
      }
    }

    function createScriptFallback() {
      scriptNode = ctx.createScriptProcessor(1024, 0, 1);
      resetFallbackState();
      scriptNode.onaudioprocess = (event) => {
        const out = event.outputBuffer.getChannelData(0);
        for (let i = 0; i < out.length; i++) {
          if (fallbackQueue.length) {
            const chunk = fallbackQueue[0];
            out[i] = chunk[fallbackOffset++];
            fallbackQueuedSamples--;
            if (fallbackOffset >= chunk.length) {
              fallbackQueue.shift();
              fallbackOffset = 0;
            }
          } else {
            out[i] = 0;
            underruns++;
          }
        }
        buffer.textContent = Math.round(fallbackQueuedSamples * 1000 / ctx.sampleRate) + ' ms';
        if (underruns > 8) msg.textContent = 'Network jitter detected. Try Stable mode.';
      };
      return scriptNode;
    }

    function prefersCompatibilityAudio() {
      const ua = navigator.userAgent || '';
      const isiOS = /iPad|iPhone|iPod/.test(ua) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
      const isMobile = /Android|Mobile/.test(ua) || navigator.maxTouchPoints > 1;
      return isiOS || isMobile;
    }

    function unlockAudioOutput() {
      if (!ctx) return;
      const bufferSource = ctx.createBufferSource();
      bufferSource.buffer = ctx.createBuffer(1, 1, ctx.sampleRate);
      bufferSource.connect(ctx.destination);
      bufferSource.start(0);
    }

    async function createAudioChain(rate) {
      streamRate = rate;
      ctx = new (window.AudioContext || window.webkitAudioContext)({
        latencyHint: 'interactive'
      });
      filterNode = ctx.createBiquadFilter();
      filterNode.type = 'highpass';
      filterNode.frequency.value = 115;
      filterNode.Q.value = 0.7;
      compressorNode = ctx.createDynamicsCompressor();
      compressorNode.threshold.value = -24;
      compressorNode.knee.value = 18;
      compressorNode.ratio.value = 3;
      compressorNode.attack.value = 0.003;
      compressorNode.release.value = 0.12;
      gainNode = ctx.createGain();
      updateGain();

      let usingFallback = false;
      const processorSource = `
        class LeosVoicePlayer extends AudioWorkletProcessor {
          constructor() {
            super();
            this.queue = [];
            this.offset = 0;
            this.queuedSamples = 0;
            this.targetSamples = sampleRate * 0.22;
            this.maxSamples = sampleRate * 0.42;
            this.underruns = 0;
            this.framesSinceReport = 0;
            this.port.onmessage = (event) => {
              const data = event.data || {};
              if (data.type === 'config') {
                this.targetSamples = Math.max(128, Math.round(sampleRate * data.targetMs / 1000));
                this.maxSamples = Math.max(this.targetSamples * 2, Math.round(sampleRate * data.maxMs / 1000));
                this.trim();
              } else if (data.type === 'audio') {
                const chunk = new Float32Array(data.buffer);
                this.queue.push(chunk);
                this.queuedSamples += chunk.length;
                this.trim();
              }
            };
          }
          trim() {
            while (this.queuedSamples > this.maxSamples && this.queue.length > 1) {
              const first = this.queue.shift();
              this.queuedSamples -= first.length - this.offset;
              this.offset = 0;
            }
          }
          process(inputs, outputs) {
            const out = outputs[0][0];
            for (let i = 0; i < out.length; i++) {
              if (this.queue.length) {
                const chunk = this.queue[0];
                out[i] = chunk[this.offset++];
                this.queuedSamples--;
                if (this.offset >= chunk.length) {
                  this.queue.shift();
                  this.offset = 0;
                }
              } else {
                out[i] = 0;
                this.underruns++;
              }
            }
            this.framesSinceReport += out.length;
            if (this.framesSinceReport >= sampleRate / 5) {
              this.port.postMessage({
                queuedMs: Math.round(this.queuedSamples * 1000 / sampleRate),
                underruns: this.underruns
              });
              this.framesSinceReport = 0;
            }
            return true;
          }
        }
        registerProcessor('leos-voice-player', LeosVoicePlayer);
      `;
      try {
        if (prefersCompatibilityAudio()) throw new Error('Mobile compatibility mode');
        if (!ctx.audioWorklet) throw new Error('AudioWorklet unavailable');
        const blob = new Blob([processorSource], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        await ctx.audioWorklet.addModule(url);
        URL.revokeObjectURL(url);
        workletNode = new AudioWorkletNode(ctx, 'leos-voice-player');
        workletNode.connect(filterNode).connect(compressorNode).connect(gainNode).connect(ctx.destination);
        workletNode.port.onmessage = (event) => {
          const data = event.data || {};
          if (typeof data.queuedMs === 'number') buffer.textContent = data.queuedMs + ' ms';
          if (typeof data.underruns === 'number') underruns = data.underruns;
          if (underruns > 8) msg.textContent = 'Network jitter detected. Try Stable mode.';
        };
      } catch (error) {
        usingFallback = true;
        workletNode = null;
        scriptNode = createScriptFallback();
        scriptNode.connect(filterNode).connect(compressorNode).connect(gainNode).connect(ctx.destination);
      }
      applyMode();
      await ctx.resume();
      unlockAudioOutput();
      msg.textContent = usingFallback ? 'Listening with compatibility audio mode.' : 'Listening live in ' + modeEl.value + ' mode.';
    }

    function applyMode() {
      const p = profile();
      if (workletNode) {
        workletNode.port.postMessage({ type: 'config', targetMs: p.targetMs, maxMs: p.maxMs });
      } else {
        trimFallbackQueue(Math.round((p.maxMs / 1000) * (ctx ? ctx.sampleRate : streamRate)));
      }
      audio.textContent = streamRate + ' Hz / ' + p.label + (ctx && ctx.sampleRate !== streamRate ? ' / resampled' : '');
      msg.textContent = 'Playback profile: ' + modeEl.value;
    }

    function queuePcm(pcm, samples, rate) {
      if (!ctx) return;
      const view = new DataView(pcm.buffer, pcm.byteOffset, pcm.byteLength);
      const chunk = new Float32Array(samples);
      let peak = 0;
      for (let i = 0; i < samples; i++) {
        const value = view.getInt16(i * 2, true) / 32768;
        chunk[i] = value;
        peak = Math.max(peak, Math.abs(value));
      }
      const prepared = resampleLinear(chunk, rate, ctx.sampleRate);
      if (workletNode) {
        workletNode.port.postMessage({ type: 'audio', buffer: prepared.buffer }, [prepared.buffer]);
      } else {
        fallbackQueue.push(prepared);
        fallbackQueuedSamples += prepared.length;
        trimFallbackQueue(Math.round((profile().maxMs / 1000) * ctx.sampleRate));
      }
      bar.style.width = Math.min(100, peak * 130) + '%';
      audio.textContent = rate + ' Hz / ' + profile().label + (ctx.sampleRate !== rate ? ' / resampled' : '');
    }

    function payloadBytes(samples, codec) {
      if (codec === CODEC_PCM16) return samples * 2;
      if (codec === CODEC_IMA_ADPCM) return Math.ceil(samples / 2);
      throw new Error('Unknown audio codec ' + codec);
    }

    function decodeImaAdpcm(payload, samples, predictor, stepIndex) {
      predictor = Math.max(-32768, Math.min(32767, predictor | 0));
      stepIndex = Math.max(0, Math.min(88, stepIndex | 0));
      const pcm = new Uint8Array(samples * 2);
      const view = new DataView(pcm.buffer);
      let out = 0;
      for (let i = 0; i < payload.length && out < samples; i++) {
        const byte = payload[i];
        for (const code of [byte & 0x0f, byte >> 4]) {
          if (out >= samples) break;
          const step = IMA_STEP_TABLE[stepIndex];
          let delta = step >> 3;
          if (code & 4) delta += step;
          if (code & 2) delta += step >> 1;
          if (code & 1) delta += step >> 2;
          predictor += (code & 8) ? -delta : delta;
          predictor = Math.max(-32768, Math.min(32767, predictor));
          stepIndex += IMA_INDEX_TABLE[code & 0x0f];
          stepIndex = Math.max(0, Math.min(88, stepIndex));
          view.setInt16(out * 2, predictor, true);
          out++;
        }
      }
      return pcm;
    }

    function decodeAudioPayload(payload, samples, codec, stepIndex, predictor) {
      if (codec === CODEC_PCM16) return payload;
      if (codec === CODEC_IMA_ADPCM) return decodeImaAdpcm(payload, samples, predictor, stepIndex);
      throw new Error('Unknown audio codec ' + codec);
    }

    async function consume() {
      while (stash.length >= HEADER_BYTES) {
        const pos = findMagic(stash);
        if (pos < 0) {
          stash = stash.slice(Math.max(0, stash.length - 3));
          return;
        }
        if (pos > 0) stash = stash.slice(pos);
        if (stash.length < HEADER_BYTES) return;
        const dv = new DataView(stash.buffer, stash.byteOffset, stash.byteLength);
        const samples = dv.getUint16(4, true);
        const rate = dv.getUint16(6, true);
        const codec = dv.getUint8(8);
        const stepIndex = dv.getUint8(9);
        const predictor = dv.getInt16(10, true);
        const bytes = payloadBytes(samples, codec);
        if (stash.length < HEADER_BYTES + bytes) return;
        if (!ctx) {
          streamRate = rate;
          await createAudioChain(rate);
        }
        const payload = stash.slice(HEADER_BYTES, HEADER_BYTES + bytes);
        queuePcm(decodeAudioPayload(payload, samples, codec, stepIndex, predictor), samples, rate);
        stash = stash.slice(HEADER_BYTES + bytes);
      }
    }

    function concatFloat32(a, b) {
      if (!a.length) return b;
      const out = new Float32Array(a.length + b.length);
      out.set(a, 0);
      out.set(b, a.length);
      return out;
    }

    function makePcmFrame(samples, rate) {
      const frame = new Uint8Array(HEADER_BYTES + samples.length * 2);
      const view = new DataView(frame.buffer);
      frame[0] = 65; frame[1] = 85; frame[2] = 68; frame[3] = 48;
      view.setUint16(4, samples.length, true);
      view.setUint16(6, rate, true);
      view.setUint8(8, CODEC_PCM16);
      view.setUint8(9, 0);
      view.setInt16(10, 0, true);
      for (let i = 0; i < samples.length; i++) {
        const value = Math.max(-1, Math.min(1, samples[i]));
        view.setInt16(HEADER_BYTES + i * 2, value < 0 ? value * 32768 : value * 32767, true);
      }
      return frame;
    }

    function sendTalkAudio(input, fromRate) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      const mono16k = resampleLinear(input, fromRate, 16000);
      talkRemainder = concatFloat32(talkRemainder, mono16k);
      while (talkRemainder.length >= 320) {
        const chunk = talkRemainder.slice(0, 320);
        talkRemainder = talkRemainder.slice(320);
        ws.send(makePcmFrame(chunk, 16000));
      }
    }

    async function startTalking() {
      if (talking) return;
      if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
        msg.textContent = 'Microphone requires HTTPS or localhost in this browser.';
        return;
      }
      if (!running) await startListening();
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        msg.textContent = 'Connect first, then start talk.';
        return;
      }
      micStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true,
          channelCount: 1
        }
      });
      talkCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
      await talkCtx.resume();
      talkSource = talkCtx.createMediaStreamSource(micStream);
      talkNode = talkCtx.createScriptProcessor(1024, 1, 1);
      talkRemainder = new Float32Array(0);
      talkNode.onaudioprocess = (event) => {
        const input = event.inputBuffer.getChannelData(0);
        const output = event.outputBuffer.getChannelData(0);
        output.fill(0);
        sendTalkAudio(new Float32Array(input), talkCtx.sampleRate);
      };
      talkSource.connect(talkNode);
      talkNode.connect(talkCtx.destination);
      talking = true;
      talkBtn.textContent = 'Stop talking';
      talkBtn.className = '';
      msg.textContent = 'Talking to ESP32 speaker...';
    }

    async function stopTalking() {
      talking = false;
      talkRemainder = new Float32Array(0);
      if (talkNode) talkNode.disconnect();
      if (talkSource) talkSource.disconnect();
      if (micStream) micStream.getTracks().forEach(track => track.stop());
      if (talkCtx) await talkCtx.close();
      talkNode = null;
      talkSource = null;
      micStream = null;
      talkCtx = null;
      talkBtn.textContent = 'Talk to ESP32';
      talkBtn.className = 'secondary';
      if (running) msg.textContent = 'Listening live in ' + modeEl.value + ' mode.';
    }

    async function startListening() {
      if (running) return;
      running = true;
      underruns = 0;
      packetCount = 0;
      stash = new Uint8Array(0);
      try {
        if (!ctx) {
          await createAudioChain(streamRate);
        } else if (ctx.state !== 'running') {
          await ctx.resume();
          unlockAudioOutput();
        }
      } catch (error) {
        running = false;
        msg.textContent = 'Audio setup failed: ' + error.message;
        conn.className = 'val bad';
        return;
      }
      ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
      ws.binaryType = 'arraybuffer';
      conn.textContent = 'connecting';
      conn.className = 'val';
      ws.onopen = () => {
        conn.textContent = 'connected';
        conn.className = 'val ok';
        msg.textContent = 'Waiting for audio packets...';
        if (!statusTimer) statusTimer = setInterval(loadStatus, 2000);
      };
      ws.onclose = () => {
        conn.textContent = 'closed';
        conn.className = 'val bad';
        running = false;
        msg.textContent = 'Connection closed.';
      };
      ws.onerror = () => {
        conn.textContent = 'error';
        conn.className = 'val bad';
      };
      ws.onmessage = async (event) => {
        packetCount++;
        if (packetCount === 1) msg.textContent = 'Receiving audio packets...';
        stash = merge(stash, new Uint8Array(event.data));
        try {
          await consume();
        } catch (error) {
          msg.textContent = 'Audio setup failed: ' + error.message;
          conn.className = 'val bad';
        }
      };
    }

    async function stopListening() {
      running = false;
      if (talking) await stopTalking();
      if (ws) ws.close();
      ws = null;
      if (ctx) await ctx.close();
      ctx = null;
      workletNode = null;
      scriptNode = null;
      gainNode = null;
      resetFallbackState();
      conn.textContent = 'stopped';
      audio.textContent = 'waiting';
      buffer.textContent = '0 ms';
      bar.style.width = '0';
      msg.textContent = 'Stopped.';
      if (statusTimer) { clearInterval(statusTimer); statusTimer = null; }
    }

    document.getElementById('start').onclick = startListening;
    document.getElementById('stop').onclick = stopListening;
    talkBtn.onclick = async () => {
      try {
        if (talking) await stopTalking();
        else await startTalking();
      } catch (error) {
        msg.textContent = 'Talk failed: ' + error.message;
        await stopTalking();
      }
    };
    modeEl.onchange = applyMode;
    gainEl.oninput = updateGain;
    updateGain();
    function fmtBytes(n){if(n<1024)return n+' B'; if(n<1048576)return (n/1024).toFixed(1)+' KB'; return (n/1048576).toFixed(1)+' MB'}
    function fmtDuration(s){if(!s)return '0:00'; const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sec=Math.floor(s%60); return h ? `${h}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}` : `${m}:${String(sec).padStart(2,'0')}`}
    async function loadRecordings(){
      const box = document.getElementById('recordings');
      try{
        const res = await fetch('/api/recordings', {cache:'no-store'});
        const items = await res.json();
        if(!items.length){box.innerHTML='<div class="empty">No recordings yet</div>';return}
        box.innerHTML = items.map(item => `
          <div class="recording">
            <div class="rec-head">
              <div><div class="rec-title">${item.date}</div><div class="rec-meta">${fmtDuration(item.duration_seconds)} · ${fmtBytes(item.bytes)} · ${item.rate || '-'} Hz</div></div>
              <a class="button secondary" href="${item.download_url}">Download</a>
            </div>
            <audio controls preload="none" src="${item.play_url}"></audio>
          </div>`).join('');
      }catch(e){box.innerHTML='<div class="empty">Could not load recordings</div>'}
    }
    async function loadStatus() {
      try {
        const res = await fetch('/api/status', {cache: 'no-store'});
        const data = await res.json();
        if (data.source_connected) {
          sourceEl.textContent = 'live / ' + (data.source_age_ms || 0) + ' ms ago';
          sourceEl.className = 'val ok';
        } else if (data.source_recent) {
          sourceEl.textContent = 'recent / ' + (data.source_age_ms || 0) + ' ms ago';
          sourceEl.className = 'val';
        } else {
          sourceEl.textContent = 'no live audio';
          sourceEl.className = 'val bad';
        }
        if (!data.source_recent && running) {
          msg.textContent = 'ESP32 is not sending audio to the server right now.';
        }
      } catch (e) {
        sourceEl.textContent = 'status unavailable';
        sourceEl.className = 'val bad';
      }
    }
    document.getElementById('refresh').onclick = loadRecordings;
    loadRecordings();
    loadStatus();
  </script>
</body>
</html>
"""


class SocketReader:
    def __init__(self, sock):
        self.sock = sock

    def read(self, count):
        return self.sock.recv(count)


class DailyWavRecorder:
    def __init__(self, directory):
        self.directory = os.path.abspath(directory)
        self.current_day = None
        self.file = None
        self.rate = None
        self.data_bytes = 0
        self.buffer = bytearray()
        self.last_header_update = 0
        self.source_connected = False
        self.last_packet_wall = 0.0
        os.makedirs(self.directory, exist_ok=True)

    def close(self):
        if self.file:
            self._finalize_header()
            self.file.close()
            self.file = None
        self.source_connected = False

    def flush_header(self):
        self._finalize_header()

    def write_frame(self, chunk):
        self.source_connected = True
        self.last_packet_wall = time.time()
        self.buffer.extend(chunk)
        while True:
            pos = self.buffer.find(MAGIC)
            if pos < 0:
                if len(self.buffer) > len(MAGIC):
                    del self.buffer[:-len(MAGIC)]
                return
            if pos > 0:
                del self.buffer[:pos]
            if len(self.buffer) < HEADER.size:
                return
            _, samples, rate, codec, step_index, predictor = HEADER.unpack(self.buffer[:HEADER.size])
            payload_start = HEADER.size
            try:
                payload_end = payload_start + payload_size(samples, codec)
            except ValueError:
                del self.buffer[:len(MAGIC)]
                continue
            if payload_end > len(self.buffer):
                return
            try:
                pcm = decode_audio_payload(
                    bytes(self.buffer[payload_start:payload_end]),
                    samples,
                    codec,
                    step_index,
                    predictor,
                )
            except ValueError:
                del self.buffer[:payload_end]
                continue
            self.write_pcm(pcm, rate)
            del self.buffer[:payload_end]

    def write_pcm(self, pcm, rate):
        today = dt.datetime.now().strftime("%Y-%m-%d")
        if self.file is None or self.current_day != today or self.rate != rate:
            self._open(today, rate)
        self.file.write(pcm)
        self.data_bytes += len(pcm)
        if time.monotonic() - self.last_header_update >= 1:
            self._finalize_header()
            self.last_header_update = time.monotonic()

    def _open(self, day, rate):
        self.close()
        self.current_day = day
        self.rate = rate
        self.data_bytes = 0
        self.last_header_update = 0
        path = os.path.join(self.directory, f"{day}.wav")
        exists = os.path.exists(path)
        prior_size = os.path.getsize(path) if exists else 0
        self.file = open(path, "r+b" if exists else "w+b")
        if not exists or prior_size < 44:
            self.file.write(self._header(rate, 0))
            self.data_bytes = 0
        else:
            self.data_bytes = max(0, prior_size - 44)
            self.file.seek(0, os.SEEK_END)

    def _finalize_header(self):
        if not self.file:
            return
        pos = self.file.tell()
        self.file.seek(0)
        self.file.write(self._header(self.rate, self.data_bytes))
        self.file.seek(pos)
        self.file.flush()

    @staticmethod
    def _header(rate, data_bytes):
        byte_rate = rate * 2
        block_align = 2
        return (
            b"RIFF"
            + struct.pack("<I", 36 + data_bytes)
            + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, rate, byte_rate, block_align, 16)
            + b"data"
            + struct.pack("<I", data_bytes)
        )


def wav_info(path):
    size = os.path.getsize(path)
    rate = None
    data_bytes = max(0, size - 44)
    try:
        with open(path, "rb") as file:
            header = file.read(44)
        if len(header) >= 44 and header[0:4] == b"RIFF" and header[8:12] == b"WAVE":
            rate = struct.unpack("<I", header[24:28])[0]
            data_bytes = struct.unpack("<I", header[40:44])[0]
    except OSError:
        pass
    duration = data_bytes / (rate * 2) if rate else 0
    return {
        "bytes": size,
        "rate": rate,
        "duration_seconds": round(duration, 1),
        "mtime": int(os.path.getmtime(path)),
    }


def list_recordings(directory):
    os.makedirs(directory, exist_ok=True)
    items = []
    for name in sorted(os.listdir(directory), reverse=True):
        if not RECORDING_NAME.match(name):
            continue
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        info = wav_info(path)
        date = name[:-4]
        items.append(
            {
                "date": date,
                "filename": name,
                "bytes": info["bytes"],
                "rate": info["rate"],
                "duration_seconds": info["duration_seconds"],
                "modified": info["mtime"],
                "play_url": f"/recordings/{urllib.parse.quote(name)}",
                "download_url": f"/recordings/{urllib.parse.quote(name)}?download=1",
            }
        )
    return items


def read_exact(reader, count):
    data = bytearray()
    while len(data) < count:
        chunk = reader.read(count - len(data))
        if not chunk:
            raise TimeoutError("stream closed or timed out")
        data.extend(chunk)
    return bytes(data)


def payload_size(samples, codec):
    if codec == CODEC_PCM16:
        return samples * 2
    if codec == CODEC_IMA_ADPCM:
        return (samples + 1) // 2
    raise ValueError(f"unknown audio codec {codec}")


def decode_ima_adpcm(payload, samples, predictor, step_index):
    predictor = max(-32768, min(32767, int(predictor)))
    step_index = max(0, min(88, int(step_index)))
    output = []
    for byte in payload:
        for code in (byte & 0x0F, byte >> 4):
            if len(output) >= samples:
                break
            step = IMA_STEP_TABLE[step_index]
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            predictor += -delta if code & 8 else delta
            predictor = max(-32768, min(32767, predictor))
            step_index += IMA_INDEX_TABLE[code & 0x0F]
            step_index = max(0, min(88, step_index))
            output.append(predictor)
    return struct.pack(f"<{len(output)}h", *output)


def decode_audio_payload(payload, samples, codec, step_index=0, predictor=0):
    if codec == CODEC_PCM16:
        return payload
    if codec == CODEC_IMA_ADPCM:
        return decode_ima_adpcm(payload, samples, predictor, step_index)
    raise ValueError(f"unknown audio codec {codec}")


def sync_to_frame(reader):
    window = bytearray()
    while True:
        byte = reader.read(1)
        if not byte:
            raise TimeoutError("waiting for audio frame")
        window += byte
        if len(window) > len(MAGIC):
            del window[0]
        if bytes(window) == MAGIC:
            rest = read_exact(reader, HEADER.size - len(MAGIC))
            _, samples, rate, codec, step_index, predictor = HEADER.unpack(MAGIC + rest)
            return samples, rate, codec, step_index, predictor


def apply_gain(pcm, samples, gain):
    if gain == 1.0:
        return pcm
    values = struct.unpack(f"<{samples}h", pcm)
    clipped = [max(-32768, min(32767, int(value * gain))) for value in values]
    return struct.pack(f"<{samples}h", *clipped)


def play_stream(reader, gain=1.0, device=None):
    import sounddevice as sd

    samples, rate, codec, step_index, predictor = sync_to_frame(reader)
    codec_name = "IMA ADPCM" if codec == CODEC_IMA_ADPCM else "PCM16"
    print(f"Audio stream locked: {rate} Hz, {samples} samples/frame, {codec_name}", flush=True)

    with sd.RawOutputStream(
        samplerate=rate,
        channels=1,
        dtype="int16",
        device=device,
        blocksize=samples,
    ) as stream:
        last_report = time.monotonic()
        frames = 0
        while True:
            payload = read_exact(reader, payload_size(samples, codec))
            pcm = decode_audio_payload(payload, samples, codec, step_index, predictor)
            stream.write(apply_gain(pcm, samples, gain))
            frames += samples

            next_samples, next_rate, next_codec, next_step_index, next_predictor = sync_to_frame(reader)
            if next_rate != rate:
                raise RuntimeError(f"sample rate changed from {rate} to {next_rate}")
            if next_codec != codec:
                raise RuntimeError(f"audio codec changed from {codec} to {next_codec}")
            samples = next_samples
            codec = next_codec
            step_index = next_step_index
            predictor = next_predictor

            now = time.monotonic()
            if now - last_report >= 5:
                print(f"played {frames / rate:.1f}s", flush=True)
                last_report = now


def run_serial(args):
    import serial

    with serial.Serial(args.port, args.baud, timeout=2) as port:
        port.reset_input_buffer()
        print(f"Listening on {args.port} at {args.baud} baud. Press Ctrl-C to stop.", flush=True)
        play_stream(port, gain=args.gain, device=args.device)


def set_reuseaddr(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)


def make_listener(host, port):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    set_reuseaddr(listener)
    listener.bind((host, port))
    listener.listen()
    listener.setblocking(False)
    return listener


def websocket_frame(payload):
    size = len(payload)
    if size < 126:
        return bytes([0x82, size]) + payload
    if size <= 0xFFFF:
        return bytes([0x82, 126]) + struct.pack("!H", size) + payload
    return bytes([0x82, 127]) + struct.pack("!Q", size) + payload


def websocket_messages(buffer):
    messages = []
    offset = 0
    close_requested = False
    while len(buffer) - offset >= 2:
        first = buffer[offset]
        second = buffer[offset + 1]
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        size = second & 0x7F
        header_size = 2
        if size == 126:
            if len(buffer) - offset < 4:
                break
            size = struct.unpack("!H", buffer[offset + 2:offset + 4])[0]
            header_size = 4
        elif size == 127:
            if len(buffer) - offset < 10:
                break
            size = struct.unpack("!Q", buffer[offset + 2:offset + 10])[0]
            header_size = 10
        if masked:
            if len(buffer) - offset < header_size + 4:
                break
            mask = buffer[offset + header_size:offset + header_size + 4]
            header_size += 4
        else:
            mask = b""
        frame_end = offset + header_size + size
        if len(buffer) < frame_end:
            break
        payload = bytes(buffer[offset + header_size:frame_end])
        if masked:
            payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        if opcode == 0x8:
            close_requested = True
        elif opcode == 0x2:
            messages.append(payload)
        offset = frame_end
    del buffer[:offset]
    return messages, close_requested


def http_response(body, status="200 OK", content_type="text/html; charset=utf-8", extra_headers=None):
    data = body if isinstance(body, bytes) else body.encode("utf-8")
    headers = (
        f"HTTP/1.1 {status}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(data)}\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
    )
    if extra_headers:
        for key, value in extra_headers.items():
            headers += f"{key}: {value}\r\n"
    headers += "\r\n"
    headers = headers.encode("utf-8")
    return headers + data


def websocket_accept_key(key):
    digest = hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def serve_recording(sock, path, recordings_dir, request_text):
    parsed = urllib.parse.urlsplit(path)
    name = urllib.parse.unquote(parsed.path.rsplit("/", 1)[-1])
    if not RECORDING_NAME.match(name):
        sock.sendall(http_response("Not found", "404 Not Found", "text/plain"))
        return

    file_path = os.path.abspath(os.path.join(recordings_dir, name))
    if os.path.dirname(file_path) != os.path.abspath(recordings_dir) or not os.path.exists(file_path):
        sock.sendall(http_response("Not found", "404 Not Found", "text/plain"))
        return

    disposition = "attachment" if "download=1" in parsed.query else "inline"
    size = os.path.getsize(file_path)
    start = 0
    end = size - 1
    status = "200 OK"
    for line in request_text.splitlines()[1:]:
        if line.lower().startswith("range:"):
            value = line.split(":", 1)[1].strip()
            if value.startswith("bytes="):
                first, _, last = value[6:].partition("-")
                if first:
                    start = max(0, int(first))
                if last:
                    end = min(size - 1, int(last))
                status = "206 Partial Content"
            break
    if start > end or start >= size:
        sock.sendall(http_response("Invalid range", "416 Range Not Satisfiable", "text/plain"))
        return
    length = end - start + 1
    headers = (
        f"HTTP/1.1 {status}\r\n"
        "Content-Type: audio/wav\r\n"
        f"Content-Length: {length}\r\n"
        "Accept-Ranges: bytes\r\n"
        f'Content-Disposition: {disposition}; filename="{name}"\r\n'
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
    )
    if status.startswith("206"):
        headers += f"Content-Range: bytes {start}-{end}/{size}\r\n"
    headers = (headers + "\r\n").encode("utf-8")
    sock.sendall(headers)
    with open(file_path, "rb") as file:
        file.seek(start)
        remaining = length
        while True:
            data = file.read(min(64 * 1024, remaining))
            if not data:
                break
            sock.sendall(data)
            remaining -= len(data)
            if remaining <= 0:
                break


def handle_http_request(sock, request, recorder):
    text = request.decode("utf-8", errors="replace")
    first = text.splitlines()[0] if text.splitlines() else ""
    path = first.split(" ")[1] if len(first.split(" ")) >= 2 else "/"
    if "GET /ws " in first:
        key = ""
        for line in text.splitlines()[1:]:
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
                break
        if not key:
            sock.sendall(http_response("Missing WebSocket key", "400 Bad Request", "text/plain"))
            return False
        response = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {websocket_accept_key(key)}\r\n\r\n"
        ).encode("ascii")
        sock.sendall(response)
        return True

    if path == "/" or path == "/index.html":
        sock.sendall(http_response(WEB_PAGE))
    elif path == "/api/status":
        now = time.time()
        age_ms = int((now - recorder.last_packet_wall) * 1000) if recorder.last_packet_wall else None
        payload = {
            "source_connected": recorder.source_connected,
            "source_recent": age_ms is not None and age_ms < 3000,
            "source_age_ms": age_ms,
            "recordings_dir": recorder.directory,
        }
        sock.sendall(http_response(json.dumps(payload), content_type="application/json"))
    elif path == "/api/recordings":
        recorder.flush_header()
        sock.sendall(http_response(json.dumps(list_recordings(recorder.directory)), content_type="application/json"))
    elif path.startswith("/recordings/"):
        recorder.flush_header()
        serve_recording(sock, path, recorder.directory, text)
    else:
        sock.sendall(http_response("Not found", "404 Not Found", "text/plain"))
    return False


def read_http_request(sock, timeout=1.0):
    sock.settimeout(timeout)
    data = bytearray()
    while b"\r\n\r\n" not in data and len(data) < 16384:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def close_socket(sel, sock):
    try:
        sel.unregister(sock)
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass


def run_server(args):
    sel = selectors.DefaultSelector()
    source_listener = make_listener(args.bind, args.source_port)
    client_listener = make_listener(args.bind, args.client_port)
    web_listener = make_listener(args.bind, args.web_port)
    recorder = DailyWavRecorder(args.recordings_dir)
    sel.register(source_listener, selectors.EVENT_READ, "source-listener")
    sel.register(client_listener, selectors.EVENT_READ, "client-listener")
    sel.register(web_listener, selectors.EVENT_READ, "web-listener")

    source = None
    clients = set()
    web_clients = set()
    web_buffers = {}
    total_bytes = 0
    talk_bytes = 0
    last_report = time.monotonic()

    print(
        f"Relay server ready. ESP32 connects to {args.bind}:{args.source_port}; "
        f"TCP listeners connect to {args.bind}:{args.client_port}; "
        f"web listeners open http://{args.bind}:{args.web_port}/.",
        flush=True,
    )

    while True:
        for key, _ in sel.select(timeout=1):
            if key.data == "source-listener":
                conn, addr = source_listener.accept()
                conn.setblocking(False)
                if source is not None:
                    print("Replacing previous ESP32 source", flush=True)
                    close_socket(sel, source)
                source = conn
                sel.register(source, selectors.EVENT_READ, "source")
                print(f"ESP32 source connected from {addr[0]}:{addr[1]}", flush=True)

            elif key.data == "client-listener":
                conn, addr = client_listener.accept()
                conn.setblocking(False)
                clients.add(conn)
                print(f"Listener connected from {addr[0]}:{addr[1]} clients={len(clients)}", flush=True)

            elif key.data == "web-listener":
                conn, addr = web_listener.accept()
                print(f"Web connection from {addr[0]}:{addr[1]}", flush=True)
                try:
                    request = read_http_request(conn)
                except OSError:
                    request = b""
                if not request:
                    conn.close()
                    continue
                try:
                    upgraded = handle_http_request(conn, request, recorder)
                except OSError:
                    upgraded = False
                if upgraded:
                    conn.setblocking(False)
                    web_clients.add(conn)
                    web_buffers[conn] = bytearray()
                    sel.register(conn, selectors.EVENT_READ, "websocket")
                    print(f"Web listener upgraded clients={len(web_clients)}", flush=True)
                else:
                    conn.close()

            elif key.data == "websocket":
                try:
                    chunk = key.fileobj.recv(1024)
                except OSError:
                    chunk = b""
                if not chunk:
                    web_clients.discard(key.fileobj)
                    web_buffers.pop(key.fileobj, None)
                    close_socket(sel, key.fileobj)
                    print(f"Web listener disconnected clients={len(web_clients)}", flush=True)
                else:
                    buffer = web_buffers.setdefault(key.fileobj, bytearray())
                    buffer.extend(chunk)
                    messages, close_requested = websocket_messages(buffer)
                    if close_requested:
                        web_clients.discard(key.fileobj)
                        web_buffers.pop(key.fileobj, None)
                        close_socket(sel, key.fileobj)
                        print(f"Web listener disconnected clients={len(web_clients)}", flush=True)
                        continue
                    for message in messages:
                        if source is None:
                            continue
                        try:
                            source.sendall(message)
                            talk_bytes += len(message)
                        except OSError:
                            print("ESP32 source disconnected during talk downlink", flush=True)
                            close_socket(sel, source)
                            source = None
                            recorder.close()
                            break

            elif key.data == "source":
                try:
                    chunk = key.fileobj.recv(4096)
                except OSError:
                    chunk = b""
                if not chunk:
                    print("ESP32 source disconnected", flush=True)
                    close_socket(sel, key.fileobj)
                    source = None
                    recorder.close()
                    continue
                total_bytes += len(chunk)
                recorder.write_frame(chunk)
                dead = []
                for client in clients:
                    try:
                        client.sendall(chunk)
                    except OSError:
                        dead.append(client)
                for client in dead:
                    clients.remove(client)
                    close_socket(sel, client)
                    print(f"Listener disconnected clients={len(clients)}", flush=True)
                web_dead = []
                frame = websocket_frame(chunk)
                for client in web_clients:
                    try:
                        client.sendall(frame)
                    except OSError:
                        web_dead.append(client)
                for client in web_dead:
                    web_clients.remove(client)
                    web_buffers.pop(client, None)
                    close_socket(sel, client)
                    print(f"Web listener disconnected clients={len(web_clients)}", flush=True)

        now = time.monotonic()
        if now - last_report >= 5:
            kbps = (total_bytes * 8 / 1000) / (now - last_report)
            print(
                f"status source={'yes' if source else 'no'} tcp_clients={len(clients)} web_clients={len(web_clients)} rate={kbps:.1f} kbit/s talk_down={(talk_bytes * 8 / 1000) / (now - last_report):.1f} kbit/s",
                flush=True,
            )
            total_bytes = 0
            talk_bytes = 0
            last_report = now


def run_client(args):
    print(f"Connecting to relay {args.host}:{args.port}...", flush=True)
    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.settimeout(None)
        print("Connected. Press Ctrl-C to stop.", flush=True)
        play_stream(SocketReader(sock), gain=args.gain, device=args.device)


def build_parser():
    parser = argparse.ArgumentParser(description="ESP32-S3 microphone listener and relay.")
    subparsers = parser.add_subparsers(dest="mode")

    serial_parser = subparsers.add_parser("serial", help="Play the old USB serial stream.")
    serial_parser.add_argument("--port", default="/dev/cu.usbmodem11101", help="Serial port for the ESP32.")
    serial_parser.add_argument("--baud", type=int, default=921600, help="Serial baud rate.")
    serial_parser.add_argument("--gain", type=float, default=1.0, help="Playback gain multiplier.")
    serial_parser.add_argument("--device", default=None, help="Optional sounddevice output device name or id.")
    serial_parser.set_defaults(func=run_serial)

    server_parser = subparsers.add_parser("server", help="Relay ESP32 audio to listener clients.")
    server_parser.add_argument("--bind", default="0.0.0.0", help="Address to bind.")
    server_parser.add_argument("--source-port", type=int, default=8765, help="Port the ESP32 connects to.")
    server_parser.add_argument("--client-port", type=int, default=8766, help="Port listener clients connect to.")
    server_parser.add_argument("--web-port", type=int, default=8080, help="Port for the mobile/browser listener page.")
    server_parser.add_argument("--recordings-dir", default="recordings", help="Directory for daily WAV recordings.")
    server_parser.set_defaults(func=run_server)

    client_parser = subparsers.add_parser("client", help="Play audio from the relay server.")
    client_parser.add_argument("--host", required=True, help="Relay server hostname or IP.")
    client_parser.add_argument("--port", type=int, default=8766, help="Relay listener port.")
    client_parser.add_argument("--gain", type=float, default=1.0, help="Playback gain multiplier.")
    client_parser.add_argument("--device", default=None, help="Optional sounddevice output device name or id.")
    client_parser.set_defaults(func=run_client)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.mode is None:
        args = parser.parse_args(["serial"])
    args.func(args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
