/* 8Kloud Switcher web console.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * One WebSocket (/ws) carries everything: text lines in the remote-control
 * protocol (docs/remote-control.md) and JSON web commands out; JSON
 * `state` / `ui` / `meters` events and binary JPEG multiview frames in. */
'use strict';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};
const pad2 = (n) => String(n).padStart(2, '0');
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const fmtTime = (ms) => {
  if (!ms || ms <= 0) return '00:00';
  const s = Math.floor(ms / 1000);
  return `${pad2(Math.floor(s / 60))}:${pad2(s % 60)}`;
};
const fmtTrim = (ms, end) => {
  if (end && ms === 0) return 'END';
  const s = Math.floor(ms / 1000);
  return `${pad2(Math.floor(s / 60))}:${pad2(s % 60)}.${String(ms % 1000).padStart(3, '0')}`;
};
const basename = (p) => p.substring(p.lastIndexOf('/') + 1);
const VIDEO_EXT = /\.(mkv|mp4|mov|m4v|ts|mts|m2ts|webm|avi)$/i;
const STILL_EXT = /\.(png|jpe?g|webp|bmp|tiff?|tga|exr)$/i;

const RESOLUTIONS = [
  [1280, 720, '1280 × 720  HD'], [1920, 1080, '1920 × 1080  FHD'], [2560, 1440, '2560 × 1440  QHD'],
  [3840, 2160, '3840 × 2160  UHD'], [4096, 2160, '4096 × 2160  DCI 4K'], [7680, 4320, '7680 × 4320  8K'],
];
const FRAME_RATES = [
  [24000, 1001, '23.98p'], [24, 1, '24p'], [25, 1, '25p'], [30000, 1001, '29.97p'],
  [30, 1, '30p'], [50, 1, '50p'], [60000, 1001, '59.94p'], [60, 1, '60p'],
];
const MV_SIZES = [[1280, 720], [1920, 1080], [2560, 1440], [3840, 2160]];

// ---------------------------------------------------------------- state --
const S = {
  ws: null, connected: false, state: null, ui: null, inputCount: 0,
  names: [], frame: null, frameSeq: 0, hover: -1, hitRects: [],
  tbarDown: false, editing: new Set(), meters: [], meterHold: [],
  sources: { omt: [], decklink: [] }, pendingSources: null, layout: 'stacked',
};

function send(text) {
  if (S.ws && S.ws.readyState === WebSocket.OPEN) S.ws.send(text);
}
const cmd = (obj) => send(JSON.stringify(obj));

// ------------------------------------------------------------- socket ----
function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = 'blob';
  S.ws = ws;
  ws.onopen = () => {
    S.connected = true;
    setBadge($('linkBadge'), 'good', '●  LINK UP');
    cmd({ cmd: 'ui' });
  };
  ws.onclose = () => {
    S.connected = false;
    setBadge($('linkBadge'), 'bad', '●  RECONNECTING');
    setTimeout(connect, 1000);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    if (ev.data instanceof Blob) {
      onFrame(ev.data);
      return;
    }
    let msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    switch (msg.event) {
      case 'state': onState(msg); break;
      case 'ui': onUi(msg); break;
      case 'meters': onMeters(msg.v); break;
      case 'sources': onSources(msg); break;
      case 'ls': onListing(msg); break;
      case 'error': console.warn('switcher:', msg.message); flashStatus(msg.message); break;
      default: break;
    }
  };
}

function setBadge(node, state, text) {
  node.dataset.state = state;
  node.textContent = text;
}

let statusFlashTimer = null;
function flashStatus(message) {
  const st = $('status');
  st.textContent = `⚠ ${message}`;
  st.style.color = '#ffb347';
  clearTimeout(statusFlashTimer);
  statusFlashTimer = setTimeout(() => { st.style.color = ''; if (S.ui) st.textContent = S.ui.status; }, 4000);
}

// ---------------------------------------------------------- multiview ----
let drawing = false;
async function onFrame(blob) {
  if (drawing) { S.pendingBlob = blob; return; }
  drawing = true;
  try {
    const bmp = await createImageBitmap(blob);
    if (S.frame) S.frame.close();
    S.frame = bmp;
    $('mvWaiting').hidden = true;
    drawMultiview();
  } catch (e) { /* a torn frame: skip */ }
  drawing = false;
  if (S.pendingBlob) { const b = S.pendingBlob; S.pendingBlob = null; onFrame(b); }
}

// Mirrors gpu/Compositor.cpp's input-matrix layout so clicks land on the
// right cell. Everything is computed in wall pixels and scaled to the canvas.
function mvLayout(mvW, mvH, n) {
  const splitX = (mvW >> 1) & ~1;
  const inputW = splitX;
  const cols = n <= 6 ? Math.max(n, 1) : Math.floor((n + 2) / 3);
  const rows = Math.max(1, Math.ceil(n / cols));
  const cellW = Math.floor(inputW / cols) & ~1;
  const videoH = Math.max(2, Math.floor(cellW * 9 / 16) & ~1);
  const cellH = Math.min(videoH + 24, Math.max(24, Math.floor(mvH / rows) & ~1));
  const cells = [];
  for (let i = 0; i < n; i++) {
    const col = i % cols, row = Math.floor(i / cols);
    const x = col * cellW, y = row * cellH;
    cells.push({ x, y, w: col === cols - 1 ? inputW - x : cellW, h: Math.min(cellH, mvH - y) });
  }
  return cells;
}

