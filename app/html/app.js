/* Object Dwell Timer — operator page.
 *
 * Talks only to the application's own endpoints, served through the manifest's
 * reverseProxy so the device supplies TLS, authentication and access level:
 *
 *   GET  status         in-zone objects and zone definitions
 *   GET  api/health     whether metadata is actually arriving
 *   GET  api/config     settings and the class table   PUT to change them
 *   GET  api/zones      zone polygons                  PUT to change them
 *   GET  api/mqtt       resolved topics and bridge state   POST to configure
 *   POST api/test       fire a real event flagged test=true
 *
 * Paths are relative so the page works wherever the app is mounted.
 * Every device-derived value is written with textContent, never innerHTML.
 */

'use strict';

const POLL_MS = 2000;
const TICK_MS = 200;
const MQTT_MS = 30000;
const HIT_RADIUS = 11;

let zones = [];
let config = null;
let classCounts = {};
let lastObjects = [];
let lastPollAt = 0;
let healthOk = false;
let currentZone = 0;
let dragging = null;

/* ------------------------------------------------------------------ helpers */

const $ = (id) => document.getElementById(id);

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
    const t = Math.floor(seconds);
    const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
    const mm = String(m).padStart(2, '0'), ss = String(s).padStart(2, '0');
    return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

function fmtCount(n) {
    if (!Number.isFinite(n)) return '—';
    if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
    if (n >= 10000) return (n / 1000).toFixed(1) + 'k';
    return String(n);
}

function setResult(id, message, kind) {
    const node = $(id);
    if (!node) return;
    node.textContent = message;
    node.className = 'result' + (kind ? ' ' + kind : '');
}

async function api(path, options) {
    const res = await fetch(path, Object.assign({ credentials: 'same-origin', cache: 'no-store' }, options));
    let data = null;
    try { data = await res.json(); } catch (e) { /* empty body */ }
    if (!res.ok || (data && data.error)) {
        throw new Error((data && data.error) || ('HTTP ' + res.status));
    }
    return data;
}

/** Classes currently enabled globally — the pool a zone can pick from. */
function enabledClasses() {
    return config ? config.classes.filter((c) => c.enabled) : [];
}

function displayName(cls) {
    if (!config) return cls;
    const c = config.classes.find((x) => x.class === cls);
    return c && c.name ? c.name : cls;
}

/* ------------------------------------------------------------- zone editor */

const canvas = $('canvas');
const snapshot = $('snapshot');
const ctx = canvas.getContext('2d');

function cssSize() { return { w: canvas.clientWidth, h: canvas.clientHeight }; }

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
    renderBadges();
}

function toPixels(pt) { const { w, h } = cssSize(); return { x: pt[0] * w, y: pt[1] * h }; }

function toNormalized(x, y) {
    const { w, h } = cssSize();
    return [Math.min(1, Math.max(0, x / (w || 1))), Math.min(1, Math.max(0, y / (h || 1)))];
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

        ctx.fillStyle = active ? 'rgba(11,110,196,.20)' : 'rgba(90,100,110,.16)';
        ctx.fill();
        ctx.lineWidth = active ? 3 : 2.5;
        ctx.strokeStyle = active ? '#0b6ec4' : 'rgba(70,80,90,.8)';
        ctx.setLineDash(zone.enabled === false ? [9, 7] : []);
        ctx.stroke();
        ctx.setLineDash([]);

        if (!active) return;
        pts.forEach((p) => {
            ctx.beginPath();
            ctx.arc(p.x, p.y, 8, 0, Math.PI * 2);
            ctx.fillStyle = '#fff';
            ctx.fill();
            ctx.lineWidth = 3;
            ctx.strokeStyle = '#0b6ec4';
            ctx.stroke();
        });
    });
}

function renderLegend() {
    replaceChildren($('legend'), zones.map((z) => {
        const s = el('span', z.enabled === false ? 'off' : null);
        s.appendChild(el('i'));
        s.appendChild(document.createTextNode(
            (z.name || ('Zone ' + z.id)) + (z.enabled === false ? ' · off' : '')));
        return s;
    }));
}

