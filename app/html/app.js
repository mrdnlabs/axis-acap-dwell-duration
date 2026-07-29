/* Object Dwell Timer — Phase 0 status page.
 *
 * There is no backend yet. This reads the application's own syslog through
 * VAPIX (same origin, already authenticated by the device web session) and
 * parses the SPIKE_* lines the spike emits. When the Phase 1 status endpoint
 * lands this file switches to it and the layout stays.
 *
 * All device-derived values are written with textContent, never innerHTML.
 */

'use strict';

const LOG_URL = '/axis-cgi/admin/systemlog.cgi?appname=object_dwell_timer';
const REFRESH_MS = 5000;

/* Attribute detections, not independent objects — one person emits a Human
 * track AND a Head track, so these must never be counted as dwelling objects. */
const ATTRIBUTE_CLASSES = ['Head', 'LicensePlate'];
const VEHICLE_CLASSES = ['Truck', 'Car', 'Bus', 'Bike', 'Vehicle', 'VehicleOther'];

/** Parse `key=value` pairs, tolerating [..] and (..) grouped values. */
function parseKv(line) {
    const out = {};
    const re = /([A-Za-z_][A-Za-z0-9_]*)=(\[[^\]]*\]|\([^)]*\)|\S+)/g;
    let m;
    while ((m = re.exec(line)) !== null) {
        out[m[1]] = m[2];
    }
    return out;
}

function num(v, fallback) {
    const n = parseFloat(v);
    return Number.isFinite(n) ? n : fallback;
}

function shortId(id) {
    return typeof id === 'string' && id.length > 8 ? id.slice(0, 8) : (id || '?');
}

function fmtDuration(seconds) {
    if (!Number.isFinite(seconds)) return '—';
    if (seconds < 60) return seconds.toFixed(1) + ' s';
    const m = Math.floor(seconds / 60);
    const s = Math.round(seconds % 60);
    return m + 'm ' + String(s).padStart(2, '0') + 's';
}

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

/* ---------------------------------------------------------------- parsing */

function parseLog(text) {
    const state = {
        topics: [],
        oq1: null,
        classes: {},
        summary: null,
        stationary: new Map(),
        ended: [],
        renames: 0,
        coordSane: null,
        maxGapSeen: 0,
        started: false
    };

    const lines = text.split(/\r?\n/);
    for (const line of lines) {
        if (line.indexOf('SPIKE_') === -1) continue;

        if (line.indexOf('SPIKE_START') !== -1) {
            /* The syslog keeps entries from previous runs. A SPIKE_START marks a
             * fresh process, whose counters restart from zero — so discard what
             * came before rather than blending two sessions into one view. */
            state.topics = [];
            state.oq1 = null;
            state.classes = {};
            state.summary = null;
            state.stationary.clear();
            state.ended = [];
            state.renames = 0;
            state.coordSane = null;
            state.maxGapSeen = 0;
            state.started = true;
        } else if (line.indexOf('SPIKE_TOPIC ') !== -1) {
            const m = line.match(/SPIKE_TOPIC \[\d+\/\d+\]\s+(\S+)/);
            if (m && state.topics.indexOf(m[1]) === -1) state.topics.push(m[1]);
        } else if (line.indexOf('SPIKE_OQ1') !== -1) {
            state.oq1 = parseKv(line.slice(line.indexOf('SPIKE_OQ1')));
        } else if (line.indexOf('SPIKE_CLASSES') !== -1) {
            const m = line.match(/seen=\[([^\]]*)\]/);
            state.classes = {};
            if (m && m[1] && m[1] !== 'none') {
                m[1].split(',').forEach((pair) => {
                    const bits = pair.split(':');
                    if (bits.length === 2) state.classes[bits[0]] = parseInt(bits[1], 10) || 0;
                });
            }
        } else if (line.indexOf('SPIKE_SUMMARY') !== -1) {
            state.summary = parseKv(line.slice(line.indexOf('SPIKE_SUMMARY')));
        } else if (line.indexOf('SPIKE_STATIONARY') !== -1) {
            const kv = parseKv(line.slice(line.indexOf('SPIKE_STATIONARY')));
            if (kv.id) state.stationary.set(kv.id, kv);
        } else if (line.indexOf('SPIKE_TRACKEND') !== -1) {
            const kv = parseKv(line.slice(line.indexOf('SPIKE_TRACKEND')));
            if (kv.id) {
                state.ended.push(kv);
                state.stationary.delete(kv.id);
                const gap = num(kv.maxgap_s, 0);
                if (gap > state.maxGapSeen) state.maxGapSeen = gap;
            }
        } else if (line.indexOf('SPIKE_RENAME') !== -1) {
            state.renames += 1;
        } else if (line.indexOf('SPIKE_NEWTRACK') !== -1) {
            const kv = parseKv(line.slice(line.indexOf('SPIKE_NEWTRACK')));
            if (kv.sanity_bottom_gt_top) {
                state.coordSane = kv.sanity_bottom_gt_top === 'yes' && kv.sanity_right_gt_left === 'yes';
            }
        }
    }
    return state;
}