function drawMultiview() {
  const canvas = $('mv');
  const stage = canvas.parentElement;
  const dpr = window.devicePixelRatio || 1;
  const cw = Math.max(2, Math.floor(stage.clientWidth * dpr));
  const ch = Math.max(2, Math.floor(stage.clientHeight * dpr));
  if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#08090b';
  ctx.fillRect(0, 0, cw, ch);
  if (!S.frame) return;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  const fw = S.frame.width, fh = S.frame.height;
  const mv = S.ui ? S.ui.mv : null;
  const n = S.inputCount;
  if (!mv || !n) {
    const scale = Math.min((cw - 12 * dpr) / fw, (ch - 12 * dpr) / fh);
    ctx.drawImage(S.frame, (cw - fw * scale) / 2, (ch - fh * scale) / 2, fw * scale, fh * scale);
    S.hitRects = [];
    return;
  }
  // The wall is one image whose left half is the input matrix and right
  // half is PROGRAM over PREVIEW. Present the banks independently: the
  // outputs take the height (stacked) or width (side by side) they can get
  // on the right, the input bank (cropped to its used rows) fills what is
  // left, so surplus space enlarges monitors instead of becoming letterbox.
  const kx = fw / mv.w, ky = fh / mv.h;  // wall -> stream pixels
  const cells = mvLayout(mv.w, mv.h, n);
  const splitX = (mv.w >> 1) & ~1;
  const inputH = Math.min(mv.h, Math.max(...cells.map((c) => c.y + c.h)));
  const outputH = ((mv.h - 4) >> 1) & ~1;  // compositor's kOutputGap = 4
  const inSrc = { x: 0, y: 0, w: splitX, h: inputH };
  const pgmSrc = { x: splitX, y: 0, w: mv.w - splitX, h: outputH };
  const pvwSrc = { x: splitX, y: outputH + 4, w: mv.w - splitX, h: mv.h - outputH - 4 };
  const pad = 6 * dpr, gap = 12 * dpr;
  const availW = cw - 2 * pad - gap, availH = ch - 2 * pad;
  const bank = (src, dx, dy, dw, dh) => {
    ctx.drawImage(S.frame, src.x * kx, src.y * ky, src.w * kx, src.h * ky, dx, dy, dw, dh);
    ctx.strokeStyle = '#494f56';
    ctx.lineWidth = dpr;
    ctx.strokeRect(dx - 0.5 * dpr, dy - 0.5 * dpr, dw + dpr, dh + dpr);
  };
  let outLeft;
  if (S.layout === 'wall') {
    const scale = Math.min((cw - 2 * pad) / fw, availH / fh);
    const dx = (cw - fw * scale) / 2, dy = (ch - fh * scale) / 2;
    ctx.drawImage(S.frame, dx, dy, fw * scale, fh * scale);
    const sIn = scale * kx;
    S.hitRects = cells.map((c) => ({ x: (dx + c.x * sIn) / dpr, y: (dy + c.y * sIn) / dpr, w: c.w * sIn / dpr, h: c.h * sIn / dpr }));
    drawHover(ctx, cells, dx, dy, sIn, dpr);
    return;
  }
  if (S.layout === 'side') {
    // PGM and PVW in one row, each 16:9, sharing the stage height.
    const blockW = 2 * pgmSrc.w + 4;
    const sOut = Math.min(availH / pgmSrc.h, (availW * 0.66) / blockW);
    const cellW = pgmSrc.w * sOut, cellH = pgmSrc.h * sOut;
    const y = (ch - cellH) / 2;
    outLeft = cw - pad - 2 * cellW - 4 * sOut;
    bank(pgmSrc, outLeft, y, cellW, cellH);
    bank(pvwSrc, outLeft + cellW + 4 * sOut, y, cellW, pvwSrc.h * sOut);
  } else {
    const outSrc = { x: splitX, y: 0, w: mv.w - splitX, h: mv.h };
    const sOut = Math.min(availH / outSrc.h, (availW * 0.55) / outSrc.w);
    const outW = outSrc.w * sOut, outH = outSrc.h * sOut;
    outLeft = cw - pad - outW;
    bank(outSrc, outLeft, (ch - outH) / 2, outW, outH);
  }
  const sIn = Math.min((outLeft - gap - pad) / inSrc.w, availH / inSrc.h);
  const inW = inSrc.w * sIn, inH = inSrc.h * sIn;
  const inX = pad, inY = (ch - inH) / 2;
  bank(inSrc, inX, inY, inW, inH);
  S.hitRects = cells.map((c) => ({ x: (inX + c.x * sIn) / dpr, y: (inY + c.y * sIn) / dpr, w: c.w * sIn / dpr, h: c.h * sIn / dpr }));
  drawHover(ctx, cells, inX, inY, sIn, dpr);
}

function drawHover(ctx, cells, ox, oy, scale, dpr) {
  if (S.hover < 0 || S.hover >= cells.length) return;
  const c = cells[S.hover];
  ctx.strokeStyle = '#3fd6f7';
  ctx.lineWidth = 2 * dpr;
  ctx.fillStyle = 'rgba(47, 201, 242, 0.09)';
  const rx = ox + c.x * scale + 2 * dpr, ry = oy + c.y * scale + 2 * dpr;
  const rw = c.w * scale - 4 * dpr, rh = c.h * scale - 4 * dpr;
  ctx.fillRect(rx, ry, rw, rh);
  ctx.strokeRect(rx, ry, rw, rh);
}

function sourceAt(ev) {
  const rect = $('mv').getBoundingClientRect();
  const x = ev.clientX - rect.left, y = ev.clientY - rect.top;
  return S.hitRects.findIndex((r) => x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h);
}

function setupMultiview() {
  const canvas = $('mv');
  canvas.addEventListener('mousemove', (ev) => {
    const h = sourceAt(ev);
    if (h !== S.hover) { S.hover = h; drawMultiview(); }
    canvas.style.cursor = h >= 0 ? 'pointer' : 'default';
  });
  canvas.addEventListener('mouseleave', () => { S.hover = -1; drawMultiview(); });
  canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
  canvas.addEventListener('mousedown', (ev) => {
    const i = sourceAt(ev);
    if (i < 0) return;
    ev.preventDefault();
    if (ev.button === 0) send(`PVW ${i + 1}`);
    else if (ev.button === 2) send(`PGM ${i + 1}`);
  });
  new ResizeObserver(() => drawMultiview()).observe(canvas.parentElement);

  // Per-browser presentation preferences; URL parameters win so a kiosk can
  // be pinned (?layout=side&deck=280).
  const params = new URLSearchParams(location.search);
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem('kloud.view') || '{}'); } catch (e) { /* ignore */ }
  const persist = () => { try { localStorage.setItem('kloud.view', JSON.stringify(saved)); } catch (e) { /* ignore */ } };
  const layoutSel = $('mvLayout');
  const layout = params.get('layout') || saved.layout;
  if (['stacked', 'side', 'wall'].includes(layout)) { S.layout = layout; layoutSel.value = layout; }
  layoutSel.addEventListener('change', () => { S.layout = layoutSel.value; saved.layout = S.layout; persist(); drawMultiview(); });

  const deck = $('deck');
  const setDeck = (h) => {
    const max = Math.max(240, window.innerHeight - 260);
    deck.style.height = `${clamp(Math.round(h), 200, max)}px`;
  };
  const deckParam = Number(params.get('deck')) || Number(saved.deck);
  if (deckParam) setDeck(deckParam);
  const splitter = $('splitter');
  splitter.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    splitter.setPointerCapture(ev.pointerId);
    const startY = ev.clientY, startH = deck.getBoundingClientRect().height;
    const move = (e) => setDeck(startH - (e.clientY - startY));
    const up = () => {
      splitter.removeEventListener('pointermove', move);
      splitter.removeEventListener('pointerup', up);
      saved.deck = deck.getBoundingClientRect().height;
      persist();
    };
    splitter.addEventListener('pointermove', move);
    splitter.addEventListener('pointerup', up);
  });
  window.addEventListener('resize', () => { if (deck.style.height) setDeck(parseFloat(deck.style.height)); });
}

