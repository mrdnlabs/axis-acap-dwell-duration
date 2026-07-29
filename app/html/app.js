/* Object Dwell Timer — operator page.
 *
 * Talks only to the application's own endpoints, served through the manifest's
 * reverseProxy so the device supplies TLS, authentication and access level:
 *
 *   GET  status         in-zone objects and zone definitions
 *   GET  api/health     whether metadata is actually arriving
 *   GET  api/config     settings           PUT to change them
 *   GET  api/zones      zone polygons      PUT to change them
 *   POST api/test       fire a real event flagged test=true
 *
 * Paths are relative so the page works wherever the app is mounted.
 * Every device-derived value is written with textContent, never innerHTML.
 */

'use strict';

const POLL_MS = 2000;
const TICK_MS = 200;
const HIT_RADIUS = 11;

const ATTRIBUTE_CLASSES = ['Head', 'LicensePlate'];

let lastObjects = [];
let lastPollAt = 0;
let healthOk = false;

let zones = [];
let currentZone = 0;
let dragging = null;

/* ------------------------------------------------------------------ helpers */

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
}

function replaceChildren(parent, nodes) {
    while (parent.firstChild) parent.removeChild(parent.firstChild);
    nodes.forEach((n) => parent.appendChild(n));
}

function shortId(id) {
    return typeof id === 'string' && id.length > 8 ? id.slice(0, 8) : (id || '—');
}