function renderBadges() {
    const { w, h } = cssSize();
    if (!w || !h) return;
    replaceChildren($('badges'), lastObjects.map((o) => {
        const elapsed = interpolated(o);
        const b = el('div', 'badge' + (o.thresholdExceeded ? ' over' : ''));
        b.style.left = ((o.x || 0.5) * w) + 'px';
        b.style.top = (((o.y || 0.5) * h) - 10) + 'px';
        b.appendChild(el('span', 'who', o.objectType || 'Object'));
        b.appendChild(document.createTextNode(fmtClock(elapsed)));
        if (o.thresholdExceeded) {
            b.appendChild(el('span', 'plus', '+' + fmtClock(overageOf(o, elapsed))));
        }
        return b;
    }));
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
    const r = canvas.getBoundingClientRect();
    return { x: ev.clientX - r.left, y: ev.clientY - r.top };
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
        zonesDirty();
    }
});

canvas.addEventListener('pointermove', (ev) => {
    if (dragging === null) return;
    const { x, y } = localPoint(ev);
    zones[currentZone].vertices[dragging] = toNormalized(x, y);
    drawZones();
});

canvas.addEventListener('pointerup', (ev) => {
    if (dragging === null) return;
    dragging = null;
    canvas.releasePointerCapture(ev.pointerId);
    zonesDirty();
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
    zonesDirty();
});

function zonesDirty() {
    const z = zones[currentZone];
    const n = z && z.vertices ? z.vertices.length : 0;
    setResult('zoneResult',
        n < 3 ? `${n} point${n === 1 ? '' : 's'} — a polygon needs at least three.` : 'Unsaved zone changes.',
        n < 3 ? 'bad' : '');
    renderZoneTabs();
    renderZoneRows();
    renderLegend();
}

function renderZoneTabs() {
    replaceChildren($('zoneTabs'), zones.map((z, i) => {
        const b = el('button', 'tab' + (i === currentZone ? ' active' : ''));
        b.type = 'button';
        b.appendChild(el('span', null, z.name || ('Zone ' + z.id)));
        b.appendChild(el('b', null, String((z.vertices || []).length)));
        b.addEventListener('click', () => { currentZone = i; renderZoneTabs(); drawZones(); renderZoneRows(); });
        return b;
    }));
}

/* ------------------------------------------------------------- zones table */

function renderZoneRows() {
    replaceChildren($('zoneRows'), zones.map((z, i) => {
        const row = el('div', 'grid-row' + (z.enabled === false ? ' off' : ''));

        /* select + enable */
        const sel = el('div');
        const sw = el('span', 'switch');
        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.checked = z.enabled !== false;
        cb.addEventListener('change', () => { z.enabled = cb.checked; zonesDirty(); drawZones(); });
        sw.appendChild(cb); sw.appendChild(el('i'));
        sel.appendChild(sw);
        row.appendChild(sel);

        /* name */
        const nameCell = el('div');
        const name = document.createElement('input');
        name.type = 'text';
        name.value = z.name || '';
        name.addEventListener('input', () => { z.name = name.value; renderZoneTabs(); renderLegend(); });
        nameCell.appendChild(name);
        nameCell.appendChild(el('span', 'cell-sub', 'zone id ' + z.id));
        row.appendChild(nameCell);

        /* classes */
        const chips = el('div', 'chips');
        const pool = enabledClasses();
        if (!pool.length) {
            chips.appendChild(el('span', 'cell-sub', 'no classes enabled'));
        } else {
            pool.forEach((c) => {
                const on = (z.classes || []).indexOf(c.class) !== -1;
                const chip = el('button', 'chip' + (on ? '' : ' plain'), c.name || c.class);
                chip.type = 'button';
                chip.style.cursor = 'pointer';
                chip.addEventListener('click', () => {
                    z.classes = z.classes || [];
                    const at = z.classes.indexOf(c.class);
                    if (at === -1) z.classes.push(c.class); else z.classes.splice(at, 1);
                    zonesDirty();
                });
                chips.appendChild(chip);
            });
            if (!(z.classes || []).length) {
                chips.appendChild(el('span', 'cell-sub', 'all enabled classes'));
            }
        }
        row.appendChild(chips);

        /* threshold override */
        const thrCell = el('div');
        const thr = document.createElement('input');
        thr.type = 'number'; thr.min = '0'; thr.max = '86400'; thr.step = '1';
        thr.placeholder = config ? String(config.dwellThreshold) : 'default';
        thr.value = (z.dwellThreshold === null || z.dwellThreshold === undefined) ? '' : z.dwellThreshold;
        thr.addEventListener('input', () => {
            z.dwellThreshold = thr.value === '' ? null : parseFloat(thr.value);
            zonesDirty();
        });
        thrCell.appendChild(thr);
        row.appendChild(thrCell);

        row.appendChild(el('span', 'num', String((z.vertices || []).length)));

        const timing = el('div');
        timing.appendChild(el('span', 'cell-sub',
            (z.dwellThreshold ? z.dwellThreshold + ' s' : 'default')));
        row.appendChild(timing);

        if (i === currentZone) row.style.background = 'var(--accent-soft)';
        return row;
    }));
}