// -------------------------------------------------------------- state ----
function onState(st) {
  S.state = st;
  const pgm = st.program - 1, pvw = st.preview - 1;
  document.querySelectorAll('#pgmKeys .source-button').forEach((b, i) => { b.dataset.tally = i === pgm ? 'program' : 'idle'; });
  document.querySelectorAll('#pvwKeys .source-button').forEach((b, i) => { b.dataset.tally = i === pvw ? 'preview' : 'idle'; });
  refreshBusReadouts();
  $('autoBtn').classList.toggle('active', st.inTransition);
  $('ftbBtn').classList.toggle('active', st.ftb);
  $('ftbBtn').textContent = st.ftb ? 'BLACK ON AIR' : 'FADE TO BLACK';
  // Snap the T-bar home once a transition lands (unless the user holds it).
  if (!st.inTransition && !S.tbarDown && $('tbar').value !== '0') $('tbar').value = 0;
  st.dsk.forEach((d, k) => {
    const card = $(`dsk${k}`);
    if (!card) return;
    const take = card.querySelector('.key-button');
    take.classList.toggle('active', d.on);
    take.textContent = d.on ? 'ON AIR' : 'TAKE';
    card.querySelector('.tie').classList.toggle('active', d.tie);
    card.querySelector('.afv').classList.toggle('active', d.afv);
    const src = card.querySelector('select');
    if (!S.editing.has(src) && document.activeElement !== src && src.value !== String(d.src - 1))
      src.value = String(d.src - 1);
  });
  refreshRecording(st.record, $('recBtn'), $('recState'), '●  RECORD');
  refreshRecording(st.cleanRecord, $('cleanBtn'), $('cleanState'), '●  CLEAN');
  document.querySelectorAll('.channel-strip[data-input]').forEach((strip) => {
    const i = Number(strip.dataset.input);
    const inp = st.inputs[i];
    if (!inp) return;
    strip.querySelector('.mute').classList.toggle('active', !!inp.mute);
    strip.querySelector('.solo').classList.toggle('active', !!inp.solo);
    const fader = strip.querySelector('.fader');
    if (!S.editing.has(fader) && typeof inp.gain === 'number') {
      const db = inp.gain <= 0.001 ? -600 : Math.round(200 * Math.log10(inp.gain));
      fader.value = db;
      strip.querySelector('.gain-readout').textContent = gainText(db);
    }
  });
  refreshMediaRows();
}

function refreshRecording(r, button, label, idleText) {
  if (r.pending) {
    label.dataset.state = 'pending';
    label.textContent = r.active ? 'STOPPING…' : 'STARTING…';
    return;
  }
  button.classList.toggle('active', r.active);
  button.innerHTML = r.active ? '■&nbsp; STOP' : idleText.replace(/ {2}/, '&nbsp; ');
  if (r.error) { label.dataset.state = 'error'; label.textContent = 'RECORD ERROR'; label.title = r.path || ''; }
  else if (r.active) { label.dataset.state = 'recording'; label.textContent = `REC  ${r.frames}`; label.title = r.path || ''; }
  else { label.dataset.state = 'idle'; label.textContent = 'IDLE'; label.title = ''; }
}

function refreshBusReadouts() {
  if (!S.state) return;
  const nameAt = (i) => (i >= 0 && i < S.names.length ? S.names[i].substring(0, 24) : '—');
  const pgm = S.state.program - 1, pvw = S.state.preview - 1;
  $('pgmReadout').textContent = `PGM  ${pad2(pgm + 1)}  ${nameAt(pgm).toUpperCase()}`;
  $('pvwReadout').textContent = `PVW  ${pad2(pvw + 1)}  ${nameAt(pvw).toUpperCase()}`;
}

// ------------------------------------------------------------- ui doc ----
function onUi(ui) {
  S.ui = ui;
  if (ui.inputCount !== S.inputCount) {
    S.inputCount = ui.inputCount;
    buildBuses(ui.inputCount);
    buildInputGrid(ui.inputCount);
    buildKeyers(ui.inputCount);
    buildMixer(ui);
    buildMediaRows(ui.inputCount);
  }
  S.names = ui.inputs.map((i) => i.name);
  refreshNames();
  refreshBusReadouts();
  if (!S.editing.has($('transType')) && $('transType').value !== String(ui.transType)) $('transType').value = String(ui.transType);
  if (!S.editing.has($('transDur'))) $('transDur').value = ui.transDur;
  ui.dsk.forEach((d, k) => {
    const card = $(`dsk${k}`);
    if (!card) return;
    const fade = card.querySelector('input[type=number]');
    if (!S.editing.has(fade)) fade.value = d.fade;
  });
  refreshHealth(ui.health);
  $('status').textContent = ui.status;
  refreshMixerLive(ui);
  refreshMediaRows();
  refreshSettings(ui);
  if (S.state) onState(S.state);
  drawMultiview();
}

function refreshHealth(problems) {
  const good = !problems || problems.length === 0;
  const banner = $('banner');
  banner.hidden = good;
  if (!good) banner.textContent = `SIGNAL WARNING   ·   ${problems.join('   |   ')}`;
  setBadge($('healthBadge'), good ? 'good' : 'bad', good ? '●  ENGINE ONLINE' : '●  ATTENTION');
}

function refreshNames() {
  const n = S.names.length;
  // Wide bus rows leave fewer pixels per key than the classic 2-6 input show:
  // shorten the mnemonics, then drop to numbered crosspoints.
  const nameChars = n > 12 ? 0 : n > 8 ? 7 : 14;
  ['pgmKeys', 'pvwKeys'].forEach((id) => {
    const keys = $(id);
    keys.classList.toggle('compact', nameChars === 0);
    keys.querySelectorAll('.source-button').forEach((b, i) => {
      const name = S.names[i] || '';
      const text = nameChars === 0 ? String(i + 1) : `${pad2(i + 1)}\n${name.substring(0, nameChars).toUpperCase()}`;
      if (b.textContent !== text) b.textContent = text;
      b.title = `Send ${name} to ${id === 'pgmKeys' ? 'PROGRAM' : 'PREVIEW'}`;
    });
  });
  document.querySelectorAll('.input-name').forEach((b) => {
    const i = Number(b.dataset.input);
    const inp = S.ui.inputs[i];
    if (!inp) return;
    b.textContent = inp.name;
    b.title = `Configure source and frame sync for input ${i + 1}`;
    b.classList.toggle('dark', !inp.ref);
    b.classList.toggle('down', !!inp.ref && !inp.connected);
  });
  // Never rewrite an open dropdown's options: Chromium closes the popup the
  // moment an option changes, which made the keyer source unpickable.
  document.querySelectorAll('#keyerCards select').forEach((sel) => {
    if (S.editing.has(sel) || document.activeElement === sel) return;
    Array.from(sel.options).forEach((o, i) => {
      const text = `${pad2(i + 1)} · ${(S.names[i] || '').substring(0, 12)}`;
      if (o.textContent !== text) o.textContent = text;
    });
  });
}

// ------------------------------------------------------------- buses -----
function buildBuses(n) {
  ['pgmKeys', 'pvwKeys'].forEach((id) => {
    const keys = $(id);
    keys.innerHTML = '';
    for (let i = 0; i < n; i++) {
      const b = el('button', 'source-button', String(i + 1));
      b.dataset.tally = 'idle';
      b.addEventListener('click', () => send(`${id === 'pgmKeys' ? 'PGM' : 'PVW'} ${i + 1}`));
      keys.appendChild(b);
    }
  });
}

// ------------------------------------------------------------ inputs -----
function buildInputGrid(n) {
  const grid = $('inputGrid');
  grid.innerHTML = '';
  for (let i = 0; i < n; i++) {
    const card = el('div', 'keyer-card');
    card.appendChild(el('div', 'keyer-title', `INPUT ${pad2(i + 1)}`));
    const b = el('button', 'name-button input-name', 'BLACK');
    b.dataset.input = i;
    b.addEventListener('click', () => openPicker(i));
    card.appendChild(b);
    grid.appendChild(card);
  }
}

