// The remnant and safety inspector: a working model of VortexClean's main window.
// The listed items are the engine's own output (demo-data.js). The "left alone"
// rows below are written by hand to illustrate the rules that exclude something,
// because the application itself does not list what it has ruled out.
import { standard, packaged, suite } from './demo-data.js';

const SAMPLES = [
  {
    id: 'standard', data: standard, kind: 'Standard app',
    leftAlone: [
      { path: '~/Library/Caches/com.pixelforge.Studio.Beta', badge: 'LaunchServices Filtered',
        why: 'Registered to PixelForge Studio Beta, an app installed somewhere else on the disk.' },
      { path: '~/Library/Application Support/Helper', badge: 'Too Generic',
        why: '“Helper” is a stop word. A name like this is not evidence.' },
    ],
  },
  {
    id: 'packaged', data: packaged, kind: 'Package install',
    leftAlone: [
      { path: '/System/Library/Extensions/TidewaterFS.kext', badge: 'SIP Locked',
        why: 'The receipt lists it, but it sits in a location that System Integrity Protection guards.' },
      { path: '/usr/local/bin', badge: 'Shared Root',
        why: 'The package wrote into this folder. Only its own file inside it is offered.' },
    ],
  },
  {
    id: 'suite', data: suite, kind: 'App in a suite',
    leftAlone: [
      { path: '~/Library/Caches/com.fernwood.sheets', badge: 'Another App’s',
        why: 'Belongs to Fernwood Sheets, which is still installed.' },
      { path: '~/Library/Caches/com.fernwood.writer.preview', badge: 'LaunchServices Filtered',
        why: 'Registered to Fernwood Writer Preview, an app outside the Applications folder.' },
    ],
  },
];

const CATEGORY = {
  app_bundle: ['Application', 'app'],
  application_support: ['Application Support', 'support'],
  caches: ['Caches', 'caches'],
  preferences: ['Preferences', 'prefs'],
  containers: ['Containers', 'containers'],
  group_containers: ['Group Containers', 'containers'],
  saved_application_state: ['Saved Application State', 'prefs'],
  logs: ['Logs', 'logs'],
  webkit: ['WebKit', 'prefs'],
  http_storages: ['HTTP Storages', 'prefs'],
  cookies: ['Cookies', 'prefs'],
  application_scripts: ['Application Scripts', 'containers'],
  launch_agents: ['Launch Agents', 'system'],
  launch_daemons: ['Launch Daemons', 'system'],
  privileged_helper_tools: ['Privileged Helper Tools', 'system'],
  receipts: ['Package Receipts', 'system'],
  package_payload: ['Package Payload', 'system'],
  quicklook: ['Quick Look Plug-ins', 'system'],
  preference_panes: ['Preference Panes', 'system'],
  frameworks: ['Frameworks', 'system'],
};

const BUCKETS = [
  ['app', 'App Bundle'], ['support', 'App Support'], ['caches', 'Caches'], ['containers', 'Containers'],
  ['prefs', 'Preferences & State'], ['logs', 'Logs'], ['system', 'System & Daemons'],
];

const REASON_KEY = {
  'Application Bundle': 'target_bundle', 'Exact Bundle ID': 'exact_bundle_id', 'Bundle ID Prefix': 'bundle_id_prefix',
  'Embedded Helper ID': 'embedded_helper_id', 'Launchd Job in Bundle': 'launchd_program_in_bundle',
  'BOM Receipt': 'bom_receipt', 'App Name Heuristic': 'app_name_heuristic', 'Vendor Directory': 'vendor_directory',
};