$('addZone').addEventListener('click', () => {
    if (zones.length >= 8) { setResult('zoneResult', 'At most eight zones are supported.', 'bad'); return; }
    const id = zones.reduce((m, z) => Math.max(m, z.id || 0), 0) + 1;
    zones.push({ id, name: 'Zone ' + id, enabled: true, vertices: [], classes: [], dwellThreshold: null });
    currentZone = zones.length - 1;
    zonesDirty(); drawZones();
    setResult('zoneResult', 'Click the image to place the first point.', '');
});

$('deleteZone').addEventListener('click', () => {
    if (zones.length <= 1) { setResult('zoneResult', 'At least one zone is required.', 'bad'); return; }
    zones.splice(currentZone, 1);
    currentZone = Math.max(0, currentZone - 1);
    zonesDirty(); drawZones();
});

$('refreshSnap').addEventListener('click', () => {
    snapshot.src = '/axis-cgi/jpg/image.cgi?resolution=1280x720&t=' + Date.now();
});

$('saveZones').addEventListener('click', async () => {
    const bad = zones.find((z) => !z.vertices || z.vertices.length < 3);
    if (bad) { setResult('zoneResult', `${bad.name || 'A zone'} needs at least three points.`, 'bad'); return; }
    setResult('zoneResult', 'saving…', '');
    try {
        const data = await api('api/zones', {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(zones)
        });
        zones = data.zones || zones;
        currentZone = Math.min(currentZone, zones.length - 1);
        zonesDirty(); drawZones();
        setResult('zoneResult', 'Zones saved and applied.', 'good');
    } catch (err) {
        setResult('zoneResult', String(err.message || err), 'bad');
    }
});

snapshot.addEventListener('load', sizeCanvas);
window.addEventListener('resize', sizeCanvas);

/* ----------------------------------------------------------- classes table */