// ------------------------------------------------------------ keyers -----
function buildKeyers(n) {
  const cards = $('keyerCards');
  cards.innerHTML = '';
  for (let k = 0; k < 2; k++) {
    const card = el('div', 'keyer-card');
    card.id = `dsk${k}`;
    card.appendChild(el('div', 'keyer-title', `DSK ${k + 1}`));
    const settings = el('div', 'row');
    const src = el('select');
    src.title = 'Key source';
    for (let i = 0; i < n; i++) { const o = el('option', '', pad2(i + 1)); o.value = String(i); src.appendChild(o); }
    src.addEventListener('change', () => send(`DSK ${k + 1} SRC ${Number(src.value) + 1}`));
    trackEditing(src);
    settings.appendChild(src);
    const fade = el('input');
    fade.type = 'number'; fade.min = 1; fade.max = 600; fade.value = 30; fade.title = 'Key fade duration (frames)';
    fade.addEventListener('change', () => send(`DSK ${k + 1} FADE ${clamp(Number(fade.value) || 30, 1, 600)}`));
    trackEditing(fade);
    settings.appendChild(fade);
    settings.appendChild(el('span', 'unit', 'fr'));
    card.appendChild(settings);
    const options = el('div', 'row');
    const tie = el('button', 'key-option tie', 'TIE');
    tie.title = 'Tie to transition: this keyer rides the next AUTO / CUT / T-bar move';
    tie.addEventListener('click', () => send(`DSK ${k + 1} TIE`));
    const afv = el('button', 'key-option afv', 'AUD FOLLOW');
    afv.title = "Audio follows key: the key source's audio fades in and out with the keyer";
    afv.addEventListener('click', () => send(`DSK ${k + 1} AFV`));
    options.appendChild(tie); options.appendChild(afv);
    card.appendChild(options);
    const take = el('button', 'key-button', 'TAKE');
    take.addEventListener('click', () => send(`DSK ${k + 1} TOGGLE`));
    card.appendChild(take);
    cards.appendChild(card);
  }
}

// ------------------------------------------------------------- mixer -----
const gainText = (tenths) => (tenths <= -600 ? '−∞ dB' : `${(tenths / 10).toFixed(1)} dB`);
const dbFor = (lin) => (lin > 1e-6 ? 20 * Math.log10(lin) : -120);

function buildMixer(ui) {
  const strips = $('strips');
  strips.innerHTML = '';
  $('audioTabButton').hidden = !ui.audio.available;
  if (!ui.audio.available) return;
  const n = ui.inputCount;
  S.meters = new Array(n * 2 + 2).fill(0);
  S.meterHold = new Array(n * 2 + 2).fill(0).map(() => ({ v: 0, age: 0 }));
  for (let i = 0; i < n; i++) {
    const strip = el('div', 'channel-strip');
    strip.dataset.input = i;
    strip.appendChild(el('div', 'channel-index', `INPUT ${pad2(i + 1)}`));
    const name = el('button', 'name-button input-name', 'BLACK');
    name.dataset.input = i;
    name.addEventListener('click', () => openPicker(i));
    strip.appendChild(name);
    const mf = el('div', 'meter-fader');
    const scale = el('div', 'meter-scale');
    ['0', '−6', '−18', '−36', '−60'].forEach((t) => scale.appendChild(el('span', '', t)));
    mf.appendChild(scale);
    const meter = el('canvas', 'meter');
    meter.width = 30; meter.height = 120;
    meter.dataset.meter = i;
    mf.appendChild(meter);
    const fader = el('input', 'fader');
    fader.type = 'range'; fader.min = -600; fader.max = 100; fader.value = 0; fader.title = 'Fader';
    let lastSend = 0, pending = null;
    const push = () => {
      const v = Number(fader.value);
      strip.querySelector('.gain-readout').textContent = gainText(v);
      const lin = v <= -600 ? 0 : Math.pow(10, v / 10 / 20);
      send(`AUDIO ${i + 1} GAIN ${lin.toFixed(4)}`);
    };
    fader.addEventListener('input', () => {
      const now = performance.now();
      if (now - lastSend > 40) { lastSend = now; push(); clearTimeout(pending); pending = null; }
      else if (!pending) pending = setTimeout(() => { pending = null; push(); }, 40);
    });
    trackEditing(fader, true);
    mf.appendChild(fader);
    strip.appendChild(mf);
    strip.appendChild(el('div', 'gain-readout', '0.0 dB'));
    const toggles = el('div', 'toggles');
    const mute = el('button', 'mixer-toggle mute', 'MUTE');
    mute.addEventListener('click', () => send(`AUDIO ${i + 1} MUTE TOGGLE`));
    const solo = el('button', 'mixer-toggle solo', 'SOLO');
    solo.addEventListener('click', () => send(`AUDIO ${i + 1} SOLO TOGGLE`));
    toggles.appendChild(mute); toggles.appendChild(solo);
    strip.appendChild(toggles);
    const delayRow = el('div', 'delay-row');
    delayRow.appendChild(el('span', 'channel-index', 'DELAY'));
    const delay = el('input');
    delay.type = 'number'; delay.min = 0; delay.max = 500; delay.value = 0; delay.title = 'Manual audio delay trim (ms)';
    delay.addEventListener('change', () => cmd({ cmd: 'audioDelay', input: i + 1, ms: clamp(Number(delay.value) || 0, 0, 500) }));
    trackEditing(delay);
    delayRow.appendChild(delay);
    delayRow.appendChild(el('span', 'unit', 'ms'));
    strip.appendChild(delayRow);
    const trim = el('div', 'trim-readout', 'AUTO —');
    trim.title = 'Frame-sync auto A/V trim applied on top of manual delay';
    strip.appendChild(trim);
    strips.appendChild(strip);
  }
  const master = el('div', 'channel-strip master');
  master.appendChild(el('div', 'channel-index', 'PROGRAM'));
  master.appendChild(el('div', 'section-title', 'MASTER')).style.textAlign = 'center';
  const mf = el('div', 'meter-fader');
  mf.style.justifyContent = 'center';
  const meter = el('canvas', 'meter');
  meter.width = 30; meter.height = 120; meter.dataset.meter = n;
  mf.appendChild(meter);
  master.appendChild(mf);
  master.appendChild(el('div', 'channel-index', 'A/V CALIBRATION'));
  const delayRow = el('div', 'delay-row');
  const delay = el('input');
  delay.type = 'number'; delay.min = 0; delay.max = 200; delay.value = ui.audio.masterDelayMs || 0; delay.title = 'Master output audio delay (ms)';
  delay.id = 'masterDelay';
  delay.addEventListener('change', () => cmd({ cmd: 'masterDelay', ms: clamp(Number(delay.value) || 0, 0, 200) }));
  trackEditing(delay);
  delayRow.appendChild(delay);
  delayRow.appendChild(el('span', 'unit', 'ms'));
  master.appendChild(delayRow);
  strips.appendChild(master);
}

