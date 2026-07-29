/* Object Dwell Timer — operator page.
 *
 * Reads the application's own endpoints, served through the manifest's
 * reverseProxy so the device supplies TLS, authentication and access level:
 *
 *   GET  status         current in-zone objects and the zone definitions
 *   GET  api/health     whether the metadata subscription is actually working
 *   POST api/test       fire a real event flagged test=true
 *
 * Paths are relative, so the page works wherever the app is mounted.
 *
 * Every device-derived value is written with textContent, never innerHTML.
 */

'use strict';

const POLL_MS = 2000;
const TICK_MS = 200;

/* Attribute detections carry their own track id but are not independent
 * objects — one person emits both a Human and a Head track. */
const ATTRIBUTE_CLASSES = ['Head', 'LicensePlate'];

let lastObjects = [];
let lastPollAt = 0;
let healthOk = false;

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

function setLive(ok, message) {
    document.getElementById('dot').className = 'dot ' + (ok ? 'on' : 'off');
    document.getElementById('liveText').textContent = message;
}

/* ---------------------------------------------------------------- rendering */

/** Elapsed advances between polls so the timer reads as live, not as a sample. */
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

            const elapsedCell = el('td', 'timer', fmtClock(elapsed));
            tr.appendChild(elapsedCell);

            tr.appendChild(el('td', o.thresholdExceeded ? 'timer over-value' : 'timer',
                              o.thresholdExceeded ? '+' + fmtClock(overage) : '—'));

            /* Say plainly why an object is still counted when it is not visible. */
            let state = 'in zone';
            if (o.leaving) state = 'leaving';
            else if (o.bridging) state = 'bridging gap';
            else if (!o.present) state = 'not in frame';
            const stateCell = el('td', 'state', state);
            if (o.stationary) {
                stateCell.appendChild(el('span', 'tag', 'stationary'));
            }
            tr.appendChild(stateCell);

            return tr;
        });

    replaceChildren(tbody, rows);
}

function renderZones(zones) {
    const host = document.getElementById('zoneList');
    if (!zones || !zones.length) {
        replaceChildren(host, [el('li', 'empty', 'no zones defined')]);
        return;
    }

    replaceChildren(
        host,
        zones.map((z) => {
            const li = el('li');
            li.appendChild(el('span', 'zone-name', z.name || ('Zone ' + z.id)));
            li.appendChild(el('span', 'zone-meta',
                              `${z.vertices ? z.vertices.length : 0} points`
                              + (z.enabled ? '' : ' · disabled')));
            return li;
        })
    );
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

        renderZones(status.zones);
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
            /* status is viewer-level, health is admin-level — a viewer sees the
               dwell table but not the diagnostics. Say so rather than look broken. */
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

/* ------------------------------------------------------------- test buttons */

async function fireTest(kind, button) {
    const result = document.getElementById('testResult');
    button.disabled = true;
    result.textContent = 'sending…';
    result.className = 'result';

    try {
        const res = await fetch('api/test?kind=' + encodeURIComponent(kind), {
            method: 'POST',
            credentials: 'same-origin'
        });
        const data = await res.json();

        if (!res.ok || data.error) {
            result.textContent = data.error || ('HTTP ' + res.status);
            result.className = 'result bad';
        } else {
            result.textContent =
                `Sent ${data.kind} for zone ${data.zoneId} with test=true — check the VMS or broker.`;
            result.className = 'result good';
        }
    } catch (err) {
        result.textContent = 'Failed: ' + (err.message || err);
        result.className = 'result bad';
    } finally {
        button.disabled = false;
    }
}

document.querySelectorAll('.buttons button').forEach((b) => {
    b.addEventListener('click', () => fireTest(b.dataset.kind, b));
});

poll();
setInterval(poll, POLL_MS);
setInterval(renderObjects, TICK_MS);