/* --------------------------------------------------------------- rendering */

function renderOqCards(state) {
    const cards = [];

    const frameOk = state.oq1 && state.oq1.frame_v1 === 'PRESENT';
    cards.push({
        id: 'OQ-1',
        title: 'Metadata source',
        status: state.oq1 ? (frameOk ? 'ok' : 'bad') : 'wait',
        value: state.oq1 ? (frameOk ? 'scene.frame.v1 present' : 'frame.v1 ABSENT') : 'reading…',
        detail: state.oq1
            ? 'object_track.v1 ' + String(state.oq1.object_track_v1 || '?').toLowerCase()
            : 'waiting for topic enumeration'
    });

    cards.push({
        id: 'OQ-2',
        title: 'Coordinate frame',
        status: state.coordSane === null ? 'wait' : (state.coordSane ? 'ok' : 'bad'),
        value: state.coordSane === null ? 'no tracks yet' : (state.coordSane ? 'top-left, Y down' : 'unexpected'),
        detail: state.coordSane === null
            ? 'needs one detection to confirm'
            : 'bottom > top and right > left on every box'
    });

    const seen = Object.keys(state.classes);
    const vehiclesSeen = seen.filter((c) => VEHICLE_CLASSES.indexOf(c) !== -1);
    cards.push({
        id: 'OQ-3',
        title: 'Vehicle sub-types',
        status: vehiclesSeen.length ? 'ok' : 'wait',
        value: vehiclesSeen.length ? vehiclesSeen.join(', ') : 'none seen yet',
        detail: vehiclesSeen.length
            ? 'Truck is the FR-2 default type'
            : 'needs a vehicle in view — play a traffic video'
    });

    cards.push({
        id: 'OQ-4',
        title: 'Track continuity',
        status: state.ended.length ? 'ok' : 'wait',
        value: state.ended.length ? 'max gap ' + fmtDuration(state.maxGapSeen) : 'no ended tracks',
        detail: state.renames
            ? state.renames + ' rename event(s) seen'
            : 'no Rename events — id is reused across gaps instead'
    });

    replaceChildren(
        document.getElementById('oqCards'),
        cards.map((c) => {
            const card = el('div', 'card ' + c.status);
            card.appendChild(el('span', 'card-id', c.id));
            card.appendChild(el('span', 'card-title', c.title));
            card.appendChild(el('strong', 'card-value', c.value));
            card.appendChild(el('span', 'card-detail', c.detail));
            return card;
        })
    );
}

function renderClasses(state) {
    const host = document.getElementById('classes');
    const entries = Object.keys(state.classes).sort((a, b) => state.classes[b] - state.classes[a]);

    if (!entries.length) {
        replaceChildren(host, [el('span', 'empty', 'waiting for detections…')]);
        document.getElementById('classNote').textContent = '';
        return;
    }

    replaceChildren(
        host,
        entries.map((name) => {
            const isAttr = ATTRIBUTE_CLASSES.indexOf(name) !== -1;
            const chip = el('span', 'chip' + (isAttr ? ' attr' : ''));
            chip.appendChild(el('span', 'chip-name', name));
            chip.appendChild(el('span', 'chip-count', String(state.classes[name])));
            return chip;
        })
    );

    const attrs = entries.filter((n) => ATTRIBUTE_CLASSES.indexOf(n) !== -1);
    document.getElementById('classNote').textContent = attrs.length
        ? attrs.join(' and ') + ' are attribute detections carrying their own track id — one person '
          + 'emits both a Human and a Head track. They are excluded from dwell counting so the same '
          + 'object is not timed twice.'
        : '';
}