function refreshMixerLive(ui) {
  if (!ui.audio.available) return;
  document.querySelectorAll('.channel-strip[data-input]').forEach((strip) => {
    const inp = ui.inputs[Number(strip.dataset.input)];
    if (!inp) return;
    const delay = strip.querySelector('.delay-row input');
    if (!S.editing.has(delay)) delay.value = inp.delayMs || 0;
    strip.querySelector('.trim-readout').textContent = inp.autoTrimMs > 0 ? `AUTO +${inp.autoTrimMs} ms` : 'AUTO —';
  });
  const md = $('masterDelay');
  if (md && !S.editing.has(md)) md.value = ui.audio.masterDelayMs || 0;
}

function onMeters(v) {
  if (!S.meters.length) return;
  for (let i = 0; i < v.length && i < S.meters.length; i++) {
    S.meters[i] = Math.max(v[i], S.meters[i] * 0.8);
    const h = S.meterHold[i];
    if (v[i] >= h.v || ++h.age > 45) { h.v = v[i]; h.age = 0; }
  }
  document.querySelectorAll('canvas.meter').forEach((c) => {
    const i = Number(c.dataset.meter);
    drawMeter(c, S.meters[i * 2], S.meters[i * 2 + 1], S.meterHold[i * 2].v, S.meterHold[i * 2 + 1].v);
  });
}

function drawMeter(canvas, l, r, holdL, holdR) {
  const rect = canvas.getBoundingClientRect();
  const h = Math.max(40, Math.floor(rect.height)) || 120;
  if (canvas.height !== h) canvas.height = h;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#08090b';
  ctx.fillRect(0, 0, canvas.width, h);
  const segments = 20, gap = 2, barW = Math.floor((canvas.width - gap - 4) / 2), inner = h - 4, segH = inner / segments;
  const draw = (x, disp, hold) => {
    const db = dbFor(disp);
    for (let s = 0; s < segments; s++) {
      const low = -60 + s * 3;
      const y = h - 2 - Math.round((s + 1) * segH);
      ctx.fillStyle = db >= low ? (low >= -6 ? '#f0363e' : low >= -18 ? '#f0b73c' : '#2fc56e') : '#1b1e22';
      ctx.fillRect(x, y + 1, barW, Math.max(1, Math.floor(segH) - 1));
    }
    const hdb = dbFor(hold);
    if (hdb > -60) {
      const t = clamp((hdb + 60) / 60, 0, 1);
      ctx.fillStyle = '#eef4f8';
      ctx.fillRect(x, clamp(Math.round(inner * (1 - t)) + 2, 1, h - 2), barW, 1);
    }
  };
  draw(2, l, holdL);
  draw(2 + barW + gap, r, holdR);
}

// ------------------------------------------------------------- media -----
function buildMediaRows(n) {
  const rows = $('mediaRows');
  rows.innerHTML = '';
  for (let i = 0; i < n; i++) {
    const row = el('div', 'media-row idle');
    row.dataset.input = i;
    row.appendChild(el('div', 'section-title name', `INPUT ${pad2(i + 1)}   ·   NO MEDIA PLAYLIST`));
    row.appendChild(el('div', 'time', '— / —'));
    const mk = (text, line, cls) => {
      const b = el('button', `action-button ${cls || ''}`, text);
      b.addEventListener('click', () => send(line()));
      row.appendChild(b);
      return b;
    };
    mk('◀  PREV', () => `MEDIA ${i + 1} PREV`, 'prev');
    mk('↺  RESTART', () => `MEDIA ${i + 1} RESTART`);
    mk('PLAY', () => `MEDIA ${i + 1} ${row.dataset.playing === '1' ? 'PAUSE' : 'PLAY'}`, 'play');
    mk('NEXT  ▶', () => `MEDIA ${i + 1} NEXT`, 'next');
    mk('LOOP LIST', () => `MEDIA ${i + 1} LOOP ${row.dataset.loop === '1' ? 'OFF' : 'ON'}`, 'loop');
    rows.appendChild(row);
  }
}

function refreshMediaRows() {
  if (!S.ui) return;
  document.querySelectorAll('.media-row').forEach((row) => {
    const i = Number(row.dataset.input);
    const inp = S.ui.inputs[i];
    const m = inp && inp.media;
    row.classList.toggle('idle', !m);
    if (!m) {
      row.querySelector('.name').textContent = `INPUT ${pad2(i + 1)}   ·   NO MEDIA PLAYLIST`;
      row.querySelector('.time').textContent = '— / —';
      return;
    }
    row.dataset.playing = m.playing ? '1' : '0';
    row.dataset.loop = m.loop ? '1' : '0';
    row.querySelector('.name').textContent = `INPUT ${pad2(i + 1)}   ·   CLIP ${m.index}/${m.size}   ·   MEDIA · ${basename(m.currentRef || '')}`;
    row.querySelector('.time').textContent =
      `${fmtTime(m.positionMs)} / ${fmtTime(m.durationMs)}   ·   ${fmtTime(m.trimInMs)} → ${m.trimOutMs > 0 ? fmtTime(m.trimOutMs) : 'END'}   ·   ×${(m.speed / 1000).toFixed(2)}`;
    const play = row.querySelector('.play');
    play.textContent = m.playing ? 'PAUSE' : m.atEnd ? 'REPLAY' : 'PLAY';
    play.classList.toggle('active', m.playing);
    row.querySelector('.loop').classList.toggle('active', m.loop);
    row.querySelector('.prev').disabled = m.size <= 1;
    row.querySelector('.next').disabled = m.size <= 1;
  });
}

// --------------------------------------------------------- transition ----
function setupTransition() {
  const pushTransition = () => {
    const names = ['mix', 'wipelr', 'wiperl', 'wipetb', 'wipebt', 'wipebox', 'wipecircle'];
    const dur = clamp(Number($('transDur').value) || 30, 1, 600);
    send(`TRANSITION ${names[Number($('transType').value)] || 'mix'} ${dur}`);
  };
  $('transType').addEventListener('change', pushTransition);
  $('transDur').addEventListener('change', pushTransition);
  trackEditing($('transType'));
  trackEditing($('transDur'));
  $('cutBtn').addEventListener('click', () => send('CUT'));
  $('autoBtn').addEventListener('click', () => send('AUTO'));
  $('ftbBtn').addEventListener('click', () => send('FTB'));
  const tbar = $('tbar');
  tbar.addEventListener('pointerdown', () => { S.tbarDown = true; send('TBAR BEGIN'); });
  let last = 0;
  tbar.addEventListener('input', () => {
    const now = performance.now();
    if (now - last < 16) return;
    last = now;
    send(`TBAR ${(Number(tbar.value) / 1000).toFixed(3)}`);
  });
  const release = () => {
    if (!S.tbarDown) return;
    S.tbarDown = false;
    send(`TBAR ${(Number(tbar.value) / 1000).toFixed(3)}`);
    send('TBAR END');
  };
  tbar.addEventListener('pointerup', release);
  tbar.addEventListener('pointercancel', release);
  window.addEventListener('pointerup', release);
}