function fmtClock(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return '—';
    const total = Math.floor(seconds);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const mm = String(m).padStart(2, '0');
    const ss = String(s).padStart(2, '0');
    return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

function setResult(id, message, kind) {
    const node = document.getElementById(id);
    node.textContent = message;
    node.className = 'result' + (kind ? ' ' + kind : '');
}

function setLive(ok, message) {
    document.getElementById('dot').className = 'dot ' + (ok ? 'on' : 'off');
    document.getElementById('liveText').textContent = message;
}

/* ------------------------------------------------------------- zone editor */

const canvas = document.getElementById('canvas');
const snapshot = document.getElementById('snapshot');
const ctx = canvas.getContext('2d');

function sizeCanvas() {
    const rect = snapshot.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(rect.width * dpr);
    canvas.height = Math.round(rect.height * dpr);
    canvas.style.width = rect.width + 'px';
    canvas.style.height = rect.height + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    drawZones();
}

function cssSize() {
    return { w: canvas.clientWidth, h: canvas.clientHeight };
}

function toPixels(pt) {
    const { w, h } = cssSize();
    return { x: pt[0] * w, y: pt[1] * h };
}

function toNormalized(x, y) {
    const { w, h } = cssSize();
    return [
        Math.min(1, Math.max(0, x / (w || 1))),
        Math.min(1, Math.max(0, y / (h || 1)))
    ];
}

function drawZones() {
    const { w, h } = cssSize();
    ctx.clearRect(0, 0, w, h);

    zones.forEach((zone, idx) => {
        const active = idx === currentZone;
        const pts = (zone.vertices || []).map(toPixels);
        if (!pts.length) return;

        ctx.beginPath();
        pts.forEach((p, i) => (i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
        ctx.closePath();

        ctx.fillStyle = active ? 'rgba(11,110,196,0.22)' : 'rgba(140,148,158,0.14)';
        ctx.fill();
        ctx.lineWidth = active ? 2 : 1.5;
        ctx.strokeStyle = active ? '#0b6ec4' : 'rgba(140,148,158,0.85)';
        ctx.setLineDash(zone.enabled === false ? [6, 5] : []);
        ctx.stroke();
        ctx.setLineDash([]);

        /* Label the zone at its topmost point so it is readable at a glance. */
        const top = pts.reduce((a, b) => (b.y < a.y ? b : a), pts[0]);
        ctx.font = '600 12px system-ui, sans-serif';
        ctx.fillStyle = active ? '#0b6ec4' : 'rgba(120,128,138,0.95)';
        ctx.fillText(zone.name || ('Zone ' + zone.id), top.x + 6, Math.max(12, top.y - 6));

        if (!active) return;
        pts.forEach((p) => {
            ctx.beginPath();
            ctx.arc(p.x, p.y, 5.5, 0, Math.PI * 2);
            ctx.fillStyle = '#ffffff';
            ctx.fill();
            ctx.lineWidth = 2;
            ctx.strokeStyle = '#0b6ec4';
            ctx.stroke();
        });
    });
}

function vertexAt(x, y) {
    const zone = zones[currentZone];
    if (!zone) return -1;
    const pts = (zone.vertices || []).map(toPixels);
    for (let i = 0; i < pts.length; i++) {
        if (Math.hypot(pts[i].x - x, pts[i].y - y) <= HIT_RADIUS) return i;
    }
    return -1;
}

function localPoint(ev) {
    const rect = canvas.getBoundingClientRect();
    return { x: ev.clientX - rect.left, y: ev.clientY - rect.top };
}

canvas.addEventListener('pointerdown', (ev) => {
    if (ev.button !== 0) return;
    const zone = zones[currentZone];
    if (!zone) return;

    const { x, y } = localPoint(ev);
    const hit = vertexAt(x, y);

    if (hit >= 0) {
        dragging = hit;
        canvas.setPointerCapture(ev.pointerId);
    } else {
        zone.vertices = zone.vertices || [];
        zone.vertices.push(toNormalized(x, y));
        drawZones();
        markZonesDirty();
    }
});

canvas.addEventListener('pointermove', (ev) => {
    if (dragging === null) return;
    const zone = zones[currentZone];
    const { x, y } = localPoint(ev);
    zone.vertices[dragging] = toNormalized(x, y);
    drawZones();
});

canvas.addEventListener('pointerup', (ev) => {
    if (dragging === null) return;
    dragging = null;
    canvas.releasePointerCapture(ev.pointerId);
    markZonesDirty();
});

canvas.addEventListener('contextmenu', (ev) => {
    ev.preventDefault();
    const { x, y } = localPoint(ev);
    const hit = vertexAt(x, y);
    const zone = zones[currentZone];
    if (hit < 0 || !zone) return;

    if (zone.vertices.length <= 3) {
        setResult('zoneResult', 'A polygon needs at least three points.', 'bad');
        return;
    }
    zone.vertices.splice(hit, 1);
    drawZones();
    markZonesDirty();
});

function markZonesDirty() {
    const zone = zones[currentZone];
    const n = zone && zone.vertices ? zone.vertices.length : 0;
    setResult('zoneResult',
              n < 3 ? `${n} point${n === 1 ? '' : 's'} — a polygon needs at least three.`
                    : 'Unsaved changes.',
              n < 3 ? 'bad' : '');
    renderZoneTabs();
}

function renderZoneTabs() {
    const host = document.getElementById('zoneTabs');
    replaceChildren(
        host,
        zones.map((z, idx) => {
            const b = el('button', 'tab' + (idx === currentZone ? ' active' : ''));
            b.type = 'button';
            b.appendChild(el('span', null, z.name || ('Zone ' + z.id)));
            b.appendChild(el('span', 'tab-count', String((z.vertices || []).length)));
            b.addEventListener('click', () => {
                currentZone = idx;
                renderZoneTabs();
                drawZones();
            });
            return b;
        })
    );
}

function nextZoneId() {
    return zones.reduce((max, z) => Math.max(max, z.id || 0), 0) + 1;
}

document.getElementById('addZone').addEventListener('click', () => {
    if (zones.length >= 8) {
        setResult('zoneResult', 'At most eight zones are supported.', 'bad');
        return;
    }
    const id = nextZoneId();
    zones.push({ id, name: 'Zone ' + id, enabled: true, vertices: [] });
    currentZone = zones.length - 1;
    renderZoneTabs();
    drawZones();
    setResult('zoneResult', 'Click the image to place the first point.', '');
});

document.getElementById('deleteZone').addEventListener('click', () => {
    if (zones.length <= 1) {
        setResult('zoneResult', 'At least one zone is required.', 'bad');
        return;
    }
    zones.splice(currentZone, 1);
    currentZone = Math.max(0, currentZone - 1);
    renderZoneTabs();
    drawZones();
    markZonesDirty();
});

document.getElementById('clearPoints').addEventListener('click', () => {
    const zone = zones[currentZone];
    if (!zone) return;
    zone.vertices = [];
    drawZones();
    markZonesDirty();
});

document.getElementById('refreshSnap').addEventListener('click', () => {
    snapshot.src = '/axis-cgi/jpg/image.cgi?resolution=1280x720&t=' + Date.now();
});

document.getElementById('saveZones').addEventListener('click', async () => {
    const bad = zones.find((z) => !z.vertices || z.vertices.length < 3);
    if (bad) {
        setResult('zoneResult', `${bad.name || 'A zone'} needs at least three points.`, 'bad');
        return;
    }

    setResult('zoneResult', 'saving…', '');
    try {
        const res = await fetch('api/zones', {
            method: 'PUT',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(zones)
        });
        const data = await res.json();
        if (!res.ok || data.error) {
            setResult('zoneResult', data.error || ('HTTP ' + res.status), 'bad');
            return;
        }
        zones = data.zones || zones;
        currentZone = Math.min(currentZone, zones.length - 1);
        renderZoneTabs();
        drawZones();
        setResult('zoneResult', 'Zones saved and applied.', 'good');
    } catch (err) {
        setResult('zoneResult', 'Failed: ' + (err.message || err), 'bad');
    }
});

snapshot.addEventListener('load', sizeCanvas);
window.addEventListener('resize', sizeCanvas);

/* ----------------------------------------------------------------- settings */

const form = document.getElementById('settingsForm');

function renderTypeChecks(available, selected) {
    replaceChildren(
        document.getElementById('typeChecks'),
        available.map((name) => {
            const label = el('label', 'check');
            const input = document.createElement('input');
            input.type = 'checkbox';
            input.value = name;
            input.checked = selected.indexOf(name) !== -1;
            label.appendChild(input);
            label.appendChild(el('span', null, name));
            return label;
        })
    );
}

function fillSettings(cfg) {
    renderTypeChecks(cfg.availableTypes || [], cfg.objectTypes || []);
    form.elements.minScore.value = cfg.minScore;
    form.elements.referencePoint.value = cfg.referencePoint;
    form.elements.dwellThreshold.value = cfg.dwellThreshold;
    form.elements.updateInterval.value = cfg.updateInterval;
    form.elements.enterDebounce.value = cfg.enterDebounce;
    form.elements.exitDebounce.value = cfg.exitDebounce;
    form.elements.occlusionMaxGap.value = cfg.occlusionMaxGap;
    form.elements.stationaryHold.value = cfg.stationaryHold;
    form.elements.fallbackToVehicle.checked = !!cfg.fallbackToVehicle;
    form.elements.overlayEnabled.checked = !!cfg.overlayEnabled;
    form.elements.mqttAutoConfigure.checked = !!cfg.mqttAutoConfigure;
}

async function loadSettings() {
    try {
        const res = await fetch('api/config', { credentials: 'same-origin', cache: 'no-store' });
        if (!res.ok) throw new Error('HTTP ' + res.status);
        fillSettings(await res.json());
        setResult('settingsResult', '', '');
    } catch (err) {
        setResult('settingsResult', 'Could not load settings: ' + (err.message || err), 'bad');
    }
}

form.addEventListener('submit', async (ev) => {
    ev.preventDefault();

    const types = Array.from(document.querySelectorAll('#typeChecks input:checked'))
        .map((i) => i.value);
    if (!types.length) {
        setResult('settingsResult', 'Select at least one object type.', 'bad');
        return;
    }

    const payload = {
        objectTypes: types,
        minScore: parseFloat(form.elements.minScore.value),
        referencePoint: form.elements.referencePoint.value,
        dwellThreshold: parseFloat(form.elements.dwellThreshold.value),
        updateInterval: parseFloat(form.elements.updateInterval.value),
        enterDebounce: parseFloat(form.elements.enterDebounce.value),
        exitDebounce: parseFloat(form.elements.exitDebounce.value),
        occlusionMaxGap: parseFloat(form.elements.occlusionMaxGap.value),
        stationaryHold: parseFloat(form.elements.stationaryHold.value),
        fallbackToVehicle: form.elements.fallbackToVehicle.checked,
        overlayEnabled: form.elements.overlayEnabled.checked,
        mqttAutoConfigure: form.elements.mqttAutoConfigure.checked
    };

    setResult('settingsResult', 'saving…', '');
    try {
        const res = await fetch('api/config', {
            method: 'PUT',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        const data = await res.json();
        if (!res.ok || data.error) {
            setResult('settingsResult', data.error || ('HTTP ' + res.status), 'bad');
            return;
        }
        /* Show what the device actually stored — values may have been clamped. */
        if (data.config) fillSettings(data.config);
        setResult('settingsResult', 'Settings saved and applied.', 'good');
    } catch (err) {
        setResult('settingsResult', 'Failed: ' + (err.message || err), 'bad');
    }
});

document.getElementById('reloadSettings').addEventListener('click', loadSettings);

/* ---------------------------------------------------------------- rendering */

function interpolatedElapsed(obj) {
    const drift = (performance.now() - lastPollAt) / 1000;
    return (obj.elapsedSeconds || 0) + (healthOk ? drift : 0);
}

function renderObjects() {
    const tbody = document.querySelector('#objectsTable tbody');

    if (!lastObjects.length) {
        const tr = el('tr', 'empty-row');
        const td = el('td', null, 'nothing in zone');
        td.colSpan = 6;
        tr.appendChild(td);
        replaceChildren(tbody, [tr]);
        return;
    }

    const rows = lastObjects
        .slice()
        .sort((a, b) => (b.elapsedSeconds || 0) - (a.elapsedSeconds || 0))
        .map((o) => {
            const elapsed = interpolatedElapsed(o);
            const overage = o.thresholdExceeded
                ? Math.max(0, (o.overageSeconds || 0) + (elapsed - (o.elapsedSeconds || 0)))
                : 0;

            const tr = el('tr', o.thresholdExceeded ? 'over' : null);
            tr.appendChild(el('td', 'mono', shortId(o.objectId)));
            tr.appendChild(el('td', null, o.objectType || 'Unknown'));
            tr.appendChild(el('td', null, o.zoneName || String(o.zoneId)));
            tr.appendChild(el('td', 'timer', fmtClock(elapsed)));
            tr.appendChild(el('td', o.thresholdExceeded ? 'timer over-value' : 'timer',
                              o.thresholdExceeded ? '+' + fmtClock(overage) : '—'));

            let state = 'in zone';
            if (o.leaving) state = 'leaving';
            else if (o.bridging) state = 'bridging gap';
            else if (!o.present) state = 'not in frame';
            const stateCell = el('td', 'state', state);
            if (o.stationary) stateCell.appendChild(el('span', 'tag', 'stationary'));
            tr.appendChild(stateCell);

            return tr;
        });

    replaceChildren(tbody, rows);
}

function renderHealth(h) {
    const rows = [
        ['Metadata', h.subscribed ? (h.receiving ? 'receiving' : 'subscribed, idle') : 'not subscribed'],
        ['Last frame', Number.isFinite(h.secondsSinceLastFrame) && h.secondsSinceLastFrame >= 0
            ? h.secondsSinceLastFrame.toFixed(1) + ' s ago' : '—'],
        ['Frames', h.framesSeen],
        ['Detections', h.detectionsSeen],
        ['Unclassified', h.unclassified],
        ['Tracked objects', h.tracks],
        ['In zone', h.inZone],
        ['Events emitted', h.eventsEmitted],
        ['Event declarations', `${h.eventsReady}/${h.eventsDeclared}`],
        ['Renames seen', h.renames],
        ['Track ends', h.trackEnds]
    ];

    const nodes = [];
    rows.forEach(([k, v]) => {
        nodes.push(el('dt', null, k));
        nodes.push(el('dd', null, v === undefined || v === null ? '—' : String(v)));
    });
    replaceChildren(document.getElementById('health'), nodes);
}

function renderClasses(classes) {
    const host = document.getElementById('classes');
    const names = Object.keys(classes || {}).sort((a, b) => classes[b] - classes[a]);

    if (!names.length) {
        replaceChildren(host, [el('span', 'empty', 'waiting for detections…')]);
        document.getElementById('classNote').textContent = '';
        return;
    }

    replaceChildren(
        host,
        names.map((name) => {
            const isAttr = ATTRIBUTE_CLASSES.indexOf(name) !== -1;
            const chip = el('span', 'chip' + (isAttr ? ' attr' : ''));
            chip.appendChild(el('span', 'chip-name', name));
            chip.appendChild(el('span', 'chip-count', String(classes[name])));
            return chip;
        })
    );

    const attrs = names.filter((n) => ATTRIBUTE_CLASSES.indexOf(n) !== -1);
    document.getElementById('classNote').textContent = attrs.length
        ? attrs.join(' and ') + ' are attributes of a parent object and carry their own track id. '
          + 'They are excluded from dwell counting so one person is not timed twice.'
        : '';
}

/* -------------------------------------------------------------------- poll */

async function poll() {
    try {
        const [statusRes, healthRes] = await Promise.all([
            fetch('status', { credentials: 'same-origin', cache: 'no-store' }),
            fetch('api/health', { credentials: 'same-origin', cache: 'no-store' })
        ]);

        if (!statusRes.ok) throw new Error('status HTTP ' + statusRes.status);

        const status = await statusRes.json();
        lastObjects = Array.isArray(status.objects) ? status.objects : [];
        lastPollAt = performance.now();

        /* Do not stamp on top of edits in progress. */
        if (!zones.length && Array.isArray(status.zones)) {
            zones = status.zones;
            renderZoneTabs();
            sizeCanvas();
        }

        renderObjects();

        if (healthRes.ok) {
            const health = await healthRes.json();
            healthOk = !!health.ok;
            renderHealth(health);
            renderClasses(health.classesSeen);
            setLive(health.ok,
                    health.ok ? 'receiving metadata'
                              : (health.subscribed ? 'subscribed, no recent frames' : 'not subscribed'));
        } else if (healthRes.status === 401 || healthRes.status === 403) {
            healthOk = true;
            setLive(true, 'running (health needs admin)');
        }

        document.getElementById('stamp').textContent = 'updated ' + new Date().toLocaleTimeString();
    } catch (err) {
        healthOk = false;
        setLive(false, 'application not reachable');
        document.getElementById('stamp').textContent = String(err.message || err);
    }
}

/* --------------------------------------------------------------------- MQTT */

function renderMqtt(m) {
    const status = document.getElementById('mqttStatus');
    if (!m.available) {
        status.textContent =
            'Cannot reach the MQTT configuration — the application has no VAPIX credentials.';
        status.className = 'mqtt-status bad';
    } else if (!m.clientActive) {
        status.textContent =
            'The device MQTT client is not enabled. Configure and enable it under System > MQTT, '
            + 'then return here.';
        status.className = 'mqtt-status warn';
    } else if (!m.clientConnected) {
        status.textContent = 'The device MQTT client is enabled but not connected to its broker.';
        status.className = 'mqtt-status warn';
    } else {
        const extra = m.otherFilters
            ? ` ${m.otherFilters} other event filter${m.otherFilters === 1 ? '' : 's'} left untouched.`
            : '';
        status.textContent = m.configured
            ? 'Connected, and this application’s events are being published.' + extra
            : 'Connected, but this application’s events are not published yet.' + extra;
        status.className = 'mqtt-status ' + (m.configured ? 'good' : 'warn');
    }

    document.getElementById('mqttWildcard').textContent = m.wildcard || '—';
    document.getElementById('mqttSample').textContent = m.samplePayload || '—';

    replaceChildren(
        document.getElementById('mqttTopics'),
        (m.topics || []).map((t, i) => {
            const row = el('div', 'copyrow');
            const label = el('span', 'topic-label');
            label.appendChild(el('span', 'topic-event', t.event));
            label.appendChild(el('span', 'topic-zone', t.zoneName || ('Zone ' + t.zoneId)));
            row.appendChild(label);

            const code = el('code', 'wrap', t.topic);
            code.id = 'mqttTopic' + i;
            row.appendChild(code);

            const btn = el('button', 'copy', 'Copy');
            btn.type = 'button';
            btn.dataset.copy = code.id;
            row.appendChild(btn);
            return row;
        })
    );
}

async function loadMqtt() {
    try {
        const res = await fetch('api/mqtt', { credentials: 'same-origin', cache: 'no-store' });
        if (!res.ok) throw new Error('HTTP ' + res.status);
        renderMqtt(await res.json());
    } catch (err) {
        const status = document.getElementById('mqttStatus');
        status.textContent = 'Could not read MQTT state: ' + (err.message || err);
        status.className = 'mqtt-status bad';
    }
}

async function configureMqtt(enable, button) {
    button.disabled = true;
    setResult('mqttResult', enable ? 'configuring…' : 'removing…', '');
    try {
        const res = await fetch('api/mqtt?enable=' + (enable ? 'true' : 'false'), {
            method: 'POST',
            credentials: 'same-origin'
        });
        const data = await res.json();
        if (!res.ok || data.error) {
            setResult('mqttResult', data.error || ('HTTP ' + res.status), 'bad');
            return;
        }
        renderMqtt(data);
        setResult('mqttResult',
                  enable ? 'Bridge configured. Existing filters were preserved.'
                         : 'Our filters removed. Everything else was left in place.',
                  'good');
    } catch (err) {
        setResult('mqttResult', 'Failed: ' + (err.message || err), 'bad');
    } finally {
        button.disabled = false;
    }
}

document.getElementById('mqttEnable')
    .addEventListener('click', (e) => configureMqtt(true, e.currentTarget));
document.getElementById('mqttDisable')
    .addEventListener('click', (e) => configureMqtt(false, e.currentTarget));

/* Copy buttons. Delegated, because topic rows are rebuilt on every refresh. */
document.addEventListener('click', async (ev) => {
    const btn = ev.target.closest('.copy');
    if (!btn) return;

    const source = document.getElementById(btn.dataset.copy);
    if (!source) return;

    const text = source.textContent;
    try {
        await navigator.clipboard.writeText(text);
    } catch (err) {
        /* Clipboard access needs a secure context; the device may be on plain
         * HTTP. Fall back to selecting the text so it can still be copied. */
        const range = document.createRange();
        range.selectNodeContents(source);
        const sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(range);
    }

    const original = btn.textContent;
    btn.textContent = 'Copied';
    setTimeout(() => { btn.textContent = original; }, 1200);
});

/* ------------------------------------------------------------- test buttons */

async function fireTest(kind, button) {
    button.disabled = true;
    setResult('testResult', 'sending…', '');

    try {
        const res = await fetch('api/test?kind=' + encodeURIComponent(kind), {
            method: 'POST',
            credentials: 'same-origin'
        });
        const data = await res.json();
        if (!res.ok || data.error) {
            setResult('testResult', data.error || ('HTTP ' + res.status), 'bad');
        } else {
            setResult('testResult',
                      `Sent ${data.kind} for zone ${data.zoneId} with test=true — check the VMS or broker.`,
                      'good');
        }
    } catch (err) {
        setResult('testResult', 'Failed: ' + (err.message || err), 'bad');
    } finally {
        button.disabled = false;
    }
}

document.querySelectorAll('[data-kind]').forEach((b) => {
    b.addEventListener('click', () => fireTest(b.dataset.kind, b));
});

/* -------------------------------------------------------------------- boot */

loadSettings();
loadMqtt();
poll();
setInterval(poll, POLL_MS);
setInterval(renderObjects, TICK_MS);
/* MQTT state involves several VAPIX round trips — poll it far less often. */
setInterval(loadMqtt, 30000);