const ICON = {
  folder: 'M1.5 4.2c0-.9.7-1.6 1.6-1.6h3.1c.4 0 .8.2 1.1.5l.8.9h4.8c.9 0 1.6.7 1.6 1.6v6.2c0 .9-.7 1.6-1.6 1.6H3.1c-.9 0-1.6-.7-1.6-1.6V4.2Z',
  file: 'M3.5 2.6c0-.6.5-1.1 1.1-1.1h4.3l3.6 3.6v8.3c0 .6-.5 1.1-1.1 1.1H4.6c-.6 0-1.1-.5-1.1-1.1V2.6Z',
  lock: 'M8 1.4a3.1 3.1 0 0 0-3.1 3.1v1.9h-.4c-.7 0-1.2.5-1.2 1.2v5.2c0 .7.5 1.2 1.2 1.2h7c.7 0 1.2-.5 1.2-1.2V7.6c0-.7-.5-1.2-1.2-1.2h-.4V4.5A3.1 3.1 0 0 0 8 1.4Zm1.7 5H6.3V4.5a1.7 1.7 0 0 1 3.4 0v1.9Z',
  shield: 'M8 1.2 2.8 3.1v4.1c0 3.4 2.1 6 5.2 7.6 3.1-1.6 5.2-4.2 5.2-7.6V3.1L8 1.2Z',
  chevron: 'M5.5 3.2 10.3 8l-4.8 4.8',
  warning: 'M8 1.9c.4 0 .8.2 1 .6l5.4 9.8c.4.8-.1 1.7-1 1.7H2.6c-.9 0-1.4-.9-1-1.7L7 2.5c.2-.4.6-.6 1-.6Zm-.8 4v3.6h1.6V5.9H7.2Zm0 4.7v1.5h1.6v-1.5H7.2Z',
};

// --- Small helpers -------------------------------------------------------------

/// Sizes the way macOS writes them: decimal units, "96 MB", "612.2 MB", "1.67 GB".
export function formatBytes(bytes) {
  if (bytes === 0) return 'Zero KB';
  if (bytes < 1000) return `${bytes} bytes`;
  const [value, unit, digits] = bytes < 1e6 ? [bytes / 1e3, 'KB', 0] : bytes < 1e9 ? [bytes / 1e6, 'MB', 1] : [bytes / 1e9, 'GB', 2];
  return `${Number(value.toFixed(digits))} ${unit}`;
}

const plural = (count, one, many) => `${count} ${count === 1 ? one : many}`;

function h(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) {
    if (value === undefined || value === null || value === false) continue;
    if (name.startsWith('on')) node.addEventListener(name.slice(2), value);
    else if (name === 'class') node.className = value;
    else if (value === true) node.setAttribute(name, '');
    else node.setAttribute(name, String(value));
  }
  node.append(...children.filter((child) => child !== null && child !== undefined));
  return node;
}

function icon(name, className = 'glyph') {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 16 16');
  svg.setAttribute('class', `${className} ${className}--${name}`);
  svg.setAttribute('aria-hidden', 'true');
  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('d', ICON[name]);
  svg.append(path);
  return svg;
}

/// A path that may wrap after a slash, and only mid-word when nothing else fits.
function pathText(path) {
  const span = h('span', { class: 'row__path' });
  path.split(/(?<=\/)/).forEach((part, index) => { span.append(index ? document.createElement('wbr') : '', part); });
  return span;
}

/// Splits pretty-printed JSON into coloured pieces without ever parsing HTML.
function paintJson(target, text) {
  target.replaceChildren();
  const pattern = /("(?:\\.|[^"\\])*")(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d+)?/g;
  let last = 0;
  for (const match of text.matchAll(pattern)) {
    target.append(text.slice(last, match.index));
    const kind = match[1] ? (match[2] ? 'key' : 'string') : match[3] ? 'literal' : 'number';
    const token = match[1] ?? match[0];
    target.append(h('span', { class: `json-${kind}` }, token));
    last = match.index + token.length;
  }
  target.append(text.slice(last));
}

// --- The inspector -------------------------------------------------------------