function renderClassRows() {
    if (!config) return;

    const rows = config.classes.map((c) => {
        const row = el('div', 'grid-row' + (c.enabled ? '' : ' off'));

        const sw = el('span', 'switch');
        const cb = document.createElement('input');
        cb.type = 'checkbox'; cb.checked = !!c.enabled;
        cb.dataset.cls = c.class;
        cb.className = 'class-enable';
        cb.addEventListener('change', () => { c.enabled = cb.checked; renderClassRows(); renderZoneRows(); });
        sw.appendChild(cb); sw.appendChild(el('i'));
        row.appendChild(sw);

        row.appendChild(el('span', 'cell-mono', c.class));

        const nameCell = el('div');
        const name = document.createElement('input');
        name.type = 'text'; name.value = c.name || '';
        name.dataset.cls = c.class;
        name.className = 'class-name';
        name.addEventListener('input', () => { c.name = name.value; });
        nameCell.appendChild(name);
        row.appendChild(nameCell);

        /* min confidence: a slider plus the resolved value */
        const scoreCell = el('div');
        scoreCell.style.display = 'flex';
        scoreCell.style.alignItems = 'center';
        scoreCell.style.gap = '8px';
        const range = document.createElement('input');
        range.type = 'range'; range.min = '0'; range.max = '1'; range.step = '0.05';
        const inherited = (c.minScore === null || c.minScore === undefined);
        range.value = inherited ? config.minScore : c.minScore;
        const val = el('span', 'cell-mono', Number(range.value).toFixed(2) + (inherited ? ' *' : ''));
        val.style.minWidth = '46px';
        range.addEventListener('input', () => {
            c.minScore = parseFloat(range.value);
            val.textContent = Number(range.value).toFixed(2);
        });
        scoreCell.appendChild(range);
        scoreCell.appendChild(val);
        row.appendChild(scoreCell);

        row.appendChild(el('span', 'num', fmtCount(classCounts[c.class] || 0)));
        return row;
    });

    (config.excludedClasses || []).forEach((cls) => {
        const row = el('div', 'grid-row excluded');
        row.appendChild(el('span', null, '—'));
        row.appendChild(el('span', 'cell-mono', cls));
        row.appendChild(el('span', null, 'Never timed'));
        row.appendChild(el('span', 'cell-sub', config.excludedReason || 'attribute of a parent object'));
        row.appendChild(el('span', 'num', fmtCount(classCounts[cls] || 0)));
        rows.push(row);
    });

    replaceChildren($('classRows'), rows);

    $('excludedNote').textContent =
        (config.excludedClasses || []).join(' and ') +
        ' are attributes of a parent object and carry their own track id, so timing them would count ' +
        'one person or one car twice. A value marked * inherits the default confidence floor.';
}

$('saveClasses').addEventListener('click', async () => {
    if (!config) return;
    if (!config.classes.some((c) => c.enabled)) {
        setResult('classesResult', 'Enable at least one class, or nothing will ever be timed.', 'bad');
        return;
    }
    setResult('classesResult', 'saving…', '');
    try {
        const data = await api('api/config', {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                classes: config.classes.map((c) => ({
                    class: c.class, name: c.name, enabled: c.enabled, minScore: c.minScore
                })),
                fallbackToVehicle: $('fallbackToVehicle').checked
            })
        });
        if (data.config) applyConfig(data.config);
        setResult('classesResult', 'Classes saved and applied.', 'good');
    } catch (err) {
        setResult('classesResult', String(err.message || err), 'bad');
    }
});

$('discardClasses').addEventListener('click', loadConfig);

/* ---------------------------------------------------------------- settings */

const form = $('settingsForm');

function applyConfig(cfg) {
    config = cfg;
    form.elements.minScore.value = cfg.minScore;
    form.elements.referencePoint.value = cfg.referencePoint;
    form.elements.dwellThreshold.value = cfg.dwellThreshold;
    form.elements.updateInterval.value = cfg.updateInterval;
    form.elements.enterDebounce.value = cfg.enterDebounce;
    form.elements.exitDebounce.value = cfg.exitDebounce;
    form.elements.occlusionMaxGap.value = cfg.occlusionMaxGap;
    form.elements.stationaryHold.value = cfg.stationaryHold;
    form.elements.overlayEnabled.checked = !!cfg.overlayEnabled;
    form.elements.mqttAutoConfigure.checked = !!cfg.mqttAutoConfigure;
    $('fallbackToVehicle').checked = !!cfg.fallbackToVehicle;
    $('thresholdNote').textContent = 'default threshold ' + cfg.dwellThreshold + ' s';
    renderClassRows();
    renderZoneRows();
}

async function loadConfig() {
    try {
        applyConfig(await api('api/config'));
        setResult('settingsResult', '', '');
        setResult('classesResult', '', '');
    } catch (err) {
        setResult('settingsResult', 'Could not load settings: ' + (err.message || err), 'bad');
    }
}