// ---------------------------------------------------------- recording ----
function setupRecording() {
  const wire = (button, clean) => {
    button.addEventListener('click', () => {
      const r = S.state && (clean ? S.state.cleanRecord : S.state.record);
      if (r && (r.active || r.pending)) { send(clean ? 'CLEAN STOP' : 'RECORD STOP'); return; }
      const dir = (S.ui && S.ui.recordDir) || '~/Videos';
      const d = new Date();
      const ts = `${d.getFullYear()}${pad2(d.getMonth() + 1)}${pad2(d.getDate())}-${pad2(d.getHours())}${pad2(d.getMinutes())}${pad2(d.getSeconds())}`;
      $('recordTitle').textContent = clean ? 'RECORD CLEAN FEED' : 'RECORD PROGRAM';
      $('recordPath').value = `${dir}/8Kloud-Switcher${clean ? '-Clean' : ''}-${ts}.mkv`;
      const dlg = $('recordDialog');
      dlg.onclose = () => {
        if (dlg.returnValue !== 'ok') return;
        let path = $('recordPath').value.trim();
        if (!path) return;
        if (!/\.mkv$/i.test(path)) path += '.mkv';
        send(`${clean ? 'CLEAN' : 'RECORD'} START ${path}`);
      };
      dlg.returnValue = '';
      dlg.showModal();
      $('recordPath').select();
    });
  };
  wire($('recBtn'), false);
  wire($('cleanBtn'), true);
  $('recordDialog').querySelector('form').addEventListener('submit', () => { $('recordDialog').returnValue = 'ok'; });
  $('recordCancel').addEventListener('click', () => $('recordDialog').close(''));
}

// ------------------------------------------------------------ settings ---
function fillFormatSelects() {
  const res = $('outRes'), fps = $('outFps');
  RESOLUTIONS.forEach(([w, h, label]) => { const o = el('option', '', label); o.value = `${w}x${h}`; res.appendChild(o); });
  FRAME_RATES.forEach(([n, d, label]) => { const o = el('option', '', label); o.value = `${n}/${d}`; fps.appendChild(o); });
  const mv = $('settings').querySelector('[data-setting=mvSize]');
  MV_SIZES.forEach(([w, h]) => { const o = el('option', '', `${w} × ${h}`); o.value = `${w}x${h}`; mv.appendChild(o); });
  const onFormat = () => {
    const [w, h] = res.value.split('x').map(Number);
    const [n, d] = fps.value.split('/').map(Number);
    cmd({ cmd: 'settings', show: { width: w, height: h, fpsN: n, fpsD: d } });
  };
  res.addEventListener('change', onFormat);
  fps.addEventListener('change', onFormat);
  trackEditing(res); trackEditing(fps);
  $('settings').querySelectorAll('[data-setting]').forEach((input) => {
    trackEditing(input);
    input.addEventListener('change', () => {
      const key = input.dataset.setting;
      const msg = { cmd: 'settings' };
      if (key === 'mvSize') { const [w, h] = input.value.split('x').map(Number); msg.mvW = w; msg.mvH = h; }
      else if (key === 'srtKeyframeSec') msg.srtKeyframeMs = Math.round((Number(input.value) || 0) * 1000);
      else if (input.type === 'checkbox') msg[key] = input.checked;
      else if (input.type === 'number' || input.dataset.numeric !== undefined) msg[key] = Number(input.value) || 0;
      else msg[key] = input.value.trim();
      cmd(msg);
    });
  });
}

function ensureOption(select, value, label) {
  if (!Array.from(select.options).some((o) => o.value === value)) {
    const o = el('option', '', label);
    o.value = value;
    select.appendChild(o);
  }
}

function refreshSettings(ui) {
  const s = ui.settings;
  const p = s.pending, a = s.active;
  const res = $('outRes'), fps = $('outFps');
  ensureOption(res, `${p.show.width}x${p.show.height}`, `${p.show.width} × ${p.show.height}  CUSTOM`);
  ensureOption(fps, `${p.show.fpsN}/${p.show.fpsD}`, `${(p.show.fpsN / p.show.fpsD).toFixed(3).replace(/\.?0+$/, '')}p  CUSTOM`);
  if (!S.editing.has(res)) res.value = `${p.show.width}x${p.show.height}`;
  if (!S.editing.has(fps)) fps.value = `${p.show.fpsN}/${p.show.fpsD}`;
  const formatPending = p.show.width !== a.show.width || p.show.height !== a.show.height || p.show.fpsN !== a.show.fpsN || p.show.fpsD !== a.show.fpsD;
  const fmt = $('fmtState');
  fmt.dataset.state = formatPending ? 'pending' : 'active';
  fmt.textContent = formatPending ? 'RESTART TO APPLY' : 'ACTIVE';
  fmt.title = formatPending
    ? `Saved for the next start. The engine is currently running at ${a.show.width} × ${a.show.height}, ${(a.show.fpsN / a.show.fpsD).toFixed(2)}p.`
    : 'This is the format currently used by the engine.';
  const st = $('settingsState');
  st.dataset.state = s.restartRequired ? 'pending' : 'active';
  st.textContent = s.restartRequired ? 'RESTART TO APPLY' : 'ACTIVE';
  $('settings').querySelectorAll('[data-setting]').forEach((input) => {
    if (S.editing.has(input)) return;
    const key = input.dataset.setting;
    if (key === 'mvSize') { ensureOption(input, `${p.mvW}x${p.mvH}`, `${p.mvW} × ${p.mvH}`); input.value = `${p.mvW}x${p.mvH}`; }
    else if (key === 'srtSend') input.checked = !!(p.srtSend && p.srtOut);  // parked URL reads as off
    else if (key === 'srtKeyframeSec') input.value = p.srtKeyframeMs ? p.srtKeyframeMs / 1000 : '';
    else if (input.type === 'checkbox') input.checked = !!p[key];
    else if (input.tagName === 'SELECT') input.value = String(p[key] ?? '');
    else if (input.type === 'number') input.value = p[key] || '';
    else input.value = p[key] || '';
  });
  // The auto bitrate the encoder would pick for the pending format, so the
  // empty field says what it means.
  const autoKbps = Math.max(8000, Math.round(p.show.width * p.show.height * (p.show.fpsN / p.show.fpsD) * 0.04 / 1000));
  $('settings').querySelector('[data-setting=srtBitrateKbps]').placeholder = `auto ${autoKbps}`;
  $('settings').querySelector('[data-setting=recordBitrateKbps]').placeholder = `auto ${autoKbps}`;
  const live = (id, o, what) => {
    const node = $(id);
    if (!o.configured) { node.textContent = `${what}: off`; node.className = 'live-note'; return; }
    node.textContent = `${what}: ${o.up ? 'UP' : 'DOWN'} · ${o.name} · ${o.frames} frames`;
    node.className = `live-note ${o.up ? 'up' : 'down'}`;
  };
  live('liveOmtOut', ui.outputs.omtOut, 'Live');
  live('liveCleanOmtOut', ui.outputs.cleanOmtOut, 'Live');
  live('liveMvOmtOut', ui.outputs.mvOmtOut, `Live (${ui.mv.w} × ${ui.mv.h})`);
  const sdi = $('liveSdiOut');
  sdi.textContent = `Program: ${ui.outputs.sdiOut.configured ? (ui.outputs.sdiOut.up ? 'UP' : 'DOWN') : 'off'} · Clean: ${ui.outputs.cleanSdiOut.configured ? (ui.outputs.cleanSdiOut.up ? 'UP' : 'DOWN') : 'off'}`;
  const srt = $('liveSrt');
  srt.textContent = ui.outputs.srt.configured
    ? `Live: ${ui.outputs.srt.connected ? 'CONNECTED' : 'WAITING FOR PEER'} · ${ui.outputs.srt.frames} frames encoded · ${a.srtOut}`
    : `Live: off${a.srtOut && !a.srtSend ? ' (parked)' : ''}`;
  srt.className = `live-note ${ui.outputs.srt.configured ? (ui.outputs.srt.connected ? 'up' : 'down') : ''}`;
  $('liveRecord').textContent = `Program: ${ui.record.active ? `REC ${ui.record.frames}` : 'idle'} · Clean: ${ui.cleanRecord.active ? `REC ${ui.cleanRecord.frames}` : 'idle'}`;
  $('showPath').textContent = ui.showPath || '(no show file)';
}