export function startInspector(mount, { announce, bindCopy }) {
  const state = { sample: null, checked: new Set(), collapsed: new Set(), dryRun: true, highlighted: null, cleaned: false, report: null };
  const refs = {};

  // --- Structure ---

  function build() {
    refs.list = h('div', { class: 'side__list', role: 'radiogroup', 'aria-label': 'Sample application' });
    for (const sample of SAMPLES) {
      const app = sample.data.application;
      const size = sample.data.items.find((item) => item.category === 'app_bundle').allocated_bytes;
      const input = h('input', { type: 'radio', name: 'sample', value: sample.id, id: `sample-${sample.id}`,
        onchange: () => select(sample) });
      refs[`pick-${sample.id}`] = input;
      refs.list.append(h('label', { class: 'side__item', for: `sample-${sample.id}` }, input,
        h('img', { src: `assets/app-${sample.id}.png`, width: 32, height: 32, alt: '' }),
        h('span', { class: 'side__text' }, h('b', {}, app.name), h('small', {}, `${sample.kind} · ${formatBytes(size)}`))));
    }

    refs.icon = h('img', { class: 'detail__icon', width: 60, height: 60, alt: '' });
    refs.name = h('h3', {});
    refs.identifier = h('code', {});
    refs.version = h('p', {});
    refs.total = h('output', { class: 'detail__total', for: 'sample' });
    refs.totalNote = h('p', {});
    refs.summary = h('p', { class: 'storage__summary' });
    refs.bar = h('div', { class: 'storage__bar', role: 'img' });
    refs.tip = h('div', { class: 'storage__tip', 'aria-hidden': 'true' });
    refs.legend = h('div', { class: 'storage__legend' });
    refs.tree = h('div', { class: 'tree' });
    refs.status = h('p', { class: 'foot__status' });
    refs.dry = h('input', { type: 'checkbox', role: 'switch', id: 'dry-run', checked: true,
      onchange: () => { state.dryRun = refs.dry.checked; render(); } });
    refs.run = h('button', { class: 'button button--clean', type: 'button', onclick: simulate }, 'Simulate Clean');
    refs.resultText = h('p', {});
    refs.putBack = h('button', { class: 'button button--small', type: 'button', onclick: putBack }, 'Put Back');
    refs.json = h('code', {});
    refs.copy = h('button', { class: 'copy', type: 'button' }, 'Copy');
    refs.jsonName = h('span', {});
    refs.result = h('section', { class: 'result', hidden: true, 'aria-label': 'Simulated audit log' },
      h('div', { class: 'result__head' }, refs.resultText, refs.putBack),
      h('div', { class: 'code code--json' }, h('p', { class: 'code__name' }, refs.jsonName), h('pre', { tabindex: 0 }, refs.json), refs.copy));

    mount.replaceChildren(
      h('div', { class: 'side' },
        h('div', { class: 'lights', 'aria-hidden': 'true' }, h('i'), h('i'), h('i')), refs.list),
      h('div', { class: 'detail' },
        h('div', { class: 'detail__head' }, refs.icon,
          h('div', { class: 'detail__id' }, refs.name, refs.identifier, refs.version),
          h('div', { class: 'detail__reclaim' }, h('p', { class: 'caps' }, 'Reclaimable'), refs.total, refs.totalNote)),
        h('div', { class: 'storage' }, refs.summary, h('div', { class: 'storage__track' }, refs.bar, refs.tip), refs.legend),
        refs.tree,
        h('div', { class: 'foot' }, refs.status,
          h('label', { class: 'switch', for: 'dry-run' }, h('span', {}, 'Dry Run'), refs.dry), refs.run),
        refs.result));
    bindCopy(refs.copy, refs.json);
  }

  function select(sample) {
    state.sample = sample;
    state.checked = new Set(sample.data.items.filter((item) => item.selected_by_default).map((item) => item.path));
    state.collapsed = new Set();
    state.highlighted = null;
    state.cleaned = false;
    state.report = null;
    refs[`pick-${sample.id}`].checked = true;
    mount.parentElement.dataset.sample = sample.id;  // the wallpaper follows the sample
    renderHead();
    renderTree();
    render();
  }

  const items = () => state.sample.data.items;
  const chosen = () => items().filter((item) => state.checked.has(item.path));
  const sum = (list) => list.reduce((total, item) => total + item.allocated_bytes, 0);

  function groups() {
    const map = new Map();
    for (const item of items()) {
      if (!map.has(item.category)) map.set(item.category, []);
      map.get(item.category).push(item);
    }
    return [...map];
  }

  // --- Rendering ---

  function renderHead() {
    const app = state.sample.data.application;
    const bundle = items().find((item) => item.category === 'app_bundle');
    refs.icon.src = `assets/app-${state.sample.id}.png`;
    refs.name.textContent = app.name;
    refs.identifier.textContent = app.bundle_id;
    refs.version.textContent = `Version ${app.version} · ${formatBytes(bundle.allocated_bytes)} on disk`;
  }

  function badges(item) {
    const list = [];
    if (item.shared) list.push(h('span', { class: 'pill pill--shared', title: item.warning }, icon('warning', 'pill__icon'), 'Shared — Unchecked by Default'));
    const confidence = item.confidence[0].toUpperCase() + item.confidence.slice(1);
    list.push(h('span', { class: `pill pill--${item.confidence}` }, `${confidence} · ${item.reason}`));
    return h('span', { class: 'row__badges' }, ...list);
  }

  function renderTree() {
    refs.tree.replaceChildren();
    refs.groupBoxes = new Map();
    refs.itemBoxes = new Map();
    for (const [category, list] of groups()) {
      const title = CATEGORY[category]?.[0] ?? category;
      const bodyId = `group-${state.sample.id}-${category}`;
      const box = h('input', { type: 'checkbox', 'aria-label': `Select all in ${title}`, onchange: () => {
        for (const item of list) box.checked ? state.checked.add(item.path) : state.checked.delete(item.path);
        render();
      } });
      refs.groupBoxes.set(category, box);
      const body = h('ul', { class: 'group__body', id: bodyId });
      for (const item of list) {
        const id = `item-${items().indexOf(item)}`;
        const check = h('input', { type: 'checkbox', id, onchange: () => {
          check.checked ? state.checked.add(item.path) : state.checked.delete(item.path);
          render();
        } });
        refs.itemBoxes.set(item.path, check);
        const glyph = item.category === 'app_bundle'
          ? h('img', { class: 'row__app', src: `assets/app-${state.sample.id}.png`, width: 16, height: 16, alt: '' })
          : icon(/\.(plist|bom|1)$|\.daemon$|\/tidewater$/.test(item.path) ? 'file' : 'folder');
        body.append(h('li', { class: `row${item.shared ? ' row--shared' : ''}`, 'data-path': item.path },
          check,
          h('label', { class: 'row__label', for: id }, glyph, pathText(item.display_path),
            item.requires_root ? h('span', { class: 'row__root', title: 'Needs an administrator' }, icon('shield'), h('span', { class: 'sr' }, 'Needs an administrator')) : null),
          badges(item),
          h('span', { class: 'row__size' }, formatBytes(item.allocated_bytes))));
      }
      refs.tree.append(group(category, title, box, body, plural(list.length, 'item', 'items'), formatBytes(sum(list))));
    }
    renderLeftAlone();
  }

  function group(key, title, lead, body, count, size) {
    const disclosure = h('button', { class: 'group__toggle', type: 'button', 'aria-expanded': 'true', 'aria-controls': body.id,
      'aria-label': `Show or hide ${title}`, onclick: () => {
        const collapse = !state.collapsed.has(key);
        collapse ? state.collapsed.add(key) : state.collapsed.delete(key);
        disclosure.setAttribute('aria-expanded', String(!collapse));
        body.hidden = collapse;
      } }, icon('chevron', 'chevron'));
    return h('div', { class: `group group--${key}` },
      h('div', { class: 'group__head' }, disclosure, lead, h('b', {}, title), h('span', { class: 'group__count' }, count), h('span', { class: 'row__size' }, size)),
      body);
  }

  function renderLeftAlone() {
    const list = state.sample.leftAlone;
    const body = h('ul', { class: 'group__body', id: `group-${state.sample.id}-left-alone` });
    for (const entry of list) {
      body.append(h('li', { class: 'row row--locked' },
        icon('lock', 'row__lock'),
        h('span', { class: 'row__label' }, icon('folder'), pathText(entry.path)),
        h('span', { class: 'row__badges' }, h('span', { class: 'pill pill--locked' }, entry.badge)),
        h('span', { class: 'row__size' }, '—'),
        h('span', { class: 'row__why' }, entry.why)));
    }
    body.append(h('li', { class: 'tree__note' }, 'VortexClean leaves these out of its list. They are shown here to illustrate the rule that excluded each one.'));
    refs.tree.append(group('left-alone', 'Left alone on purpose', h('span', { class: 'group__spacer' }), body, plural(list.length, 'item', 'items'), '—'));
  }

  function render() {
    const selection = state.cleaned ? [] : chosen();
    const total = sum(selection);
    refs.total.textContent = formatBytes(total);
    refs.totalNote.textContent = `${selection.length} of ${plural(items().length, 'item', 'items')} selected`;
    refs.status.textContent = `${selection.length} of ${items().length} selected · ${formatBytes(total)}`;
    refs.dry.checked = state.dryRun;
    refs.dry.disabled = state.cleaned;
    refs.run.disabled = selection.length === 0;
    refs.run.classList.toggle('button--danger', !state.dryRun);

    for (const [category, list] of groups()) {
      const box = refs.groupBoxes.get(category);
      const on = list.filter((item) => state.checked.has(item.path)).length;
      box.checked = on === list.length;
      box.indeterminate = on > 0 && on < list.length;
      box.disabled = state.cleaned;
    }
    const removed = new Set(state.cleaned ? state.report.items.map((item) => item.path) : []);
    for (const item of items()) {
      const check = refs.itemBoxes.get(item.path);
      check.checked = state.checked.has(item.path);
      check.disabled = state.cleaned;
      check.closest('.row').classList.toggle('row--removed', removed.has(item.path));
    }
    renderStorage(selection, total);
    renderResult();
  }

  function renderStorage(selection, total) {
    const present = new Set(items().map((item) => CATEGORY[item.category]?.[1] ?? 'system'));
    const buckets = BUCKETS.filter(([key]) => present.has(key)).map(([key, title]) => {
      const list = selection.filter((item) => (CATEGORY[item.category]?.[1] ?? 'system') === key);
      return { key, title, bytes: sum(list), count: list.length };
    });
    const detail = (bucket) => {
      const share = total > 0 ? (100 * bucket.bytes) / total : 0;
      const percent = share > 0 && share < 0.1 ? '<0.1%' : `${share.toFixed(1)}%`;
      return `${bucket.title} — ${formatBytes(bucket.bytes)} · ${percent} · ${plural(bucket.count, 'item', 'items')}`;
    };
    const current = buckets.find((bucket) => bucket.key === state.highlighted && bucket.bytes > 0);
    refs.summary.textContent = current ? detail(current)
      : state.cleaned ? 'Everything that was selected is in the Trash'
      : selection.length === 0 ? 'Nothing selected'
      : `${formatBytes(total)} in ${plural(selection.length, 'item', 'items')}`;

    // Segments are kept between renders so that their widths animate.
    for (const [key] of BUCKETS) {
      const bucket = buckets.find((entry) => entry.key === key);
      let segment = refs.bar.querySelector(`[data-key="${key}"]`);
      if (!segment) {
        segment = h('span', { class: `segment segment--${key}`, 'data-key': key,
          onpointerenter: () => highlight(key), onpointerleave: () => highlight(null), onclick: () => highlight(key) });
        refs.bar.append(segment);
      }
      const bytes = bucket?.bytes ?? 0;
      segment.style.flexGrow = total > 0 ? String(bytes / total) : '0';
      segment.classList.toggle('segment--empty', bytes === 0);
      segment.classList.toggle('segment--dim', Boolean(current) && current.key !== key);
    }
    refs.bar.setAttribute('aria-label', `Storage breakdown. ${buckets.filter((bucket) => bucket.bytes > 0).map(detail).join('. ')}`);

    // Legend entries are created once per sample and then only updated, so that the
    // one under the pointer or with the keyboard focus is never replaced.
    if (refs.legend.dataset.sample !== state.sample.id) {
      refs.legend.dataset.sample = state.sample.id;
      refs.legend.replaceChildren(...buckets.map(({ key, title }) => h('button', {
        class: `legend legend--${key}`, type: 'button', 'data-legend': key,
        onpointerenter: () => highlight(key), onpointerleave: () => highlight(null),
        onfocus: () => highlight(key), onblur: () => highlight(null),
      }, h('i', { 'aria-hidden': 'true' }), h('span', {}, title), h('b', {}))));
    }
    for (const bucket of buckets) {
      const entry = refs.legend.querySelector(`[data-legend="${bucket.key}"]`);
      entry.querySelector('b').textContent = formatBytes(bucket.bytes);
      entry.classList.toggle('legend--dim', Boolean(current) && current.key !== bucket.key);
    }

    refs.tip.textContent = current ? detail(current) : '';
    refs.tip.classList.toggle('is-visible', Boolean(current));
    if (current) placeTip(refs.bar.querySelector(`[data-key="${current.key}"]`));
  }

  function placeTip(segment) {
    const track = refs.bar.getBoundingClientRect();
    const slice = segment.getBoundingClientRect();
    const width = refs.tip.offsetWidth;
    const centre = slice.left - track.left + slice.width / 2 - width / 2;
    refs.tip.style.left = `${Math.max(0, Math.min(centre, track.width - width))}px`;
  }

  function highlight(key) {
    if (state.highlighted === key) return;
    state.highlighted = key;
    const selection = state.cleaned ? [] : chosen();
    renderStorage(selection, sum(selection));
  }

  // --- Simulated clean ---

  function simulate() {
    const selection = chosen();
    const app = state.sample.data.application;
    const started = new Date();
    const anyRoot = selection.some((item) => item.requires_root);
    const bytes = sum(selection);
    const entries = selection.map((item) => {
      const elevated = !state.dryRun && item.requires_root;
      const entry = {
        path: item.display_path, category: item.category, confidence: item.confidence,
        reason: REASON_KEY[item.reason] ?? item.reason.toLowerCase().replaceAll(' ', '_'),
        status: state.dryRun ? 'dry_run' : 'trashed',
        size_bytes: item.allocated_bytes, logical_bytes: item.logical_bytes,
        requires_root: item.requires_root, elevated, error: null,
      };
      if (!state.dryRun) entry.trash_path = `~/.Trash/${item.display_path.split('/').pop()}`;
      if (item.requires_root) entry.note = state.dryRun ? 'would require administrator authorization' : 'moved to the Trash with its original ownership';
      return entry;
    });
    state.report = {
      dryRun: state.dryRun, items: selection, bytes, started,
      json: {
        schema: 'vortexclean.audit/1',
        generator: { name: 'VortexClean', version: '1.0.0' },
        started_at: started.toISOString(),
        finished_at: new Date(started.getTime() + selection.length * 2).toISOString(),
        duration_ms: selection.length * 2,
        host: { os_version: '27.2.1', architecture: 'arm64', user_id: 501 },
        operation: { mode: 'trash', dry_run: state.dryRun, cancelled: false,
          elevation_attempted: !state.dryRun && anyRoot, elevation_authorized: !state.dryRun && anyRoot, privacy_denied: false },
        target: { name: app.name, bundle_id: app.bundle_id, bundle_path: app.path, version: app.version,
          team_id: app.team_id, signing: app.signing, apple_app: app.apple_app, app_store: app.app_store, sip_protected: app.sip_protected },
        summary: { attempted: entries.length, trashed: state.dryRun ? 0 : entries.length, deleted: 0, skipped_sip: 0, skipped_permission: 0,
          dry_run: state.dryRun ? entries.length : 0, error: 0, skipped_missing: 0,
          bytes_reclaimed: state.dryRun ? 0 : bytes, bytes_reclaimable: state.dryRun ? bytes : 0 },
        items: entries,
      },
    };
    state.cleaned = !state.dryRun;
    state.highlighted = null;
    render();
    announce(refs.resultText.textContent);
  }

  function putBack() {
    state.cleaned = false;
    state.report = null;
    render();
    refs.run.focus({ preventScroll: true });
    announce('Put back');
  }

  function renderResult() {
    const report = state.report;
    refs.result.hidden = !report;
    if (!report) return;
    const count = plural(report.items.length, 'item', 'items');
    refs.resultText.textContent = report.dryRun
      ? `${count} would be removed, reclaiming ${formatBytes(report.bytes)}. Nothing was changed.`
      : `${count} moved to the Trash. ${formatBytes(report.bytes)} reclaimed.`;
    const same = chosen().length === report.items.length && chosen().every((item) => report.items.includes(item));
    if (report.dryRun && !same) refs.resultText.textContent += ' The selection has changed since; run it again to update the log.';
    refs.putBack.hidden = report.dryRun;
    const stamp = report.started.toISOString().replace(/[-:]/g, '').replace('T', '-').replace('.', '-').replace('Z', '');
    refs.jsonName.textContent = `~/Library/Logs/VortexClean/audit-${stamp}.json`;
    const text = JSON.stringify(report.json, null, 2);
    if (refs.json.dataset.stamp !== stamp) {
      refs.json.dataset.stamp = stamp;
      paintJson(refs.json, text);
    }
  }

  build();
  select(SAMPLES[0]);
}