function renderStationary(state) {
    const tbody = document.querySelector('#liveTable tbody');
    const rows = Array.from(state.stationary.values());

    if (!rows.length) {
        const tr = el('tr', 'empty-row');
        const td = el('td', null, 'no stationary tracks in view');
        td.colSpan = 4;
        tr.appendChild(td);
        replaceChildren(tbody, [tr]);
        return;
    }

    replaceChildren(
        tbody,
        rows.map((r) => {
            const tr = el('tr');
            tr.appendChild(el('td', 'mono', shortId(r.id)));
            tr.appendChild(el('td', null, fmtDuration(num(r.alive_s, NaN))));
            tr.appendChild(el('td', null, num(r.excursion, 0).toFixed(4)));
            tr.appendChild(el('td', null, r.class === 'none' ? '—' : r.class));
            return tr;
        })
    );
}

function renderEnded(state) {
    const tbody = document.querySelector('#endedTable tbody');
    const rows = state.ended.slice(-12).reverse();

    if (!rows.length) {
        const tr = el('tr', 'empty-row');
        const td = el('td', null, 'no completed tracks yet');
        td.colSpan = 6;
        tr.appendChild(td);
        replaceChildren(tbody, [tr]);
        return;
    }

    replaceChildren(
        tbody,
        rows.map((r) => {
            const gap = num(r.maxgap_s, 0);
            const tr = el('tr');
            tr.appendChild(el('td', 'mono', shortId(r.id)));
            tr.appendChild(el('td', null, fmtDuration(num(r.lifetime_s, NaN))));
            tr.appendChild(el('td', null, r.frames || '—'));
            const gapCell = el('td', gap >= 5 ? 'highlight' : null, fmtDuration(gap));
            tr.appendChild(gapCell);
            tr.appendChild(el('td', null, r.stationary === 'yes' ? 'stationary' : 'moving'));
            tr.appendChild(el('td', null, r.class === 'none' ? '—' : r.class));
            return tr;
        })
    );
}

function renderCounters(state) {
    const s = state.summary || {};
    const items = [
        ['Frames', s.frames],
        ['Detections', s.detections],
        ['Unclassified', s.unclassified],
        ['Live tracks', s.live_tracks],
        ['Tracks ended', s.trackends],
        ['Renames', s.renames],
        ['Consolidated tracks', s.objtracks],
        ['Frame rate', s.fps ? s.fps + ' fps' : undefined],
        ['Channel', s.channel_id]
    ];

    const nodes = [];
    items.forEach((pair) => {
        nodes.push(el('dt', null, pair[0]));
        nodes.push(el('dd', null, pair[1] === undefined ? '—' : String(pair[1])));
    });
    replaceChildren(document.getElementById('counters'), nodes);
}

function renderTopics(state) {
    const host = document.getElementById('topics');
    if (!state.topics.length) {
        replaceChildren(host, [el('span', 'empty', 'reading…')]);
        return;
    }
    replaceChildren(
        host,
        state.topics.map((t) => {
            const wanted = t === 'com.axis.scene.frame.v1' || t === 'com.axis.scene.object_track.v1';
            return el('span', 'chip' + (wanted ? ' wanted' : ''), t);
        })
    );
}

function setLive(ok, message) {
    document.getElementById('dot').className = 'dot ' + (ok ? 'on' : 'off');
    document.getElementById('liveText').textContent = message;
}

/* ------------------------------------------------------------------ poll */

async function refresh() {
    try {
        const res = await fetch(LOG_URL, { credentials: 'same-origin', cache: 'no-store' });
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const text = await res.text();
        const state = parseLog(text);

        renderOqCards(state);
        renderClasses(state);
        renderStationary(state);
        renderEnded(state);
        renderCounters(state);
        renderTopics(state);

        setLive(state.started, state.started ? 'running' : 'no log yet');
        document.getElementById('stamp').textContent = 'updated ' + new Date().toLocaleTimeString();
    } catch (err) {
        setLive(false, 'log unavailable');
        document.getElementById('stamp').textContent = String(err.message || err);
    }
}

refresh();
setInterval(refresh, REFRESH_MS);