// ------------------------------------------------------- source picker ---
const P = { input: -1, playlist: [], mediaChosen: false, stillChosen: false, current: null };

function openPicker(i) {
  const inp = S.ui && S.ui.inputs[i];
  if (!inp) return;
  P.input = i;
  P.current = inp;
  P.playlist = (inp.media ? inp.media.playlist : inp.playlist || []).map((c) => ({ ...c }));
  P.mediaChosen = false;
  P.stillChosen = false;
  $('pickerTitle').textContent = `INPUT ${pad2(i + 1)} SOURCE   ·   ${inp.name}`;
  $('pickerManual').value = '';
  $('pickerSync').value = String(inp.sync);
  renderPickerList();
  renderPlaylist(P.playlist.length ? 0 : -1);
  cmd({ cmd: 'sources' });
  const dlg = $('pickerDialog');
  dlg.returnValue = '';
  dlg.showModal();
}

function onSources(msg) {
  S.sources = { omt: msg.omt || [], decklink: msg.decklink || [] };
  if ($('pickerDialog').open) renderPickerList();
}

function renderPickerList() {
  const list = $('pickerList');
  const selected = list.value;
  list.innerHTML = '';
  const add = (label, type, ref) => {
    const o = el('option', '', label);
    o.value = JSON.stringify({ type, ref });
    list.appendChild(o);
  };
  S.sources.omt.forEach((name) => add(`OMT     ${name}`, 'omt', name));
  S.sources.decklink.forEach((d) => add(`SDI     ${d.label}`, 'decklink', d.ref));
  if (!list.options.length) {
    const o = el('option', '', 'No discovered sources · manual entry is still available');
    o.disabled = true;
    list.appendChild(o);
  }
  if (selected) list.value = selected;
  if (list.selectedIndex < 0 && P.current && P.current.ref) {
    Array.from(list.options).forEach((o) => { try { if (JSON.parse(o.value).ref === P.current.ref) o.selected = true; } catch (e) { /* placeholder */ } });
  }
}

function renderPlaylist(selectIndex) {
  const pl = $('pickerPlaylist');
  pl.innerHTML = '';
  P.playlist.forEach((c, idx) => {
    const o = el('option', '', `${basename(c.path)}   ·   ${fmtTrim(c.in)} → ${fmtTrim(c.out, true)}   ·   ×${(c.speed / 1000).toFixed(2)}   ·   ${c.path.substring(0, c.path.lastIndexOf('/'))}`);
    o.value = String(idx);
    o.title = c.path;
    pl.appendChild(o);
  });
  if (selectIndex >= 0 && selectIndex < P.playlist.length) pl.selectedIndex = selectIndex;
  loadTrim();
}

function loadTrim() {
  const idx = $('pickerPlaylist').selectedIndex;
  const valid = idx >= 0 && idx < P.playlist.length;
  ['trimIn', 'trimOut', 'trimSpeed'].forEach((id) => { $(id).disabled = !valid; });
  if (!valid) { $('trimIn').value = ''; $('trimOut').value = ''; $('trimSpeed').value = 100; return; }
  const c = P.playlist[idx];
  $('trimIn').value = c.in;
  $('trimOut').value = c.out;
  $('trimSpeed').value = Math.round(c.speed / 10);
}

function storeTrim() {
  const idx = $('pickerPlaylist').selectedIndex;
  if (idx < 0 || idx >= P.playlist.length) return;
  const c = P.playlist[idx];
  c.in = Math.max(0, Math.round(Number($('trimIn').value) || 0));
  let out = Math.max(0, Math.round(Number($('trimOut').value) || 0));
  if (out > 0 && out <= c.in) { out = c.in + 1; $('trimOut').value = out; }
  c.out = out;
  c.speed = clamp(Math.round(Number($('trimSpeed').value) || 100), 25, 400) * 10;
  P.mediaChosen = true;
  renderPlaylist(idx);
}

function setupPicker() {
  const dlg = $('pickerDialog');
  $('pickerRefresh').addEventListener('click', () => cmd({ cmd: 'sources' }));
  $('pickerList').addEventListener('change', () => { P.mediaChosen = false; P.stillChosen = false; $('pickerManual').value = ''; });
  $('pickerList').addEventListener('dblclick', () => { dlg.returnValue = 'ok'; dlg.close('ok'); });
  $('pickerManual').addEventListener('input', () => { P.mediaChosen = false; P.stillChosen = false; });
  $('pickerPlaylist').addEventListener('change', loadTrim);
  ['trimIn', 'trimOut', 'trimSpeed'].forEach((id) => $(id).addEventListener('change', storeTrim));
  $('plRemove').addEventListener('click', () => {
    const idx = $('pickerPlaylist').selectedIndex;
    if (idx < 0) return;
    P.playlist.splice(idx, 1);
    P.mediaChosen = P.playlist.length > 0;
    renderPlaylist(Math.min(idx, P.playlist.length - 1));
  });
  const move = (dir) => {
    const idx = $('pickerPlaylist').selectedIndex, target = idx + dir;
    if (idx < 0 || target < 0 || target >= P.playlist.length) return;
    [P.playlist[idx], P.playlist[target]] = [P.playlist[target], P.playlist[idx]];
    P.mediaChosen = true;
    renderPlaylist(target);
  };
  $('plUp').addEventListener('click', () => move(-1));
  $('plDown').addEventListener('click', () => move(1));
  $('pickerAddClips').addEventListener('click', () => {
    browseFiles('ADD VIDEO CLIPS', true, VIDEO_EXT, (paths) => {
      paths.forEach((p) => P.playlist.push({ path: p, in: 0, out: 0, speed: 1000 }));
      P.mediaChosen = true; P.stillChosen = false;
      $('pickerManual').value = '';
      $('pickerList').selectedIndex = -1;
      $('pickerSync').value = '-1';
      renderPlaylist(P.playlist.length - 1);
    });
  });
  $('pickerAddStill').addEventListener('click', () => {
    browseFiles('LOAD STILL IMAGE', false, STILL_EXT, (paths) => {
      if (!paths.length) return;
      $('pickerManual').value = paths[0];
      P.mediaChosen = false; P.stillChosen = true;
      $('pickerList').selectedIndex = -1;
      $('pickerSync').value = '-1';
    });
  });
  $('pickerBlack').addEventListener('click', () => {
    cmd({ cmd: 'replaceInput', input: P.input + 1, type: 'black', ref: '' });
    dlg.close('');
  });
  $('pickerCancel').addEventListener('click', () => dlg.close(''));
  dlg.querySelector('form').addEventListener('submit', () => { dlg.returnValue = 'ok'; });
  dlg.addEventListener('close', () => {
    if (dlg.returnValue !== 'ok') return;
    applyPicker();
  });
}