$('saveSettings').addEventListener('click', async () => {
    const payload = {
        minScore: parseFloat(form.elements.minScore.value),
        referencePoint: form.elements.referencePoint.value,
        dwellThreshold: parseFloat(form.elements.dwellThreshold.value),
        updateInterval: parseFloat(form.elements.updateInterval.value),
        enterDebounce: parseFloat(form.elements.enterDebounce.value),
        exitDebounce: parseFloat(form.elements.exitDebounce.value),
        occlusionMaxGap: parseFloat(form.elements.occlusionMaxGap.value),
        stationaryHold: parseFloat(form.elements.stationaryHold.value),
        overlayEnabled: form.elements.overlayEnabled.checked,
        mqttAutoConfigure: form.elements.mqttAutoConfigure.checked
    };
    setResult('settingsResult', 'saving…', '');
    try {
        const data = await api('api/config', {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        if (data.config) applyConfig(data.config);
        setResult('settingsResult', 'Settings saved and applied.', 'good');
    } catch (err) {
        setResult('settingsResult', String(err.message || err), 'bad');
    }
});

$('discardSettings').addEventListener('click', loadConfig);

/* ------------------------------------------------------------- live render */

function interpolated(o) {
    const drift = (performance.now() - lastPollAt) / 1000;
    return (o.elapsedSeconds || 0) + (healthOk ? drift : 0);
}

function thresholdFor(o) {
    const z = zones.find((x) => x.id === o.zoneId);
    if (z && z.dwellThreshold) return z.dwellThreshold;
    return config ? config.dwellThreshold : 0;
}

function overageOf(o, elapsed) {
    const thr = thresholdFor(o);
    return thr > 0 ? Math.max(0, elapsed - thr) : 0;
}

function renderObjects() {
    const host = $('objects');

    if (!lastObjects.length) {
        const e = el('div', 'empty');
        e.appendChild(el('b', null, 'Nothing in zone'));
        e.appendChild(el('span', null, 'Timers start when a selected class enters a zone.'));
        replaceChildren(host, [e]);
        renderBadges();
        return;
    }

    const rows = lastObjects
        .slice()
        .sort((a, b) => (b.elapsedSeconds || 0) - (a.elapsedSeconds || 0))
        .map((o) => {
            const elapsed = interpolated(o);
            const thr = thresholdFor(o);
            const over = o.thresholdExceeded ? overageOf(o, elapsed) : 0;

            const row = el('div', 'obj' + (o.thresholdExceeded ? ' over' : ''));

            const top = el('div', 'obj-top');
            const who = el('div', 'obj-who');
            who.appendChild(el('span', 'obj-name', o.objectType || 'Object'));
            who.appendChild(el('span', 'obj-id', shortId(o.objectId)));
            top.appendChild(who);
            const times = el('div', 'obj-times');
            times.appendChild(el('span', 'obj-elapsed', fmtClock(elapsed)));
            if (o.thresholdExceeded) times.appendChild(el('span', 'obj-over', '+' + fmtClock(over)));
            top.appendChild(times);
            row.appendChild(top);

            const bar = el('div', 'bar');
            const fill = el('i');
            fill.style.width = (thr > 0 ? Math.min(100, (elapsed / thr) * 100) : 0) + '%';
            bar.appendChild(fill);
            row.appendChild(bar);

            const foot = el('div', 'obj-foot');
            foot.appendChild(el('span', null, o.zoneName || ('Zone ' + o.zoneId)));
            let label = 'In zone';
            if (o.thresholdExceeded) label = 'Past threshold';
            else if (o.leaving) label = 'Leaving';
            else if (o.bridging) label = 'Bridging gap';
            else if (!o.present) label = 'Not in frame';
            const st = el('span', 'st');
            st.appendChild(el('span', null, label));
            if (o.stationary) st.appendChild(el('span', 'pill', 'stationary'));
            foot.appendChild(st);
            row.appendChild(foot);

            return row;
        });

    replaceChildren(host, rows);
    renderBadges();
}

function renderVerdict(h) {
    const v = $('verdict');
    const ok = h && h.ok;
    v.className = 'verdict' + (ok ? '' : ' bad');
    $('verdictIcon').textContent = ok ? '✓' : '!';
    $('verdictTitle').textContent = ok ? 'Working' : (h && h.subscribed ? 'No recent metadata' : 'Not working');

    let text;
    if (!h) text = 'Cannot reach the application.';
    else if (!h.subscribed) text = 'Not subscribed to scene metadata. Is a metadata producer running?';
    else if (!h.receiving) text = 'Subscribed, but no frames have arrived recently.';
    else text = `Scene metadata arriving, last frame ${Number(h.secondsSinceLastFrame).toFixed(1)} s ago. `
        + `${h.eventsReady} of ${h.eventsDeclared} event declarations ready.`;
    $('verdictText').textContent = text;

    $('tileInZone').textContent = h ? fmtCount(h.inZone) : '—';
    $('tileEvents').textContent = h ? fmtCount(h.eventsEmitted) : '—';
    $('tileDetections').textContent = h ? fmtCount(h.detectionsSeen) : '—';

    const rows = h ? [
        ['Last frame', Number(h.secondsSinceLastFrame) >= 0 ? Number(h.secondsSinceLastFrame).toFixed(1) + ' s ago' : '—'],
        ['Frames', fmtCount(h.framesSeen)],
        ['Detections', fmtCount(h.detectionsSeen)],
        ['Unclassified', fmtCount(h.unclassified)],
        ['Tracked objects', h.tracks],
        ['Event declarations', h.eventsReady + '/' + h.eventsDeclared],
        ['Renames seen', h.renames],
        ['Track ends', fmtCount(h.trackEnds)],
        ['Events emitted', fmtCount(h.eventsEmitted)]
    ] : [];
    replaceChildren($('counters'), rows.map(([k, val]) => {
        const d = el('div');
        d.appendChild(el('span', null, k));
        d.appendChild(el('b', null, String(val)));
        return d;
    }));

    if (h) $('diagSummary').textContent = fmtCount(h.framesSeen) + ' frames seen';

    const seen = Object.entries(classCounts).sort((a, b) => b[1] - a[1]);
    $('classesSeenNote').textContent = seen.length
        ? 'Classes seen: ' + seen.map(([k, n]) => `${k} ${fmtCount(n)}`).join(' · ')
        : 'No classified detections yet.';
}

function setLive(ok, message) {
    $('dot').className = 'dot ' + (ok ? 'on' : 'off');
    $('liveText').textContent = message;
}

/* -------------------------------------------------------------------- poll */

async function poll() {
    try {
        const [status, health] = await Promise.all([
            api('status'),
            api('api/health').catch(() => null)
        ]);

        lastObjects = Array.isArray(status.objects) ? status.objects : [];
        lastPollAt = performance.now();

        /* Never stamp over edits in progress. */
        if (!zones.length && Array.isArray(status.zones)) {
            zones = status.zones;
            renderZoneTabs(); renderLegend(); renderZoneRows(); sizeCanvas();
        }

        if (health) {
            healthOk = !!health.ok;
            classCounts = health.classesSeen || {};
            renderVerdict(health);
            setLive(health.ok, health.ok ? 'Receiving metadata'
                : (health.subscribed ? 'Subscribed, no recent frames' : 'Not subscribed'));
        } else {
            healthOk = true;
            setLive(true, 'Running (diagnostics need admin)');
        }

        renderObjects();
        $('stamp').textContent = 'updated ' + new Date().toLocaleTimeString();
    } catch (err) {
        healthOk = false;
        setLive(false, 'Application not reachable');
        renderVerdict(null);
        $('stamp').textContent = String(err.message || err);
    }
}

/* --------------------------------------------------------------------- MQTT */

function renderMqtt(m) {
    const s = $('mqttStatus');
    if (!m.available) {
        s.textContent = 'Cannot reach the MQTT configuration — the application has no VAPIX credentials.';
        s.className = 'result bad';
    } else if (!m.clientActive) {
        s.textContent = 'The device MQTT client is not enabled. Configure it under System > MQTT, then return here.';
        s.className = 'result';
    } else if (!m.clientConnected) {
        s.textContent = 'The device MQTT client is enabled but not connected to its broker.';
        s.className = 'result';
    } else {
        const extra = m.otherFilters
            ? ` ${m.otherFilters} other event filter${m.otherFilters === 1 ? '' : 's'} left untouched.`
            : '';
        s.textContent = (m.configured
            ? 'Connected, and this application’s events are being published.'
            : 'Connected, but this application’s events are not published yet.') + extra;
        s.className = 'result ' + (m.configured ? 'good' : '');
    }

    $('mqttSummary').textContent = m.clientConnected
        ? (m.configured ? 'Connected · publishing' : 'Connected · not publishing')
        : 'Not connected';

    $('mqttWildcard').textContent = m.wildcard || '—';
    $('mqttSample').textContent = m.samplePayload || '—';

    replaceChildren($('mqttTopics'), (m.topics || []).map((t, i) => {
        const row = el('div', 'copyrow');
        const label = el('span', 'label');
        label.appendChild(el('b', null, t.event));
        label.appendChild(el('span', null, t.zoneName || ('Zone ' + t.zoneId)));
        row.appendChild(label);
        const code = el('code', 'wrap', t.topic);
        code.id = 'mqttTopic' + i;
        row.appendChild(code);
        const btn = el('button', 'copy tiny', 'Copy');
        btn.type = 'button';
        btn.dataset.copy = code.id;
        row.appendChild(btn);
        return row;
    }));
}

async function loadMqtt() {
    try { renderMqtt(await api('api/mqtt')); }
    catch (err) {
        const s = $('mqttStatus');
        s.textContent = 'Could not read MQTT state: ' + (err.message || err);
        s.className = 'result bad';
    }
}

async function configureMqtt(enable, button) {
    button.disabled = true;
    setResult('mqttResult', enable ? 'configuring…' : 'removing…', '');
    try {
        renderMqtt(await api('api/mqtt?enable=' + (enable ? 'true' : 'false'), { method: 'POST' }));
        setResult('mqttResult', enable
            ? 'Bridge configured. Existing filters were preserved.'
            : 'Our filters removed. Everything else was left in place.', 'good');
    } catch (err) {
        setResult('mqttResult', String(err.message || err), 'bad');
    } finally {
        button.disabled = false;
    }
}

$('mqttEnable').addEventListener('click', (e) => configureMqtt(true, e.currentTarget));
$('mqttDisable').addEventListener('click', (e) => configureMqtt(false, e.currentTarget));

/* Copy buttons — delegated, because topic rows are rebuilt on refresh. */
document.addEventListener('click', async (ev) => {
    const btn = ev.target.closest('.copy');
    if (!btn) return;
    const source = $(btn.dataset.copy);
    if (!source) return;
    try {
        await navigator.clipboard.writeText(source.textContent);
    } catch (err) {
        /* Clipboard needs a secure context; the device may be plain HTTP. */
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

/* ------------------------------------------------------------ test buttons */

document.querySelectorAll('[data-kind]').forEach((b) => {
    b.addEventListener('click', async () => {
        b.disabled = true;
        setResult('testResult', 'sending…', '');
        try {
            const zoneId = zones[currentZone] ? zones[currentZone].id : 1;
            const data = await api(`api/test?kind=${encodeURIComponent(b.dataset.kind)}&zone=${zoneId}`,
                { method: 'POST' });
            setResult('testResult',
                `Sent ${data.kind} for zone ${data.zoneId} with test=true — check the VMS or broker.`, 'good');
        } catch (err) {
            setResult('testResult', String(err.message || err), 'bad');
        } finally {
            b.disabled = false;
        }
    });
});

/* -------------------------------------------------------------------- boot */

loadConfig();
loadMqtt();
poll();
setInterval(poll, POLL_MS);
setInterval(renderObjects, TICK_MS);
setInterval(loadMqtt, MQTT_MS);