function applyPicker() {
  const i = P.input;
  const cur = P.current;
  const sync = Number($('pickerSync').value);
  const manual = $('pickerManual').value.trim();
  if (P.mediaChosen && P.playlist.length) {
    cmd({ cmd: 'playlist', input: i + 1, items: P.playlist, sync });
    return;
  }
  if (manual) {
    cmd({ cmd: 'replaceInput', input: i + 1, type: P.stillChosen ? 'still' : 'auto', ref: manual, sync });
    return;
  }
  const opt = $('pickerList').selectedOptions[0];
  if (opt && !opt.disabled) {
    const pick = JSON.parse(opt.value);
    if (pick.ref !== cur.ref || pick.type !== cur.type || sync !== cur.sync)
      cmd({ cmd: 'replaceInput', input: i + 1, type: pick.type, ref: pick.ref, sync });
    return;
  }
  // Nothing picked: a frame-sync-only edit keeps the source (and playlist).
  if (sync !== cur.sync) cmd({ cmd: 'sync', input: i + 1, sync });
}

// -------------------------------------------------------- file browser ---
const F = { multi: false, filter: null, done: null, path: '' };

function browseFiles(title, multi, filter, done) {
  F.multi = multi; F.filter = filter; F.done = done;
  $('fileTitle').textContent = title;
  $('fileFiles').multiple = multi;
  $('fileHint').textContent = multi ? 'Select one or more clips (Ctrl / Shift for several)' : 'Select one image';
  const dlg = $('fileDialog');
  dlg.returnValue = '';
  dlg.showModal();
  cmd({ cmd: 'ls', path: F.path || (S.ui && S.ui.recordDir) || '' });
}

function onListing(msg) {
  if (!$('fileDialog').open) return;
  if (msg.error) { flashStatus(`${msg.path}: ${msg.error}`); return; }
  F.path = msg.path;
  F.parent = msg.parent;
  $('filePath').value = msg.path;
  const dirs = $('fileDirs');
  dirs.innerHTML = '';
  msg.dirs.forEach((d) => { const o = el('option', '', `📁 ${d}`); o.value = d; dirs.appendChild(o); });
  const files = $('fileFiles');
  files.innerHTML = '';
  msg.files.filter((f) => !F.filter || F.filter.test(f.name)).forEach((f) => {
    const mb = f.size >= 1048576 ? `${(f.size / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(f.size / 1024))} KB`;
    const o = el('option', '', `${f.name}   ·   ${mb}`);
    o.value = f.name;
    files.appendChild(o);
  });
}

function setupFileBrowser() {
  const dlg = $('fileDialog');
  const go = () => cmd({ cmd: 'ls', path: $('filePath').value.trim() });
  $('fileGo').addEventListener('click', go);
  $('filePath').addEventListener('keydown', (ev) => { if (ev.key === 'Enter') { ev.preventDefault(); go(); } });
  $('fileUp').addEventListener('click', () => { if (F.parent) cmd({ cmd: 'ls', path: F.parent }); });
  $('fileDirs').addEventListener('dblclick', () => {
    const d = $('fileDirs').value;
    if (d) cmd({ cmd: 'ls', path: `${F.path.replace(/\/$/, '')}/${d}` });
  });
  $('fileFiles').addEventListener('dblclick', () => { dlg.returnValue = 'ok'; dlg.close('ok'); });
  $('fileCancel').addEventListener('click', () => dlg.close(''));
  dlg.querySelector('form').addEventListener('submit', () => { dlg.returnValue = 'ok'; });
  dlg.addEventListener('close', () => {
    if (dlg.returnValue !== 'ok') return;
    const base = F.path.replace(/\/$/, '');
    const paths = Array.from($('fileFiles').selectedOptions).map((o) => `${base}/${o.value}`);
    if (F.done) F.done(F.multi ? paths : paths.slice(0, 1));
  });
}

// --------------------------------------------------------------- misc ----
function trackEditing(node, pointer) {
  node.addEventListener('focus', () => S.editing.add(node));
  node.addEventListener('blur', () => S.editing.delete(node));
  if (pointer) {
    node.addEventListener('pointerdown', () => S.editing.add(node));
    node.addEventListener('pointerup', () => setTimeout(() => { if (document.activeElement !== node) S.editing.delete(node); }, 300));
  }
}

function setupTabs() {
  document.querySelectorAll('.tab-button').forEach((b) => {
    b.addEventListener('click', () => {
      document.querySelectorAll('.tab-button').forEach((x) => x.classList.toggle('active', x === b));
      document.querySelectorAll('.tab-pane').forEach((p) => p.classList.toggle('active', p.id === `tab-${b.dataset.tab}`));
      try { localStorage.setItem('kloud.tab', b.dataset.tab); } catch (e) { /* private mode */ }
    });
  });
  try {
    const saved = localStorage.getItem('kloud.tab');
    const b = saved && document.querySelector(`.tab-button[data-tab="${saved}"]`);
    if (b && !b.hidden) b.click();
  } catch (e) { /* ignore */ }
}

function setupShortcuts() {
  window.addEventListener('keydown', (ev) => {
    const t = ev.target;
    if (document.querySelector('dialog[open]')) return;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) {
      if (t.type === 'range' && (ev.key === ' ' || ev.key === 'Enter')) t.blur();
      else return;
    }
    if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
    if (ev.key === ' ') { ev.preventDefault(); send('CUT'); }
    else if (ev.key === 'Enter') { ev.preventDefault(); send('AUTO'); }
    else if (ev.key === 'f' || ev.key === 'F') send('FTB');
    else if (ev.key === 'd') send('DSK 1 TOGGLE');
    else if (ev.key === 'D') send('DSK 2 TOGGLE');
    else if (ev.code && /^Digit[1-9]$/.test(ev.code)) {
      const n = Number(ev.code.substring(5));
      if (n <= S.inputCount) send(`${ev.shiftKey ? 'PVW' : 'PGM'} ${n}`);
    }
  });
}

function tickClock() {
  const d = new Date();
  $('clock').textContent = `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

window.addEventListener('DOMContentLoaded', () => {
  fillFormatSelects();
  setupMultiview();
  setupTransition();
  setupRecording();
  setupPicker();
  setupFileBrowser();
  setupTabs();
  setupShortcuts();
  tickClock();
  setInterval(tickClock, 1000);
  connect();
});
