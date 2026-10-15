// Эмулкон — веб-UI поверх Control API (/control/v1). Без сборки и зависимостей.
// Все данные от компонент выводятся через textContent — никакого innerHTML с чужими строками.
'use strict';

const TOKEN_KEY = 'emulkon.controlToken';
const MAX_ROWS = 1000;
const TAIL = 200;

const state = {
  token: loadToken(),
  view: 'devices',
  paused: false,
  pending: [],
  lastSeq: 0,
  streamAbort: null,
  streamGen: 0,
  refreshTimer: null,
  workstations: new Set(),
  devices: new Set(),
  selected: null, // устройство в карточке
  typeFilter: '', // класс в списке устройств; пусто — все
  types: [],      // классы: схема и настройки по умолчанию (GET /control/v1/types)
  wsById: {},     // рабочие места: подписи
  metaFor: null,  // для какого устройства заполнена форма подписи
};

const $ = (sel) => document.querySelector(sel);

// ---------- токен ----------

function loadToken() {
  try { return localStorage.getItem(TOKEN_KEY) || ''; } catch { return ''; }
}
function saveToken(t) {
  try { t ? localStorage.setItem(TOKEN_KEY, t) : localStorage.removeItem(TOKEN_KEY); } catch { /* приватный режим */ }
}
function authHeaders() {
  return state.token ? { Authorization: 'Bearer ' + state.token } : {};
}

class AuthError extends Error {}

async function api(path, opts = {}) {
  const res = await fetch(path, { ...opts, headers: { ...authHeaders(), ...(opts.headers || {}) } });
  if (res.status === 401) { showTokenPanel(); throw new AuthError('нужен токен'); }
  if (!res.ok) {
    let msg = res.status + ' ' + res.statusText;
    try { msg = (await res.json()).error.message || msg; } catch { /* не JSON */ }
    throw new Error(msg);
  }
  return res;
}

function showTokenPanel() {
  $('#token-panel').hidden = false;
  setLive(false);
  $('#token-input').focus();
}

$('#token-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  state.token = $('#token-input').value.trim();
  saveToken(state.token);
  $('#token-input').value = '';
  $('#token-panel').hidden = true;
  start();
});

// ---------- вкладки ----------

document.querySelectorAll('.tab[data-view]').forEach((btn) => {
  btn.addEventListener('click', () => switchView(btn.dataset.view));
});

function switchView(view) {
  state.view = view;
  document.querySelectorAll('.tab[data-view]').forEach((b) => b.classList.toggle('active', b.dataset.view === view));
  document.querySelectorAll('.view').forEach((v) => { v.hidden = v.id !== 'view-' + view; });
  document.body.classList.toggle('fill', view === 'journal'); // одна прокрутка — у таблицы журнала
  try { localStorage.setItem('emulkon.view', view); } catch { /* ignore */ }
  if (location.hash !== '#' + view) history.replaceState(null, '', '#' + view); // ссылки со страницы загрузок
  if (view === 'classes') state.classesStale = true;
  refreshTables();
}

// ---------- утилиты ----------

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'title') node.title = v;
    else node.setAttribute(k, v);
  }
  for (const c of children) {
    if (c == null) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}

function fmtTime(ts) {
  if (!ts) return '';
  const d = new Date(ts);
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}

function fmtAgo(ts) {
  if (!ts) return '—';
  const s = Math.max(0, Math.round((Date.now() - new Date(ts).getTime()) / 1000));
  if (s < 5) return 'только что';
  if (s < 60) return s + ' с назад';
  if (s < 3600) return Math.round(s / 60) + ' мин назад';
  if (s < 86400) return Math.round(s / 3600) + ' ч назад';
  return new Date(ts).toLocaleString('ru-RU');
}

// Ревизия требований: 4008 → «4.8», 3005 → «3.5».
function fmtRevision(r) {
  if (!r) return '—';
  return `${Math.floor(r / 1000)}.${r % 1000} (${r})`;
}

function short(id, n = 8) {
  return id && id.length > n + 2 ? id.slice(0, n) + '…' : (id || '');
}

function setLive(on) {
  const live = $('#live');
  live.classList.toggle('on', on);
  live.classList.toggle('off', !on);
  live.title = on ? 'Поток журнала подключён' : 'Поток журнала не подключён';
}

// fillSelect — выпадающий список фильтра: «все» и известные значения; выбранное сохраняется.
function fillSelect(id, values, allLabel) {
  const sel = $(id);
  const cur = sel.value;
  const opts = [...values].filter(Boolean).sort();
  if (cur && !opts.includes(cur)) opts.push(cur);
  sel.replaceChildren(el('option', { value: '' }, allLabel), ...opts.map((v) => el('option', { value: v }, v)));
  sel.value = cur;
}

// ---------- уведомления (колокольчик) ----------

function setNotices(list) {
  const count = $('#bell-count');
  count.textContent = String(list.length);
  count.hidden = list.length === 0;
  $('#bell').classList.toggle('has', list.length > 0);
  $('#notices-list').replaceChildren(...(list.length ? list.map((n) => el('div', { class: 'notice' }, n))
    : [el('div', { class: 'notice muted' }, 'Уведомлений нет.')]));
}

$('#bell').addEventListener('click', (ev) => {
  ev.stopPropagation();
  $('#notices').hidden = !$('#notices').hidden;
});
document.addEventListener('click', (ev) => {
  if (!ev.target.closest('.bell-wrap')) $('#notices').hidden = true;
});

// ---------- info ----------

async function loadInfo() {
  const info = await (await api('/control/v1/info')).json();
  $('#version').textContent = 'v' + info.version;
  const warns = [];
  if (info.device_open_mode) warns.push('Токен устройств не задан (EMULKON_DEVICE_TOKEN): подключиться может любая компонента. Подходит только для локальной работы.');
  if (info.control_open_mode) warns.push('Токен Control API не задан (EMULKON_CONTROL_TOKEN): управление эмуляцией открыто.');
  setNotices(warns);
}

// ---------- устройства и сессии ----------

function scheduleRefresh() {
  if (state.refreshTimer) return;
  state.refreshTimer = setTimeout(() => { state.refreshTimer = null; refreshTables(); }, 400);
}

async function refreshTables() {
  try {
    if (state.view === 'devices') await loadDevices();
    else if (state.view === 'sessions') await loadSessions();
    else if (state.view === 'scenarios' && $('#scn-form').hidden) await loadScenarios(); // счётчики; в редакторе — не мешаем
    else if (state.view === 'classes' && state.classesStale) {
      // Классы — форма с черновиком: перерисовываем при переходе на вкладку, а не по таймеру.
      state.classesStale = false;
      await loadClasses(true);
    }
  } catch (e) {
    if (!(e instanceof AuthError)) console.warn(e);
  }
}

const SHIFT_STATES = { 1: 'закрыта', 2: 'открыта', 3: 'истекла (>24 ч)' };

// Неисправности ККТ, которые стоит видеть сразу.
function kktFaults(st) {
  const f = st.faults || {};
  const out = [];
  if (!st.fiscal) out.push('не зарегистрирована');
  if (st.fn_closed) out.push('ФН закрыт');
  if (f.paper === false) out.push('нет бумаги');
  if (f.cover_open) out.push('крышка открыта');
  if (f.fn_error) out.push('замена ФН');
  if (f.fn_overflow) out.push('память ФН');
  if (f.fn_fail) out.push('ресурс ФН');
  if (f.print_error_after_fiscal) out.push('сбой печати');
  if (st.ofd && !st.ofd.online) out.push('нет связи с ОФД');
  if (st.shift && st.shift.state === 3) out.push('смена > 24 ч');
  return out;
}

function stateCell(st) {
  const td = el('td', { class: 'state' });
  if (!st) return td;
  if (st.model === 'kkt') {
    const sh = st.shift || {};
    td.append(
      el('span', { class: 'chip' }, `смена ${sh.number || '—'}: ${SHIFT_STATES[sh.state] || '—'}`),
      el('span', { class: 'chip' }, `ФД ${st.doc_number}`),
      el('span', { class: 'chip' }, `нал. ${Number(st.cash_balance).toFixed(2)}`),
    );
    if (st.ofd && st.ofd.backlog) td.append(el('span', { class: 'chip bad' }, `в очереди ОФД: ${st.ofd.backlog}`));
    if (st.drawer_open) td.append(el('span', { class: 'chip wait' }, 'ящик открыт'));
    for (const f of kktFaults(st)) td.append(el('span', { class: 'chip bad' }, f));
    return td;
  }
  if (st.model === 'acquiring') {
    if (st.prompt) td.append(el('span', { class: 'chip wait' }, `ждёт карту: ${st.prompt.title} ${money(st.prompt.amount)}`));
    td.append(
      el('span', { class: 'chip' }, `терминал ${st.terminal_id}`),
      el('span', { class: 'chip' }, CARD_MODES[st.card_mode] || st.card_mode),
      el('span', { class: 'chip' }, `пакет ${st.batch}: операций ${(st.operations || []).length}`),
    );
    if (!st.bank_online) td.append(el('span', { class: 'chip bad' }, 'нет связи с банком'));
    return td;
  }
  if (st.model === 'scales') {
    if (st.prompt) td.append(el('span', { class: 'chip wait' }, 'ждёт вес'));
    td.append(el('span', { class: 'chip' }, `нетто ${kg(st.net)}`));
    if (st.tare) td.append(el('span', { class: 'chip' }, `тара ${kg(st.tare)}`));
    td.append(el('span', { class: 'chip' }, SCALE_MODES[st.weight_mode] || st.weight_mode));
    for (const f of scaleFaults(st)) td.append(el('span', { class: 'chip bad' }, f));
    return td;
  }
  if (st.model === 'label_scales') {
    td.append(
      el('span', { class: 'chip' }, `товаров ${st.goods_count}`),
      el('span', { class: 'chip' }, `картинок ${(st.images || []).length}`),
      el('span', { class: 'chip' }, `этикеток ${st.labels}`),
    );
    return td;
  }
  if (st.model === 'scanner' || st.model === 'card_reader') {
    td.append(el('span', { class: 'chip' }, st.model === 'scanner' ? `сканов ${st.scans}` : `карт ${st.swipes}`));
    if (st.event_format === 'base64') td.append(el('span', { class: 'chip' }, 'Base64'));
    return td;
  }
  if (st.model === 'display') {
    td.append(el('span', { class: 'chip' }, `${st.columns}×${st.rows}`), el('span', { class: 'chip' }, `выводов ${st.outputs}`));
    if (st.broken) td.append(el('span', { class: 'chip bad' }, 'не отвечает'));
    return td;
  }
  if (st.model === 'tsd') {
    td.append(el('span', { class: 'chip' }, `товаров ${st.goods_count}`), el('span', { class: 'chip' }, `в документе ${(st.document || []).length}`));
    return td;
  }
  if (st.model === 'rfid') {
    td.append(el('span', { class: 'chip' + (st.session ? ' wait' : '') }, st.session ? 'сессия открыта' : 'сессии нет'),
      el('span', { class: 'chip' }, `меток в поле ${(st.field || []).length}`));
    return td;
  }
  if (st.model === 'recognizer') {
    td.append(el('span', { class: 'chip' }, `кандидатов ${(st.candidates || []).length}`));
    if (st.awaiting_confirm) td.append(el('span', { class: 'chip wait' }, 'ждёт подтверждения'));
    return td;
  }
  if (st.model === 'receipt_printer') {
    td.append(
      el('span', { class: 'chip' }, `документов ${st.documents}`),
      el('span', { class: 'chip' }, `ширина ${st.line_length}`),
    );
    if (st.drawer_open) td.append(el('span', { class: 'chip wait' }, 'ящик открыт'));
    else if (st.drawer_opens) td.append(el('span', { class: 'chip' }, `ящик ×${st.drawer_opens}`));
    for (const f of printerFaults(st)) td.append(el('span', { class: 'chip bad' }, f));
    return td;
  }
  if (st.model === 'label_printer') {
    if (st.job && st.job.open) td.append(el('span', { class: 'chip wait' }, 'печатает задание'));
    td.append(
      el('span', { class: 'chip' }, `заданий ${st.jobs}`),
      el('span', { class: 'chip' }, `этикеток ${st.labels_printed}`),
    );
    for (const f of printerFaults(st)) td.append(el('span', { class: 'chip bad' }, f));
    return td;
  }
  for (const [k, v] of Object.entries(st)) {
    const val = typeof v === 'object' ? JSON.stringify(v) : String(v);
    td.append(el('span', { class: 'kv' }, el('b', {}, k + ': '), el('span', { class: 'mono' }, val)));
  }
  return td;
}

// ---------- учёт устройств: классы, экземпляры, рабочие места (docs/protocol.md §2.3) ----------

const STALE_DAYS = 7; // устройство «давно не подключалось» — только пометка, ничего не удаляется

async function getJSON(path) { return (await api(path)).json(); }

function wsLabel(id) {
  const w = state.wsById[id];
  return w && w.alias ? `${w.alias} (${id})` : id;
}

// Пометки устройства: не настроено, копия, давно не подключалось.
function deviceMarks(d) {
  const out = [];
  if (d.keyed_by === 'workstation') {
    out.push(el('span', { class: 'chip', title: 'У экземпляра в 1С нет идентификатора: устройство общее для всех экземпляров этого типа на рабочем месте. Откройте в 1С «Настройка» экземпляра и нажмите «ОК».' }, 'не настроено'));
  }
  if (d.copy) out.push(el('span', { class: 'chip bad', title: 'Тот же идентификатор уже подключён другим экземпляром — экземпляр скопировали вместе с параметрами. Очистите «Идентификатор экземпляра» в копии.' }, 'копия'));
  const days = d.last_seen ? (Date.now() - new Date(d.last_seen).getTime()) / 86400000 : Infinity;
  if (!d.online && days >= STALE_DAYS) out.push(el('span', { class: 'chip', title: 'Ничего не удаляется само: удалить можно в карточке.' },
    d.last_seen ? `не подключалось ${Math.floor(days)} дн.` : 'не подключалось'));
  return out;
}

function onlineCell(d) {
  return el('td', { title: d.last_seen || '' },
    el('span', { class: 'dot' + (d.online ? ' on' : '') }), d.online ? 'на связи' : fmtAgo(d.last_seen));
}

function deviceRow(d) {
  const title = d.alias || d.instance_name || d.device_id;
  return el('tr', { class: d.device_id === state.highlighted ? 'selected' : null, 'data-id': d.device_id, title: 'Двойной клик — открыть карточку' },
    el('td', {}, el('b', {}, title),
      el('div', { class: 'muted' }, el('span', { class: 'mono' }, d.device_id), ' · ', d.name),
      d.last_error ? el('div', { class: 'res-fail', title: 'Последняя ошибка (GetLastError)' }, `✗ ${d.last_error.code} ${d.last_error.text}`) : null),
    el('td', {}, d.infobase || el('span', { class: 'muted' }, 'база не указана'),
      d.instance_name ? el('div', { class: 'muted' }, d.instance_name) : null,
      el('div', {}, ...deviceMarks(d))),
    el('td', {}, wsLabel(d.workstation_id)),
    onlineCell(d),
    el('td', { class: 'num' }, d.commands),
    stateCell(d.state),
  );
}

function renderClassList(types, devices) {
  const count = {};
  devices.forEach((d) => { count[d.type] = (count[d.type] || 0) + 1; });
  const item = (key, title, n) => {
    const b = el('button', { type: 'button', class: 'class-item' + (state.typeFilter === key ? ' active' : '') + (n ? '' : ' empty') },
      el('span', {}, title), el('span', { class: 'count' }, String(n)));
    b.addEventListener('click', () => { state.typeFilter = key; refreshTables(); });
    return b;
  };
  $('#class-list').replaceChildren(item('', 'Все устройства', devices.length),
    ...types.map((t) => item(t.key, t.title, count[t.key] || 0)));
}

async function loadDevices() {
  const [{ devices }, { types }, { workstations }] = await Promise.all([
    getJSON('/control/v1/devices'), getJSON('/control/v1/types'), getJSON('/control/v1/workstations')]);
  state.types = types;
  state.wsById = Object.fromEntries(workstations.map((w) => [w.workstation_id, w]));
  renderClassList(types, devices);
  fillSelect('#dev-ws', new Set(devices.map((d) => d.workstation_id)), 'все рабочие места');
  fillSelect('#dev-base', new Set(devices.map((d) => d.infobase)), 'все базы');

  const ws = $('#dev-ws').value, base = $('#dev-base').value, q = $('#dev-q').value.trim().toLowerCase();
  const shown = devices.filter((d) => (!state.typeFilter || d.type === state.typeFilter)
    && (!ws || d.workstation_id === ws) && (!base || d.infobase === base)
    && (!q || [d.device_id, d.alias, d.note, d.instance_name, d.infobase, d.instance_id, d.workstation_id, d.name]
      .some((v) => v && String(v).toLowerCase().includes(q))));

  // Без фильтра класса — группы по классам в порядке таблицы требований.
  const rows = [];
  for (const t of types) {
    const group = shown.filter((d) => d.type === t.key);
    if (!group.length) continue;
    if (!state.typeFilter) rows.push(el('tr', { class: 'group-row' }, el('td', { colspan: '6' }, t.title, el('span', { class: 'muted' }, ` · ${group.length}`))));
    rows.push(...group.map(deviceRow));
  }
  const body = $('#devices-body');
  body.replaceChildren(...rows);
  for (const tr of body.querySelectorAll('tr[data-id]')) {
    tr.addEventListener('click', () => highlightDevice(tr.dataset.id));
    tr.addEventListener('dblclick', () => selectDevice(tr.dataset.id));
  }
  $('#devices-empty').hidden = shown.length > 0;
  const online = devices.filter((d) => d.online).length;
  $('#devices-meta').textContent = devices.length ? `${devices.length} шт., на связи ${online}` + (shown.length !== devices.length ? `, показано ${shown.length}` : '') : '';

  const sel = devices.find((d) => d.device_id === state.selected);
  if (sel) await loadDetail(sel);
  else if (state.selected) closeDetail();
  devices.forEach((d) => { state.devices.add(d.device_id); state.workstations.add(d.workstation_id); });
  fillSelect('#f-device', state.devices, 'все устройства');
  fillSelect('#f-workstation', state.workstations, 'все рабочие места');
}

for (const id of ['#dev-ws', '#dev-base']) $(id).addEventListener('change', () => refreshTables());
$('#dev-q').addEventListener('input', () => scheduleRefresh());

// --- рабочие места ---

async function patchJSON(path, body) {
  return (await api(path, { method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })).json();
}

function wsMetaInput(w, field, placeholder) {
  const input = el('input', { class: 'inline-input', value: w[field] || '', placeholder, maxlength: '500' });
  input.addEventListener('change', async () => {
    try {
      await patchJSON(`/control/v1/workstations/${encodeURIComponent(w.workstation_id)}`, { [field]: input.value.trim() });
      input.classList.add('saved');
      setTimeout(() => input.classList.remove('saved'), 1200);
    } catch (e) { if (!(e instanceof AuthError)) alert('Не сохранилось: ' + e.message); }
  });
  return input;
}

// Токен рабочего места (docs/protocol.md §1.1): виден один раз — сразу после выдачи.
async function issueToken(id) {
  try {
    const t = await (await api(`/control/v1/workstations/${encodeURIComponent(id)}/token`, { method: 'POST' })).json();
    $('#ws-token-ws').textContent = t.workstation_id;
    $('#ws-token-value').value = t.token;
    $('#ws-token').hidden = false;
    $('#ws-token-value').select();
    loadSessions();
  } catch (e) { if (!(e instanceof AuthError)) alert('Токен не выдан: ' + e.message); }
}

function wsTokenCell(w) {
  const td = el('td', { class: 'token-cell' });
  if (w.token) {
    td.append(el('span', { class: 'chip', title: 'выдан ' + new Date(w.token.issued_at).toLocaleString('ru-RU') + '. Без токена это место не подключится.' }, '🔑 …' + w.token.hint));
  } else {
    td.append(el('span', { class: 'muted', title: 'Место пускается общим токеном сервера или в открытом режиме' }, 'нет'));
  }
  const issue = el('button', { type: 'button', class: 'btn small' }, w.token ? 'Новый' : 'Выдать');
  issue.title = w.token ? 'Выдать новый токен — прежний перестанет действовать' : 'Выдать токен: подключаться смогут только с ним';
  issue.addEventListener('click', () => {
    if (w.token && !confirm(`Выдать новый токен рабочему месту ${w.workstation_id}? Прежний перестанет действовать — его придётся заменить в 1С.`)) return;
    issueToken(w.workstation_id);
  });
  td.append(issue);
  if (w.token) {
    const revoke = el('button', { type: 'button', class: 'btn small' }, 'Отозвать');
    revoke.title = 'Отозвать токен: место снова пускается без него';
    revoke.addEventListener('click', async () => {
      if (!confirm(`Отозвать токен рабочего места ${w.workstation_id}?`)) return;
      try {
        await api(`/control/v1/workstations/${encodeURIComponent(w.workstation_id)}/token`, { method: 'DELETE' });
        loadSessions();
      } catch (e) { if (!(e instanceof AuthError)) alert('Не отозван: ' + e.message); }
    });
    td.append(revoke);
  }
  return td;
}

$('#ws-add').addEventListener('submit', (e) => {
  e.preventDefault();
  const id = $('#ws-add-id').value.trim();
  if (!id) return;
  $('#ws-add-id').value = '';
  issueToken(id);
});
$('#ws-token-copy').addEventListener('click', async () => {
  const v = $('#ws-token-value');
  try { await navigator.clipboard.writeText(v.value); } catch { v.select(); document.execCommand('copy'); }
  $('#ws-token-copy').textContent = 'Скопировано';
  setTimeout(() => { $('#ws-token-copy').textContent = 'Скопировать'; }, 1500);
});
$('#ws-token-close').addEventListener('click', () => {
  $('#ws-token-value').value = '';
  $('#ws-token').hidden = true;
});

function openDevice(id) {
  state.typeFilter = '';
  switchView('devices');
  selectDevice(id);
}

async function loadSessions() {
  const [{ workstations }, { sessions }] = await Promise.all([getJSON('/control/v1/workstations'), getJSON('/control/v1/sessions')]);
  // Поле с фокусом не перерисовываем — иначе собьётся ввод подписи.
  const editing = document.activeElement && document.activeElement.closest('#ws-body');
  if (!editing) {
    $('#ws-body').replaceChildren(...workstations.map((w) => el('tr', {},
      el('td', {}, el('b', { class: 'mono' }, w.workstation_id)),
      el('td', { class: 'meta-cell' }, wsMetaInput(w, 'alias', 'подпись: Касса 1, магазин на Ленина'), wsMetaInput(w, 'note', 'заметка')),
      wsTokenCell(w),
      el('td', {}, ...(w.infobases.length ? w.infobases.map((b) => el('div', {}, b)) : [el('span', { class: 'muted' }, '—')])),
      el('td', {}, ...w.devices.map((id) => {
        const b = el('button', { type: 'button', class: 'chip link', title: 'Открыть карточку' }, id);
        b.addEventListener('click', () => openDevice(id));
        return b;
      })),
      el('td', { class: 'num' }, w.sessions),
      el('td', { title: w.first_seen }, w.first_seen && !w.first_seen.startsWith('0001') ? new Date(w.first_seen).toLocaleDateString('ru-RU') : '—'),
      el('td', { title: w.last_seen }, w.last_seen && !w.last_seen.startsWith('0001') ? fmtAgo(w.last_seen) : '—'),
    )));
  }
  $('#ws-empty').hidden = workstations.length > 0;
  $('#ws-meta').textContent = workstations.length ? `${workstations.length} шт. · подпись и заметка сохраняются сразу` : '';

  sessions.reverse();
  $('#sessions-body').replaceChildren(...sessions.map((s) => el('tr', {},
    el('td', {}, el('b', {}, s.workstation_id)),
    el('td', { class: 'mono' }, s.device_id),
    el('td', { class: 'mono', title: s.session_id }, short(s.session_id, 12)),
    el('td', { class: 'mono' }, s.equipment_type, s.driver_version ? el('span', { class: 'muted' }, ' ' + s.driver_version) : null),
    el('td', {}, fmtRevision(s.interface_revision)),
    el('td', { title: s.created_at }, fmtAgo(s.created_at)),
    el('td', { title: s.last_seen }, fmtAgo(s.last_seen)),
  )));
  $('#sessions-empty').hidden = sessions.length > 0;
  $('#sessions-meta').textContent = sessions.length ? `${sessions.length} открыто · закрываются при отключении в 1С или после 24 ч простоя` : '';
}

// --- поля настроек по схеме класса ---

function valueAt(st, f) {
  if (f.path === '-') return undefined;
  let cur = st;
  for (const p of (f.path || f.key).split('.')) {
    if (cur == null) return undefined;
    cur = cur[p];
  }
  return cur;
}

// fieldControl — поле формы по схеме. tri — для классов: «как у модели» (значение не задано).
function fieldControl(f, value, onChange, tri = false) {
  const unset = value === undefined || value === null;
  if (f.kind === 'bool' && !tri) {
    const b = el('button', { type: 'button', class: 'toggle' + (value ? ' on' : '') }, value ? 'да' : 'нет');
    b.addEventListener('click', () => onChange(!value));
    return b;
  }
  if (f.kind === 'bool' || f.kind === 'enum') {
    const opts = f.kind === 'bool' ? [{ value: 'true', title: 'да' }, { value: 'false', title: 'нет' }] : f.options;
    const sel = el('select', {}, ...(tri ? [el('option', { value: '' }, '— как у модели —')] : []),
      ...opts.map((o) => el('option', { value: o.value }, o.title)));
    sel.value = unset ? '' : String(value);
    sel.addEventListener('change', () => {
      if (sel.value === '') return onChange(undefined);
      onChange(f.kind === 'bool' ? sel.value === 'true' : sel.value);
    });
    return sel;
  }
  if (f.kind === 'json') {
    const ta = el('textarea', { rows: '3', class: 'mono', placeholder: tri ? 'как у модели' : f.hint || '' });
    ta.value = unset ? '' : JSON.stringify(value);
    const apply = () => {
      if (ta.value.trim() === '') return onChange(undefined);
      try { onChange(JSON.parse(ta.value)); } catch { alert(`${f.title}: это не JSON`); }
    };
    if (tri) { ta.addEventListener('change', apply); return ta; }
    const btn = el('button', { type: 'button', class: 'btn' }, 'Применить');
    btn.addEventListener('click', apply);
    return el('div', { class: 'json-field' }, ta, btn);
  }
  const numeric = f.kind === 'int' || f.kind === 'number';
  const input = el('input', {
    type: numeric ? 'number' : 'text', step: f.kind === 'int' ? '1' : 'any',
    min: f.min ?? null, max: f.max ?? null, placeholder: tri ? 'как у модели' : '',
  });
  input.value = unset ? '' : String(value);
  input.addEventListener('change', () => {
    const v = input.value.trim();
    if (v === '') return onChange(tri || !numeric ? (tri ? undefined : '') : undefined);
    if (!numeric) return onChange(v);
    const n = Number(v);
    if (!Number.isFinite(n)) { alert(`${f.title}: нужно число`); return; }
    onChange(n);
  });
  return input;
}

function fieldRow(f, control, extra) {
  return el('div', { class: 'field' },
    el('label', { title: f.key }, f.title, f.hint ? el('span', { class: 'hint' }, f.hint) : null),
    el('div', { class: 'field-control' }, control, extra || null));
}

// --- карточка: экземпляр и настройки ---

function renderMeta(info) {
  const ws = state.wsById[info.workstation_id];
  $('#detail-meta').replaceChildren(
    ...summaryRow('Класс', `${(state.types.find((t) => t.key === info.type) || {}).title || info.type} · ${info.name}`),
    ...summaryRow('Ключ устройства', info.keyed_by === 'instance' ? `экземпляр ${info.instance_id}` : 'рабочее место и тип — экземпляр не настроен'),
    ...summaryRow('Экземпляр в 1С', info.instance_name || '—'),
    ...summaryRow('База 1С', info.infobase || '—'),
    ...summaryRow('Рабочее место', ws && ws.alias ? `${ws.alias} (${info.workstation_id})` : info.workstation_id),
    ...summaryRow('Связь', info.online ? `на связи, сессий ${info.active_sessions}` : `последняя активность ${fmtAgo(info.last_seen)}`),
    ...summaryRow('Создано', new Date(info.created_at).toLocaleString('ru-RU')),
  );
  const marks = deviceMarks(info);
  if (marks.length) $('#detail-meta').append(...summaryRow('Пометки', el('span', {}, ...marks)));
  // Форму подписи заполняем при открытии карточки, а не на каждом обновлении — ввод не сбивается.
  if (state.metaFor !== info.device_id) {
    state.metaFor = info.device_id;
    const form = $('#meta-form');
    form.alias.value = info.alias || '';
    form.note.value = info.note || '';
    $('#meta-saved').textContent = '';
  }
}

function renderSettings(info) {
  const box = $('#detail-settings');
  if (box.contains(document.activeElement)) return; // идёт ввод
  const type = state.types.find((t) => t.key === info.type);
  const fields = (type ? type.schema : []).filter((f) => f.setting);
  if (!fields.length) {
    box.replaceChildren(el('div', { class: 'muted' }, 'У этого класса нет настроек.'));
    $('#settings-reset').hidden = true;
    return;
  }
  $('#settings-reset').hidden = false;
  box.replaceChildren(...fields.map((f) => {
    const saved = Object.prototype.hasOwnProperty.call(info.settings || {}, f.key);
    const value = f.path === '-' ? info.settings[f.key] : valueAt(info.state || {}, f);
    return fieldRow(f, fieldControl(f, value, (v) => { if (v !== undefined) setDeviceState({ [f.key]: v }); }),
      el('span', { class: 'badge' + (saved ? ' saved' : ''), title: saved ? 'сохранено у устройства' : 'значение модели по умолчанию' }, saved ? 'задано' : 'по умолчанию'));
  }));
}

// --- показ и печать документов на кассе (настройка устройства, выполняет трей; protocol.md §2.3) ---

function renderOutput(info) {
  const box = $('#output-box');
  box.hidden = !info.output;
  if (!info.output || $('#detail-output').contains(document.activeElement)) return;
  const out = info.output;
  const type = state.types.find((t) => t.key === info.type) || { output_kinds: [] };
  const ws = state.wsById[info.workstation_id] || {};
  const save = (patch) => saveOutput({ ...out, kinds: [...(out.kinds || [])], ...patch });

  const toggle = (on, label, key) => {
    const b = el('button', { type: 'button', class: 'toggle' + (on ? ' on' : '') }, on ? 'да' : 'нет');
    b.addEventListener('click', () => save({ [key]: !on }));
    return b;
  };
  // Принтер — из тех, что сообщил трей рабочего места; не сообщал — имя вводится руками.
  let printer;
  const printers = ws.printers || [];
  if (printers.length) {
    printer = el('select', {},
      el('option', { value: '' }, 'по умолчанию' + (ws.default_printer ? ` (${ws.default_printer})` : '')),
      ...printers.map((p) => el('option', { value: p }, p)));
    if (out.printer_name && !printers.includes(out.printer_name)) printer.append(el('option', { value: out.printer_name }, out.printer_name + ' — нет на компьютере'));
    printer.value = out.printer_name || '';
    printer.addEventListener('change', () => save({ printer_name: printer.value }));
  } else {
    printer = el('input', { type: 'text', placeholder: 'по умолчанию', value: out.printer_name || '' });
    printer.addEventListener('change', () => save({ printer_name: printer.value.trim() }));
  }
  const kinds = el('div', { class: 'kind-list' }, ...type.output_kinds.map((k) => {
    const cb = el('input', { type: 'checkbox' });
    cb.checked = (out.kinds || []).includes(k.id);
    cb.disabled = !out.printer;
    cb.addEventListener('change', () => {
      const set = new Set(out.kinds || []);
      if (cb.checked) set.add(k.id); else set.delete(k.id);
      save({ kinds: type.output_kinds.map((x) => x.id).filter((id) => set.has(id)) });
    });
    return el('label', { class: 'kind' }, cb, k.title);
  }));
  $('#detail-output').replaceChildren(
    fieldRow({ title: 'Показывать на экране кассы', hint: 'всплывающее окно трея с документом' }, toggle(out.screen, '', 'screen')),
    fieldRow({ title: 'Печатать на принтер', hint: 'принтер Windows рабочего места' }, toggle(out.printer, '', 'printer')),
    fieldRow({ title: 'Принтер', hint: printers.length ? `трей ${info.workstation_id} сообщил ${printers.length} шт.` : 'трей этого рабочего места ещё не сообщил принтеры' }, printer),
    fieldRow({ title: 'Что печатать' }, kinds),
  );
}

async function saveOutput(output) {
  try {
    await patchJSON(`/control/v1/devices/${encodeURIComponent(state.selected)}`, { output });
  } catch (e) { if (!(e instanceof AuthError)) alert('Не сохранилось: ' + e.message); }
  refreshTables();
}

$('#reprint').addEventListener('click', async () => {
  const status = $('#reprint-status');
  try {
    const r = await (await api(`/control/v1/devices/${encodeURIComponent(state.selected)}/reprint`, { method: 'POST' })).json();
    status.textContent = `отправлено на принтер: ${KIND_NAMES[r.kind] || r.kind}`;
  } catch (e) { if (!(e instanceof AuthError)) status.textContent = e.message; }
  setTimeout(() => { status.textContent = ''; }, 4000);
});

$('#meta-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const form = ev.target;
  try {
    await patchJSON(`/control/v1/devices/${encodeURIComponent(state.selected)}`, { alias: form.alias.value.trim(), note: form.note.value.trim() });
    $('#meta-saved').textContent = 'сохранено';
    refreshTables();
  } catch (e) { if (!(e instanceof AuthError)) $('#meta-saved').textContent = 'не сохранилось: ' + e.message; }
});

$('#settings-reset').addEventListener('click', async () => {
  if (!confirm('Вернуть устройству настройки класса? Модель вернётся в исходное состояние: смена, операции и лента сбросятся.')) return;
  try {
    await api(`/control/v1/devices/${encodeURIComponent(state.selected)}/settings/reset`, { method: 'POST' });
    refreshTables();
  } catch (e) { if (!(e instanceof AuthError)) alert('Не получилось: ' + e.message); }
});

$('#device-delete').addEventListener('click', async () => {
  const id = state.selected;
  if (!confirm(`Удалить устройство ${id}? Его сессии закроются. Если экземпляр в 1С подключится снова, появится новое устройство с исходным состоянием.`)) return;
  try {
    await api(`/control/v1/devices/${encodeURIComponent(id)}`, { method: 'DELETE' });
    closeDetail();
    refreshTables();
  } catch (e) { if (!(e instanceof AuthError)) alert('Не удалилось: ' + e.message); }
});

// --- сценарии сбоев (docs/protocol.md §2.4) ---

const SCN_PRESETS = [
  { name: 'ККТ: нет бумаги на следующем чеке', match: { type: 'kkt', method: 'ProcessCheck' }, limit: 1,
    action: { type: 'error', error_code: 1, error_text: 'Нет бумаги (сценарий)' } },
  { name: 'ККТ: чек «завис» — ответ после таймаута драйвера', match: { type: 'kkt', method: 'ProcessCheck' }, limit: 1,
    action: { type: 'late', delay_ms: 35000 } },
  { name: 'ККТ: чек пробит, ответ потерялся', match: { type: 'kkt', method: 'ProcessCheck' }, limit: 1,
    action: { type: 'drop' } },
  { name: 'Терминал: отказ банка на каждой второй оплате', match: { type: 'acquiring', method: 'Pay', every: 2 },
    action: { type: 'error', error_code: 61, error_text: 'Операция отклонена банком-эмитентом (сценарий)' } },
  { name: 'Терминал: нет связи с банком в 30% оплат', match: { type: 'acquiring', method: 'Pay', probability: 0.3 },
    action: { type: 'error', error_code: 66, error_text: 'Нет связи с банком (сценарий)' } },
  { name: 'Весы: перегрузка при следующем взвешивании', match: { type: 'scales', method: 'GetWeight' }, limit: 1,
    action: { type: 'error', error_code: 70, error_text: 'Перегрузка весов (сценарий)' } },
  { name: 'Любое устройство: отвечает 3 секунды', match: {}, action: { type: 'delay', delay_ms: 3000 } },
];

const SCN_ACTIONS = { error: 'ошибка', delay: 'задержка', late: 'ответ после таймаута', drop: 'обрыв после выполнения',
  set_state: 'состояние', emit_event: 'событие' };

function scnWhen(m) {
  const parts = [];
  parts.push(m.device || (m.type ? (state.types.find((t) => t.key === m.type) || {}).title || m.type : 'любое устройство'));
  if (m.workstation) parts.push('место ' + m.workstation);
  parts.push(m.method || 'любой метод');
  if (m.param) parts.push(`параметр ${m.param.index} содержит «${m.param.contains || m.param.equals}»`);
  if (m.nth_call) parts.push(`на ${m.nth_call}-м вызове`);
  if (m.every) parts.push(`каждый ${m.every}-й`);
  if (m.probability) parts.push(`${Math.round(m.probability * 100)}%`);
  return parts.join(' · ');
}

function scnAction(a) {
  let s = SCN_ACTIONS[a.type] || a.type;
  if (a.type === 'error') s += ` ${a.error_code}: ${a.error_text}`;
  if (a.delay_ms) s += ` · ${a.delay_ms} мс`;
  if (a.type === 'set_state') s += ' ' + JSON.stringify(a.state);
  if (a.type === 'emit_event' && a.event) s += ` ${a.event.name}`;
  return s;
}

async function saveRule(rule) {
  const path = rule.id ? `/control/v1/scenarios/${encodeURIComponent(rule.id)}` : '/control/v1/scenarios';
  return (await api(path, { method: rule.id ? 'PUT' : 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(rule) })).json();
}

async function loadScenarios() {
  const [{ rules }, { devices }, { types }] = await Promise.all([
    getJSON('/control/v1/scenarios'), getJSON('/control/v1/devices'), getJSON('/control/v1/types')]);
  state.types = types;
  state.scnDevices = devices;
  $('#scn-body').replaceChildren(...rules.map((r) => {
    const on = el('input', { type: 'checkbox', class: 'check', title: 'включить или выключить' });
    on.checked = r.enabled;
    on.addEventListener('change', async () => { await saveRule({ ...r, enabled: on.checked }); loadScenarios(); });
    const edit = el('button', { type: 'button', class: 'btn' }, 'Изменить');
    edit.addEventListener('click', () => openRule(r));
    const del = el('button', { type: 'button', class: 'btn danger' }, 'Удалить');
    del.addEventListener('click', async () => {
      if (!confirm(`Удалить правило «${r.name}»?`)) return;
      await api(`/control/v1/scenarios/${encodeURIComponent(r.id)}`, { method: 'DELETE' });
      loadScenarios();
    });
    return el('tr', { class: r.enabled ? null : 'off' },
      el('td', {}, on), el('td', {}, el('b', {}, r.name), el('div', { class: 'muted mono' }, r.id)),
      el('td', {}, scnWhen(r.match || {}), r.limit ? el('div', { class: 'muted' }, `не больше ${r.limit} раз`) : null),
      el('td', {}, scnAction(r.action)),
      el('td', { class: 'num', title: 'сработало / подходящих вызовов' }, `${r.fired} / ${r.seen}`),
      el('td', {}, el('div', { class: 'actions' }, edit, del)));
  }));
  $('#scn-empty').hidden = rules.length > 0;
}

function openRule(r) {
  const f = $('#scn-form');
  const m = r.match || {}, a = r.action || {};
  f.dataset.id = r.id || '';
  $('#scn-form-title').textContent = r.id ? 'Изменить правило' : 'Новое правило';
  f.rname.value = r.name || '';
  f.enabled.checked = r.enabled !== false;
  f.device.replaceChildren(el('option', { value: '' }, 'любое'),
    ...(state.scnDevices || []).map((d) => el('option', { value: d.device_id }, `${d.device_id} — ${d.alias || d.instance_name || d.workstation_id}`)));
  f.device.value = m.device || '';
  f.type.replaceChildren(el('option', { value: '' }, 'любой'), ...state.types.map((t) => el('option', { value: t.key }, t.title)));
  f.type.value = m.type || '';
  f.rmethod.value = m.method || '';
  f.param_index.value = m.param ? m.param.index : '';
  f.param_contains.value = m.param ? (m.param.contains || m.param.equals || '') : '';
  f.when.value = m.nth_call ? 'nth' : m.every ? 'every' : 'always';
  f.n.value = m.nth_call || m.every || '';
  f.probability.value = m.probability ? Math.round(m.probability * 100) : '';
  f.limit.value = r.limit || '';
  f.atype.value = a.type || 'error';
  f.error_code.value = a.error_code || '';
  f.error_text.value = a.error_text || '';
  f.delay_ms.value = a.delay_ms || '';
  f.state.value = a.state ? JSON.stringify(a.state) : '';
  f.event_name.value = a.event ? a.event.name : '';
  f.event_data.value = a.event ? a.event.data : '';
  $('#scn-status').textContent = '';
  showActionFields();
  f.hidden = false;
  f.scrollIntoView({ block: 'nearest' });
}

function showActionFields() {
  const type = $('#scn-form').atype.value;
  document.querySelectorAll('#scn-form .act').forEach((row) => { row.hidden = !row.classList.contains('act-' + type); });
}
$('#scn-form').atype.addEventListener('change', showActionFields);

$('#scn-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  const num = (v) => (v === '' ? 0 : Number(v));
  const match = {};
  if (f.device.value) match.device = f.device.value;
  if (f.type.value) match.type = f.type.value;
  if (f.rmethod.value.trim()) match.method = f.rmethod.value.trim();
  if (f.param_contains.value) match.param = { index: num(f.param_index.value), contains: f.param_contains.value };
  if (f.when.value === 'nth') match.nth_call = num(f.n.value);
  if (f.when.value === 'every') match.every = num(f.n.value);
  if (f.probability.value) match.probability = num(f.probability.value) / 100;
  const action = { type: f.atype.value, delay_ms: num(f.delay_ms.value) };
  if (action.type === 'error') { action.error_code = num(f.error_code.value); action.error_text = f.error_text.value; }
  if (action.type === 'set_state') {
    try { action.state = JSON.parse(f.state.value || '{}'); } catch { $('#scn-status').textContent = 'Состояние — не JSON'; return; }
  }
  if (action.type === 'emit_event') action.event = { name: f.event_name.value.trim(), data: f.event_data.value };
  const rule = { id: f.dataset.id || undefined, name: f.rname.value, enabled: f.enabled.checked, match, action, limit: num(f.limit.value) };
  try {
    await saveRule(rule);
    f.hidden = true;
    loadScenarios();
  } catch (e) { if (!(e instanceof AuthError)) $('#scn-status').textContent = 'Не сохранилось: ' + e.message; }
});

$('#scn-cancel').addEventListener('click', () => { $('#scn-form').hidden = true; });
$('#scn-new').addEventListener('click', () => openRule({ enabled: true, action: { type: 'error' } }));
$('#scn-preset').append(...SCN_PRESETS.map((p, i) => el('option', { value: String(i) }, p.name)));
$('#scn-preset').addEventListener('change', (ev) => {
  const p = SCN_PRESETS[Number(ev.target.value)];
  ev.target.value = '';
  if (p) openRule({ ...p, enabled: true });
});

// --- классы: настройки по умолчанию ---

async function loadClasses(force) {
  const box = $('#classes-body');
  if (!force && box.contains(document.activeElement)) return;
  const { types } = await getJSON('/control/v1/types');
  state.types = types;
  box.replaceChildren(...types.map((t) => {
    const fields = t.schema.filter((f) => f.setting);
    const draft = { ...t.defaults };
    const status = el('span', { class: 'muted' });
    const card = el('section', { class: 'class-card' },
      el('div', { class: 'class-head' }, el('h3', {}, t.title),
        el('span', { class: 'muted mono' }, `${t.key} · ${t.equipment_type}`),
        el('span', { class: 'muted' }, `устройств: ${t.devices}`)));
    if (!fields.length) {
      card.append(el('div', { class: 'muted' }, 'Настроек нет — у модели только состояние и действия.'));
      return card;
    }
    card.append(el('div', { class: 'settings-form' }, ...fields.map((f) => fieldRow(f,
      fieldControl(f, draft[f.key], (v) => { if (v === undefined) delete draft[f.key]; else draft[f.key] = v; status.textContent = 'не сохранено'; }, true)))));
    const save = el('button', { type: 'button', class: 'btn primary' }, 'Сохранить');
    save.addEventListener('click', async () => {
      try {
        await api(`/control/v1/types/${encodeURIComponent(t.key)}/defaults`, {
          method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(draft),
        });
        status.textContent = 'сохранено';
      } catch (e) { if (!(e instanceof AuthError)) status.textContent = 'не сохранилось: ' + e.message; }
    });
    card.append(el('div', { class: 'actions' }, save, status));
    return card;
  }));
}

// ---------- карточка устройства ----------

const KIND_NAMES = {
  check: 'Кассовый чек', correction: 'Чек коррекции', copy: 'Копия чека',
  open_shift: 'Открытие смены', close_shift: 'Закрытие смены', settlements: 'Отчёт о расчётах',
  fn_report: 'Отчёт ФН', x_report: 'X-отчёт', text: 'Текстовый документ',
  cash_in: 'Внесение', cash_out: 'Выплата',
  slip: 'Слип терминала', settlement: 'Сверка итогов', label: 'Этикетка',
};

// ---------- эквайринговый терминал ----------

const CARD_MODES = {
  interactive: 'карта — на экране', approve: 'одобрять сразу', decline: 'отказывать сразу', timeout: 'карту не прикладывают',
};

// Переключатели терминала: ключ PUT …/state.
const ACQ_TOGGLES = [
  { key: 'bank_online', label: 'Связь с банком' },
  { key: 'print_slip_on_terminal', label: 'Терминал печатает слип сам' },
  { key: 'short_slip', label: 'Короткий слип' },
  { key: 'cash_withdrawal', label: 'Выдача наличных' },
  { key: 'partial_cancellation', label: 'Частичная отмена' },
  { key: 'list_card_transactions', label: 'Список операций и статус' },
];

function money(v) { return Number(v || 0).toFixed(2) + ' ₽'; }

function acqSummary(st) {
  return [
    ...summaryRow('Банк', st.bank),
    ...summaryRow('Терминал', `${st.terminal_id} · мерчант ${st.merchant_id}`),
    ...summaryRow('Карта', `${CARD_MODES[st.card_mode] || st.card_mode}; по умолчанию ${st.card}; ждать ${st.card_timeout_s} с`),
    ...summaryRow('Пакет дня', `№${st.batch}, операций ${(st.operations || []).length}`),
    ...summaryRow('Связь с банком', st.bank_online ? 'есть' : 'нет'),
  ];
}

function acqActions(st) {
  const nodes = ACQ_TOGGLES.map((t) => {
    const on = !!st[t.key];
    const b = el('button', { class: 'toggle' + (on ? ' on' : '') + (t.key === 'bank_online' && !on ? ' bad on' : ''), type: 'button', title: t.key }, t.label);
    b.addEventListener('click', () => setDeviceState({ [t.key]: !on }));
    return b;
  });
  const mode = el('select', { title: 'card_mode' }, ...Object.entries(CARD_MODES)
    .map(([v, l]) => { const o = el('option', { value: v }, l); o.selected = st.card_mode === v; return o; }));
  mode.addEventListener('change', () => setDeviceState({ card_mode: mode.value }));
  const card = el('select', { title: 'card' }, ...(st.cards || [])
    .map((c) => { const o = el('option', { value: c.id }, c.title); o.selected = st.card === c.id; return o; }));
  card.addEventListener('change', () => setDeviceState({ card: card.value }));
  nodes.push(el('label', {}, 'Карта:', mode), el('label', {}, 'по умолчанию', card));
  return nodes;
}

// respondPrompt — ответ с экрана устройства: answer = { action, card?, value? }.
async function respondPrompt(prompt, answer) {
  const box = $('#detail-error');
  box.hidden = true;
  try {
    await api(`/control/v1/devices/${encodeURIComponent(state.selected)}/prompt`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ prompt_id: prompt.prompt_id, ...answer }),
    });
  } catch (e) {
    if (e instanceof AuthError) return;
    box.textContent = 'Устройство не приняло ответ: ' + e.message;
    box.hidden = false;
  }
  refreshTables();
}

// renderScreen — экран терминала: запрос «приложите карту» с кнопками или итог последней операции.
function renderScreen(st) {
  const box = $('#detail-screen');
  const p = st.prompt;
  if (p) {
    const left = Math.max(0, Math.round((new Date(p.deadline) - Date.now()) / 1000));
    clearTimeout(state.screenTimer);
    state.screenTimer = setTimeout(refreshTables, 1000); // обратный отсчёт и ответ из трея
    if (box.dataset.prompt === p.prompt_id && !box.hidden) {
      // Тот же запрос: обновляем только отсчёт — выбор карты в списке не сбрасываем.
      const timer = box.querySelector('.pos-timer');
      if (timer) timer.textContent = `осталось ${left} с`;
      return;
    }
    box.dataset.prompt = p.prompt_id;
    const card = el('select', { class: 'pos-card', title: 'Какой картой платят' }, ...(p.cards || [])
      .map((c) => { const o = el('option', { value: c.id }, c.title); o.selected = c.id === p.default_card; return o; }));
    const buttons = (p.actions || []).map((a) => {
      const b = el('button', { class: 'btn pos-btn ' + (a.kind || ''), type: 'button' }, a.title);
      b.addEventListener('click', () => respondPrompt(p, { action: a.id, card: a.id === 'card' ? card.value : '' }));
      return b;
    });
    box.replaceChildren(
      el('div', { class: 'pos-display' },
        el('div', { class: 'pos-op' }, p.title),
        el('div', { class: 'pos-amount' }, money(p.amount)),
        p.amount_cash ? el('div', { class: 'pos-text' }, 'наличными: ' + money(p.amount_cash)) : null,
        el('div', { class: 'pos-text' }, p.text),
        el('div', { class: 'pos-timer' }, `осталось ${left} с`)),
      el('div', { class: 'pos-controls' }, el('label', {}, 'Карта:', card), ...buttons),
    );
  } else if (st.last_result) {
    box.dataset.prompt = '';
    const r = st.last_result;
    box.replaceChildren(el('div', { class: 'pos-display ' + (r.approved ? 'ok' : 'bad') },
      el('div', { class: 'pos-op' }, r.title || r.operation),
      r.amount ? el('div', { class: 'pos-amount' }, money(r.amount)) : null,
      el('div', { class: 'pos-verdict' }, r.text),
      el('div', { class: 'pos-text' }, [r.card, r.rrn ? 'RRN ' + r.rrn : '', new Date(r.at).toLocaleTimeString('ru-RU')].filter(Boolean).join(' · '))));
  } else {
    box.dataset.prompt = '';
    box.replaceChildren(el('div', { class: 'pos-display idle' },
      el('div', { class: 'pos-op' }, st.bank), el('div', { class: 'pos-text' }, 'Ожидание операции')));
  }
  box.hidden = false;
}

function renderOps(st) {
  const ops = st.operations || [];
  $('#ops-meta').textContent = `· пакет ${st.batch}, новые сверху`;
  $('#ops-body').replaceChildren(...(ops.length ? ops.map((o) => el('tr', {},
    el('td', {}, o.title || o.type), el('td', { class: 'num' }, Number(o.amount).toFixed(2)),
    el('td', { class: 'mono', title: o.card }, o.card.replace(/\s\S*(\S{4})$/, ' …$1')), el('td', { class: 'mono' }, o.rrn),
    el('td', {}, o.status), el('td', {}, new Date(o.at).toLocaleTimeString('ru-RU'))))
    : [el('tr', {}, el('td', { colspan: '6', class: 'muted' }, 'Операций в пакете нет.'))]));
  $('#detail-ops').hidden = false;
}

// ---------- электронные весы ----------

const SCALE_MODES = { manual: 'вес — на экране весов', ask: 'спрашивать вес при каждом взвешивании' };
const SCALE_LIMITS = [6, 15, 30, 60, 150, 300];
const SCALE_QUICK = [0.25, 0.5, 1, 2.5];

function kg(v) { return Number(v || 0).toFixed(3) + ' кг'; }

function scaleFaults(st) {
  const out = [];
  if (st.weight > st.max_weight) out.push('перегрузка');
  if (!st.stable) out.push('вес не стабилен');
  if (st.placement_error) out.push('товар задевает край');
  return out;
}

function scalesSummary(st) {
  const last = st.last_weighing;
  const faults = scaleFaults(st);
  return [
    ...summaryRow('Нетто', kg(st.net)),
    ...summaryRow('Брутто / тара', `${kg(st.weight)} / ${kg(st.tare)}`),
    ...summaryRow('Предел', `${st.max_weight} кг`),
    ...summaryRow('Откуда вес', `${SCALE_MODES[st.weight_mode] || st.weight_mode}${st.weight_mode === 'ask' ? `, ждать ${st.prompt_timeout_s} с` : ''}`),
    ...summaryRow('Взвешиваний', `${st.weighings}${last ? `, последнее ${kg(last.net)} в ${new Date(last.at).toLocaleTimeString('ru-RU')}` : ''}`),
    ...summaryRow('Неисправности', faults.length ? faults.join(', ') : 'нет'),
  ];
}

function optionList(title, values, cur, label) {
  const vals = values.includes(cur) ? values : [...values, cur];
  const sel = el('select', { title }, ...vals.map((v) => { const o = el('option', { value: String(v) }, label(v)); o.selected = v === cur; return o; }));
  return sel;
}

function scalesActions(st) {
  const nodes = [
    { key: 'stable', label: 'Вес стабилен', on: st.stable },
    { key: 'placement_error', label: 'Товар задевает край', on: st.placement_error, bad: true },
  ].map((t) => {
    const b = el('button', { class: 'toggle' + (t.on ? ' on' : '') + (t.bad ? ' bad' : ''), type: 'button', title: t.key }, t.label);
    b.addEventListener('click', () => setDeviceState({ [t.key]: !t.on }));
    return b;
  });
  const untare = el('button', { class: 'toggle', type: 'button', title: 'tare = 0', disabled: !st.tare }, 'Снять тару');
  untare.addEventListener('click', () => setDeviceState({ tare: 0 }));
  nodes.push(untare);
  // «+/−» к весу на платформе (брутто), как кнопки виртуальных весов.
  for (const d of [-0.1, -0.01, 0.01, 0.1, 1]) {
    const label = (d > 0 ? '+' : '−') + String(Math.abs(d)).replace('.', ',') + ' кг';
    const b = el('button', { class: 'btn', type: 'button', title: 'изменить вес на платформе' }, label);
    b.addEventListener('click', () => setDeviceState({ weight: Math.max(0, Math.round(((st.weight || 0) + d) * 1000) / 1000) }));
    nodes.push(b);
  }
  const mode = optionList('weight_mode', Object.keys(SCALE_MODES), st.weight_mode, (v) => SCALE_MODES[v] || v);
  mode.addEventListener('change', () => setDeviceState({ weight_mode: mode.value }));
  const limit = optionList('max_weight', SCALE_LIMITS, st.max_weight, (v) => `${v} кг`);
  limit.addEventListener('change', () => setDeviceState({ max_weight: Number(limit.value) }));
  const wait = optionList('prompt_timeout_s', [30, 60, 120, 240], st.prompt_timeout_s, (v) => `${v} с`);
  wait.addEventListener('change', () => setDeviceState({ prompt_timeout_s: Number(wait.value) }));
  nodes.push(el('label', {}, 'Вес:', mode), el('label', {}, 'Предел', limit), el('label', {}, 'Ждать', wait));
  return nodes;
}

function parseKg(input) {
  const v = parseFloat(String(input.value).replace(',', '.'));
  return Number.isFinite(v) ? Math.round(v * 1000) / 1000 : NaN;
}

// weightInput — поле веса: текст, а не number — чтобы принимать и «1,245», и «1.245».
function weightInput(value, max) {
  return el('input', { class: 'pos-input', type: 'text', inputmode: 'decimal', autocomplete: 'off',
    title: max ? `от 0 до ${max} кг` : 'кг', value: Number(value || 0).toFixed(3) });
}

function badWeight(msg) {
  const box = $('#detail-error');
  box.textContent = msg;
  box.hidden = false;
}

// renderScaleScreen — табло весов. Без запроса — «платформа»: вес кладут кнопками.
// В режиме ask во время GetWeight — запрос «ВЗВЕШИВАНИЕ» с полем веса.
// Пока экран тот же, обновляются только цифры: введённое в поле не сбрасывается.
function renderScaleScreen(st) {
  const box = $('#detail-screen');
  state.scale = st; // свежее состояние для кнопок, которые не перерисовываются
  const p = st.prompt;
  const key = p ? p.prompt_id : 'scale:' + state.selected;
  const same = box.dataset.prompt === key && !box.hidden;
  box.hidden = false;
  if (p) {
    const left = Math.max(0, Math.round((new Date(p.deadline) - Date.now()) / 1000));
    clearTimeout(state.screenTimer);
    state.screenTimer = setTimeout(refreshTables, 1000);
    if (same) {
      const timer = box.querySelector('.pos-timer');
      if (timer) timer.textContent = `осталось ${left} с`;
      return;
    }
    box.dataset.prompt = key;
    const inp = p.input || {};
    const field = weightInput(inp.value, inp.max);
    const buttons = (p.actions || []).map((a) => {
      const b = el('button', { class: 'btn pos-btn ' + (a.kind || ''), type: 'button' }, a.title);
      b.addEventListener('click', () => {
        if (a.id === 'cancel') return respondPrompt(p, { action: a.id });
        const v = parseKg(field);
        if (!(v >= 0)) return badWeight('Введите вес в килограммах, например 0,532');
        respondPrompt(p, { action: a.id, value: v });
      });
      return b;
    });
    field.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') buttons[0]?.click(); });
    box.replaceChildren(
      el('div', { class: 'pos-display' },
        el('div', { class: 'pos-op' }, p.title),
        el('div', { class: 'pos-text' }, p.text),
        el('div', { class: 'pos-timer' }, `осталось ${left} с`)),
      el('div', { class: 'pos-controls' }, el('label', {}, `${inp.label || 'Вес'}, ${inp.unit || 'кг'}`, field), ...buttons),
    );
    field.focus();
    field.select();
    return;
  }
  const faults = scaleFaults(st);
  const info = `брутто ${kg(st.weight)} · тара ${kg(st.tare)} · ${faults.length ? faults.join(', ') : 'стабилен'}`;
  if (same) {
    box.querySelector('.pos-amount').textContent = kg(st.net);
    box.querySelector('.pos-text').textContent = info;
    box.querySelector('.pos-display').className = 'pos-display' + (faults.length ? ' bad' : '');
    return;
  }
  box.dataset.prompt = key;
  const field = weightInput(st.weight, st.max_weight);
  const put = (w) => { field.value = Number(w).toFixed(3); setDeviceState({ weight: w }); };
  const putBtn = el('button', { class: 'btn pos-btn primary', type: 'button' }, 'Положить');
  putBtn.addEventListener('click', () => { const w = parseKg(field); if (w >= 0) put(w); else badWeight('Введите вес в килограммах, например 0,532'); });
  field.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') putBtn.click(); });
  const quick = SCALE_QUICK.map((w) => {
    const b = el('button', { class: 'btn', type: 'button', title: `положить ${w} кг` }, String(w));
    b.addEventListener('click', () => put(w));
    return b;
  });
  const clear = el('button', { class: 'btn', type: 'button' }, 'Снять');
  clear.addEventListener('click', () => put(0));
  const tare = el('button', { class: 'btn', type: 'button', title: 'тара = вес на платформе' }, '→0← Тара');
  tare.addEventListener('click', () => setDeviceState({ tare: state.scale.weight }));
  box.replaceChildren(
    el('div', { class: 'pos-display' + (faults.length ? ' bad' : '') },
      el('div', { class: 'pos-op' }, 'ВЕСЫ · НЕТТО'),
      el('div', { class: 'pos-amount' }, kg(st.net)),
      el('div', { class: 'pos-text' }, info)),
    el('div', { class: 'pos-controls' }, el('label', {}, 'На платформе, кг', field), putBtn, ...quick, clear, tare),
  );
}

// ---------- весы с печатью этикеток ----------

function uploadText(u, what) {
  if (!u) return 'не было';
  return `${new Date(u.at).toLocaleString('ru-RU')}: пакетов ${u.packages}, ${what} ${u.records}${u.status === 'last' ? '' : ' (не завершена)'}`;
}

function labelSummary(st) {
  return [
    ...summaryRow('Товаров', `${st.goods_count}, с картинками ${(st.images || []).length}`),
    ...summaryRow('Выгрузка товаров', uploadText(st.upload, 'товаров')),
    ...summaryRow('Выгрузка картинок', uploadText(st.images_upload, 'картинок')),
    ...summaryRow('Префикс штрихкода', st.barcode_prefix),
    ...summaryRow('Этикеток напечатано', String(st.labels)),
  ];
}

function labelActions(st) {
  const prefixes = Array.from({ length: 10 }, (_, i) => '2' + i);
  const sel = optionList('barcode_prefix', prefixes, st.barcode_prefix, (v) => v);
  sel.addEventListener('change', () => setDeviceState({ barcode_prefix: sel.value }));
  return [el('label', {}, 'Префикс весового штрихкода:', sel)];
}

async function deviceAction(name, args) {
  const box = $('#detail-error');
  box.hidden = true;
  try {
    const res = await api(`/control/v1/devices/${encodeURIComponent(state.selected)}/actions/${name}`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(args),
    });
    return (await res.json()).result;
  } catch (e) {
    if (!(e instanceof AuthError)) {
      box.textContent = 'Устройство не выполнило действие: ' + e.message;
      box.hidden = false;
    }
    return null;
  } finally {
    refreshTables();
  }
}

async function showImage(g) {
  const res = await deviceAction('image', { plu: g.plu });
  if (!res) return;
  const img = el('img', { alt: g.name });
  img.src = 'data:image/png;base64,' + res.png_base64;
  $('#goods-image').replaceChildren(img, el('div', { class: 'muted' }, `PLU ${g.plu} · ${g.name}`));
  $('#goods-image').hidden = false;
}

// renderGoods — товары, выгруженные из 1С; у каждого кнопка «Этикетка».
// Таблица перерисовывается, только когда изменились товары.
function renderGoods(st) {
  $('#detail-goods').hidden = false;
  const images = new Set(st.images || []);
  const key = state.selected + JSON.stringify(st.goods) + [...images].join(',');
  const body = $('#goods-body');
  $('#goods-meta').textContent = st.goods_count > (st.goods || []).length ? `· первые ${st.goods.length} из ${st.goods_count}` : '';
  if (body.dataset.key === key) return;
  body.dataset.key = key;
  $('#goods-image').hidden = true;
  const goods = st.goods || [];
  body.replaceChildren(...(goods.length ? goods.map((g) => {
    const label = el('button', { class: 'btn', type: 'button', title: g.is_weight ? 'вес — из поля над таблицей' : `штучный: ${g.piece_weight} кг` }, 'Этикетка');
    label.addEventListener('click', () => {
      const args = { plu: g.plu };
      if (g.is_weight || !g.piece_weight) {
        args.weight = parseKg($('#goods-weight'));
        if (!(args.weight > 0)) return badWeight('Введите вес для этикетки в килограммах, например 0,532');
      }
      deviceAction('print_label', args);
    });
    let pic = null;
    if (images.has(g.plu)) {
      pic = el('button', { class: 'btn', type: 'button', title: 'Картинка товара' }, '🖼');
      pic.addEventListener('click', () => showImage(g));
    }
    const sub = [`код ${g.code}`, g.is_weight ? 'весовой' : `штучный ${g.piece_weight || '?'} кг`, g.shelf_life ? `годен ${g.shelf_life} дн.` : ''].filter(Boolean).join(' · ');
    return el('tr', {},
      el('td', { class: 'num mono' }, g.plu),
      el('td', {}, g.name, el('div', { class: 'muted' }, sub)),
      el('td', { class: 'num' }, Number(g.price).toFixed(2)),
      el('td', { class: 'goods-btns' }, pic, label));
  }) : [el('tr', {}, el('td', { colspan: '4', class: 'muted' }, 'Товаров нет: выгрузите их в весы из 1С.'))]));
}
// ---------- принтеры ----------

function printerFaults(st) {
  const out = [];
  if (!st.paper) out.push(st.model === 'label_printer' ? 'кончились этикетки' : 'нет бумаги');
  if (st.cover_open) out.push('крышка открыта');
  if (st.model === 'receipt_printer' && !st.barcodes) out.push('штрихкоды не печатает');
  return out;
}

// printerToggles — бумага и крышка (общие для принтеров).
function printerToggles(st, paperLabel) {
  return [
    { key: 'paper', label: paperLabel, on: st.paper },
    { key: 'cover_open', label: 'Крышка открыта', on: st.cover_open, bad: true },
    ...(st.model === 'receipt_printer' ? [{ key: 'drawer_open', label: 'Денежный ящик открыт', on: st.drawer_open }] : []),
  ].map((t) => {
    const b = el('button', { class: 'toggle' + (t.on ? ' on' : '') + (t.bad ? ' bad' : ''), type: 'button', title: t.key }, t.label);
    b.addEventListener('click', () => setDeviceState({ [t.key]: !t.on }));
    return b;
  });
}

function receiptSummary(st) {
  const faults = printerFaults(st);
  return [
    ...summaryRow('Ширина строки, символов', String(st.line_length)),
    ...summaryRow('Напечатано', `документов ${st.documents}`),
    ...summaryRow('Денежный ящик', st.drawer_opens ? `открывался ${st.drawer_opens} раз, последний — ${new Date(st.drawer_opened_at).toLocaleTimeString('ru-RU')}` : 'не открывался'),
    ...summaryRow('Неисправности', faults.length ? faults.join(', ') : 'нет'),
  ];
}

function receiptActions(st) {
  const nodes = printerToggles(st, 'Бумага');
  const bc = el('button', { class: 'toggle' + (st.barcodes ? ' on' : ''), type: 'button', title: 'barcodes' }, 'Печатает штрихкоды');
  bc.addEventListener('click', () => setDeviceState({ barcodes: !st.barcodes }));
  const width = optionList('line_length', [32, 42, 48, 57, 64], st.line_length, (v) => String(v));
  width.addEventListener('change', () => setDeviceState({ line_length: Number(width.value) }));
  nodes.push(bc, el('label', {}, 'Ширина строки, символов:', width));
  return nodes;
}

function labelPrinterSummary(st) {
  const j = st.job;
  const faults = printerFaults(st);
  return [
    ...summaryRow('Инициализация', st.initialized ? `да, ${st.inits} раз` : 'не было (печати не мешает)'),
    ...summaryRow('Задание', j ? `${j.open ? 'печатается' : 'завершено'}: этикетка ${j.width_mm}×${j.height_mm} мм, пакетов ${j.packages} (последний — ${j.status}), этикеток ${j.labels}, копий ${j.copies}` : 'не было'),
    ...summaryRow('Всего', `заданий ${st.jobs}, этикеток напечатано ${st.labels_printed}`),
    ...summaryRow('Неисправности', faults.length ? faults.join(', ') : 'нет'),
  ];
}

// Предпросмотр этикетки по разметке из 1С: поля в миллиметрах, текст — только textContent.
const LABEL_PX = 230; // ширина превью

function hashOf(s) {
  let h = 2166136261;
  for (const c of String(s)) h = Math.imul(h ^ c.codePointAt(0), 16777619);
  return h >>> 0;
}

// barcodeCanvas — условный рисунок штрихкода: полосы или матрица, зависят от значения.
function barcodeCanvas(type, value, w, h) {
  const c = document.createElement('canvas');
  c.width = Math.max(4, Math.round(w));
  c.height = Math.max(4, Math.round(h));
  const g = c.getContext('2d');
  g.fillStyle = '#111';
  let seed = hashOf(value) || 1;
  const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return (seed >>> 0) / 4294967296; };
  if (/^(qr|qrcode|datamatrix)$/i.test(type)) {
    const side = Math.min(c.width, c.height);
    const n = 21, m = side / n;
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) if (rnd() < 0.5) g.fillRect(x * m, y * m, Math.ceil(m), Math.ceil(m));
    if (/qr/i.test(type)) {
      for (const [fx, fy] of [[0, 0], [n - 7, 0], [0, n - 7]]) {
        g.clearRect(fx * m, fy * m, 7 * m, 7 * m);
        g.fillRect(fx * m, fy * m, 7 * m, 7 * m);
        g.clearRect((fx + 1) * m, (fy + 1) * m, 5 * m, 5 * m);
        g.fillRect((fx + 2) * m, (fy + 2) * m, 3 * m, 3 * m);
      }
    }
    return c;
  }
  let x = 0;
  while (x < c.width) {
    const bar = 1 + Math.floor(rnd() * 3), gap = 1 + Math.floor(rnd() * 2);
    g.fillRect(x, 0, bar, c.height);
    x += bar + gap;
  }
  return c;
}

function labelPreview(layout, lbl) {
  const k = LABEL_PX / layout.width_mm; // px на мм
  const box = el('div', { class: 'lbl', title: `${layout.width_mm}×${layout.height_mm} мм` });
  box.style.width = `${LABEL_PX}px`;
  box.style.height = `${layout.height_mm * k}px`;
  for (const f of layout.fields) {
    const v = (lbl.values || {})[f.name] ?? '';
    const w = (f.width || layout.width_mm - f.left) * k, h = (f.height || 5) * k;
    const node = el('div', { class: 'lbl-f lbl-' + f.type, title: f.name });
    Object.assign(node.style, { left: `${f.left * k}px`, top: `${f.top * k}px`, width: `${w}px`, height: `${h}px` });
    if (f.orientation) node.style.transform = `rotate(${f.orientation}deg)`;
    for (const side of ['Left', 'Top', 'Right', 'Bottom']) {
      if ((f.border || '').includes(side)) node.style['border' + side] = '1px solid #111';
    }
    if (f.type === 'text') {
      const fs = (f.font_size || 8) * 0.3528 * k; // pt → мм → px
      const style = f.font_style || '';
      Object.assign(node.style, {
        fontSize: `${fs}px`, fontWeight: style.includes('Bold') ? '700' : '400', fontStyle: style.includes('Italic') ? 'italic' : 'normal',
        textDecoration: [style.includes('Underline') ? 'underline' : '', /strikeout/i.test(style) ? 'line-through' : ''].join(' ').trim() || 'none',
        textAlign: (f.align || 'left').toLowerCase(), whiteSpace: f.multiline ? 'normal' : 'nowrap',
        alignItems: { top: 'flex-start', center: 'center', bottom: 'flex-end' }[(f.valign || 'top').toLowerCase()] || 'flex-start',
      });
      node.append(el('span', {}, v));
    } else if (f.type === 'barcode') {
      const hri = f.hri ? Math.max(8, (f.font_size || 8) * 0.3528 * k) : 0;
      node.append(barcodeCanvas(f.barcode_type, v, w, Math.max(4, h - hri)));
      if (f.hri) {
        const t = el('div', { class: 'lbl-hri' }, v);
        t.style.fontSize = `${hri * 0.85}px`;
        node.append(t);
      }
    } else if (f.type === 'image') {
      if (v) {
        const img = el('img', { alt: f.name });
        img.src = 'data:image/png;base64,' + v;
        node.append(img);
      } else {
        node.append(el('span', { class: 'lbl-noimg' }, 'картинка'));
      }
    }
    box.append(node);
  }
  return el('div', { class: 'lbl-wrap' }, box, el('div', { class: 'muted' }, `× ${lbl.quantity}`));
}

// labelsCard — пакет этикеток на ленте: превью и текст под спойлером.
function labelsCard(doc) {
  const lay = doc.layout;
  const meta = el('div', { class: 'receipt-meta' },
    el('b', {}, KIND_NAMES[doc.kind] || doc.kind),
    el('span', {}, `${lay.width_mm}×${lay.height_mm} мм, этикеток ${lay.total}`),
    el('span', {}, new Date(doc.time).toLocaleString('ru-RU')));
  const previews = el('div', { class: 'lbl-list' }, ...(lay.labels || []).map((l) => labelPreview(lay, l)));
  const more = lay.total > (lay.labels || []).length ? el('div', { class: 'muted' }, `показаны первые ${lay.labels.length} из ${lay.total}`) : null;
  const text = el('details', { class: 'raw' }, el('summary', {}, 'Текстом'), el('pre', {}, (doc.lines || []).join('\n')));
  return el('div', { class: 'receipt labels' }, meta, previews, more, text);
}
// ---------- устройства ввода: сканер, считыватель карт ----------

const EVENT_FORMATS = { plain: 'строкой', base64: 'в Base64' };
const SCAN_KINDS = [
  ['ean13', 'EAN-13'], ['ean8', 'EAN-8'], ['weight', 'Весовой'], ['datamatrix', 'Код маркировки'], ['code128', 'Code128'], ['qr', 'QR'],
];

// showCode — непечатаемые символы видно: GS как ␝, CR как ␍.
function showCode(s) {
  return String(s).replace(/\x1d/g, '␝').replace(/\r/g, '␍').replace(/\n/g, '␊');
}

function listenersText(info) {
  const st = info.state || {};
  // Сканер и считыватель в режиме клавиатуры: код вводит трей рабочего места (docs/models/input.md).
  const kb = st.input_mode === 'keyboard' || st.input_mode === 'both'
    ? `трей ${info.workstation_id} введёт код в активное окно через ${(st.keyboard_delay_ms / 1000).toFixed(1)} с` : '';
  if (st.input_mode === 'keyboard') return 'как клавиатура: ' + kb;
  const drv = info.event_listeners ? 'драйвер слушает события' : 'драйвер сейчас не слушает — событие уйдёт в журнал, но в 1С не попадёт';
  return kb ? drv + '; и ' + kb : drv;
}

function inputSummary(st, info) {
  const rows = [
    ...summaryRow('События в 1С', `${st.model === 'scanner' ? 'Barcode' : 'TracksData'}, ${EVENT_FORMATS[st.event_format] || st.event_format}`),
    ...summaryRow('Связь с драйвером', listenersText(info)),
  ];
  if (st.model === 'card_reader') {
    rows.push(...summaryRow('Дорожки', (st.tracks || []).map((t, i) => t.use
      ? `${i + 1}${t.prefix ? ` префикс ${t.prefix}` : ''}${t.suffix ? ` суффикс ${t.suffix}` : ''}` : null).filter(Boolean).join('; ') || 'ни одной'));
  }
  rows.push(...summaryRow(st.model === 'scanner' ? 'Отсканировано' : 'Проведено карт', String(st.scans ?? st.swipes ?? 0)));
  return rows;
}

function inputActions(st) {
  const fmt = optionList('event_format', Object.keys(EVENT_FORMATS), st.event_format, (v) => EVENT_FORMATS[v]);
  fmt.addEventListener('change', () => setDeviceState({ event_format: fmt.value }));
  const nodes = [el('label', {}, 'Событие:', fmt)];
  if (st.model === 'card_reader') {
    // Дорожки: передаётся ли, префикс и суффикс — коды символов (как в настройке считывателя в БПО).
    const rows = (st.tracks || []).map((t, i) => {
      const use = el('input', { type: 'checkbox' });
      use.checked = t.use;
      const pre = el('input', { class: 'pos-input code-input', type: 'text', inputmode: 'numeric', value: String(t.prefix), title: 'префикс, код символа' });
      const suf = el('input', { class: 'pos-input code-input', type: 'text', inputmode: 'numeric', value: String(t.suffix), title: 'суффикс, код символа (13 — CR)' });
      return { use, pre, suf, node: el('label', { class: 'track-row' }, use, `дорожка ${i + 1}`, 'префикс', pre, 'суффикс', suf) };
    });
    const save = el('button', { class: 'btn', type: 'button' }, 'Сохранить дорожки');
    save.addEventListener('click', () => setDeviceState({
      tracks: rows.map((r) => ({ use: r.use.checked, prefix: Number(r.pre.value) || 0, suffix: Number(r.suf.value) || 0 })),
    }));
    nodes.push(el('div', { class: 'tracks' }, ...rows.map((r) => r.node), save));
  }
  return nodes;
}

// renderInputPanel — «сканировать» или «провести карту». Поле не перерисовывается, пока
// карточка та же: обновляются только связь с драйвером и последние события.
function renderInputPanel(st, info) {
  const box = $('#detail-screen');
  const key = 'input:' + state.selected;
  const scanner = st.model === 'scanner';
  if (box.dataset.prompt !== key || box.hidden) {
    box.dataset.prompt = key;
    const act = (args) => deviceAction(scanner ? 'scan' : 'swipe', args);
    let controls;
    if (scanner) {
      const code = el('input', { class: 'pos-input scan-input', type: 'text', autocomplete: 'off', placeholder: 'штрихкод или код маркировки' });
      const go = el('button', { class: 'btn pos-btn primary', type: 'button' }, 'Сканировать');
      go.addEventListener('click', () => { if (code.value.trim()) act({ barcode: code.value.trim().replace(/␝/g, '\x1d') }); });
      code.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') go.click(); });
      const keep = el('button', { class: 'btn', type: 'button', title: 'Сохранить код из поля в библиотеку устройства' }, 'В библиотеку');
      keep.addEventListener('click', () => {
        const barcode = code.value.trim().replace(/␝/g, '\x1d');
        if (!barcode) return;
        const title = prompt('Подпись для кода в библиотеке:', barcode.slice(0, 40));
        if (title) saveLibrary([...(state.library || []), { title: title.trim(), barcode }]);
      });
      const gens = SCAN_KINDS.map(([k, title]) => {
        const b = el('button', { class: 'btn', type: 'button', title: 'сгенерировать и отсканировать' }, title);
        b.addEventListener('click', () => act({ kind: k }));
        return b;
      });
      controls = [el('div', { class: 'pos-controls' }, code, go, keep), el('div', { class: 'pos-controls' }, el('span', { class: 'muted' }, 'Сгенерировать:'), ...gens)];
    } else {
      const presets = [['discount', 'Дисконтная карта'], ['employee', 'Карта кассира']].map(([k, title]) => {
        const b = el('button', { class: 'btn pos-btn primary', type: 'button' }, title);
        b.addEventListener('click', () => act({ card: k }));
        return b;
      });
      const tracks = [1, 2, 3].map((n) => el('input', { class: 'pos-input scan-input', type: 'text', autocomplete: 'off', placeholder: `дорожка ${n}` }));
      const go = el('button', { class: 'btn', type: 'button' }, 'Провести');
      go.addEventListener('click', () => act({ track1: tracks[0].value, track2: tracks[1].value, track3: tracks[2].value }));
      const keep = el('button', { class: 'btn', type: 'button', title: 'Сохранить дорожки из полей в библиотеку устройства' }, 'В библиотеку');
      keep.addEventListener('click', () => {
        const [track1, track2, track3] = tracks.map((x) => x.value);
        if (!track1 && !track2 && !track3) return;
        const title = prompt('Подпись для карты в библиотеке:', 'Карта ' + ((state.library || []).length + 1));
        if (title) saveLibrary([...(state.library || []), { title: title.trim(), track1, track2, track3 }]);
      });
      controls = [el('div', { class: 'pos-controls' }, ...presets), el('div', { class: 'pos-controls tracks-input' }, ...tracks, go, keep)];
    }
    box.replaceChildren(
      el('div', { class: 'pos-display idle input-display' },
        el('div', { class: 'pos-op' }, scanner ? 'СКАНЕР ШТРИХКОДА' : 'СЧИТЫВАТЕЛЬ КАРТ'),
        el('div', { class: 'pos-text input-listen' })),
      ...controls,
      el('div', { class: 'pos-controls input-library' }),
      el('div', { class: 'input-log' }));
  }
  box.hidden = false;
  renderLibrary(box.querySelector('.input-library'), st, scanner);
  box.querySelector('.input-listen').textContent = listenersText(info);
  box.querySelector('.input-display').classList.toggle('bad', !info.event_listeners && (info.state || {}).input_mode !== 'keyboard');
  const last = st.last || [];
  box.querySelector('.input-log').replaceChildren(...(last.length ? last.map((r) => el('div', { class: 'input-log-row' },
    el('span', { class: 'muted' }, new Date(r.at).toLocaleTimeString('ru-RU')),
    el('span', { class: 'chip' }, r.event),
    el('span', { class: 'mono' }, showCode(r.barcode ?? r.data))))
    : [el('div', { class: 'muted' }, scanner ? 'Ещё ничего не сканировали.' : 'Карты ещё не проводили.')]));
}
// Библиотека тестовых данных устройства (docs/models/input.md): нажал — отсканировалось или
// проведено; ✕ — убрать. Хранится настройкой устройства (library), переживает перезапуск.
function renderLibrary(box, st, scanner) {
  const lib = st.library || [];
  state.library = lib;
  const sig = JSON.stringify(lib);
  if (box.dataset.sig === sig) return;
  box.dataset.sig = sig;
  if (!lib.length) {
    box.replaceChildren(el('span', { class: 'muted' }, scanner
      ? 'Библиотека пуста: введите код и нажмите «В библиотеку».'
      : 'Библиотека пуста: заполните дорожки и нажмите «В библиотеку».'));
    return;
  }
  box.replaceChildren(el('span', { class: 'muted' }, 'Библиотека:'), ...lib.map((item, i) => {
    const title = scanner ? showCode(item.barcode) : [item.track1, item.track2, item.track3].filter(Boolean).join(' | ');
    const b = el('button', { class: 'btn', type: 'button', title }, item.title);
    b.addEventListener('click', () => deviceAction(scanner ? 'scan' : 'swipe', scanner ? { barcode: item.barcode } : { card: item.title }));
    const drop = el('button', { class: 'btn lib-x', type: 'button', title: 'Убрать из библиотеки' }, '✕');
    drop.addEventListener('click', () => {
      if (confirm(`Убрать «${item.title}» из библиотеки?`)) saveLibrary(lib.filter((_, k) => k !== i));
    });
    return el('span', { class: 'lib-item' }, b, drop);
  }));
}

function saveLibrary(library) { setDeviceState({ library }); }

// ---------- дисплей покупателя, ТСД, RFID, распознавание ----------

// panel — карточка-панель в #detail-screen: строится один раз на устройство (поля ввода не
// сбрасываются), refresh(box) обновляет изменчивое при каждом опросе.
function panel(kind, build, refresh) {
  const box = $('#detail-screen');
  const key = kind + ':' + state.selected;
  if (box.dataset.prompt !== key || box.hidden) {
    box.dataset.prompt = key;
    box.replaceChildren(...build());
  }
  box.hidden = false;
  refresh(box);
}

function toggleButtons(st, list) {
  return list.map((t) => {
    const on = !!st[t.key];
    const b = el('button', { class: 'toggle' + (on ? ' on' : '') + (t.bad ? ' bad' : ''), type: 'button', title: t.key }, t.label);
    b.addEventListener('click', () => setDeviceState({ [t.key]: !on }));
    return b;
  });
}

const OP_STATUS = { 0: 'не задан', 1: 'операция выполнена', 2: 'операция не выполнена' };
const WP_STATUS = { 0: 'не задан', 1: 'сервисный режим', 2: 'свободно', 3: 'работает', 4: 'ошибка' };

function displaySummary(st, info) {
  const can = [['text', 'текст'], ['qr', 'QR'], ['html', 'HTML'], ['graphics', 'графика'], ['status', 'статус'],
    ['workplace_status', 'статус РМ'], ['interactive', 'нажатия']].filter(([k]) => st[k]).map(([, t]) => t).join(', ');
  return [
    ...summaryRow('Экран', `${st.columns} символов × ${st.rows} строк`),
    ...summaryRow('Умеет', can || 'ничего'),
    ...summaryRow('Медиафайлы', (st.media || []).map((f) => `${f.name} (${f.size * 0.75 < 1024 ? Math.round(f.size * 0.75) + ' Б' : Math.round(f.size * 0.75 / 1024) + ' КБ'})`).join(', ') || 'нет'),
    ...summaryRow('Выводов', `${st.outputs}, нажатий ${st.clicks}, звуковых сигналов ${st.beeps}`),
    ...summaryRow('Связь с драйвером', listenersText(info)),
  ];
}

function displayActions(st) {
  const nodes = toggleButtons(st, [
    { key: 'text', label: 'Текст' }, { key: 'qr', label: 'QR' }, { key: 'html', label: 'HTML' },
    { key: 'status', label: 'Статус операции' }, { key: 'workplace_status', label: 'Статус РМ' },
    { key: 'interactive', label: 'Сенсорный экран' }, { key: 'broken', label: 'Не отвечает', bad: true },
  ]);
  const cols = optionList('columns', [16, 20, 40], st.columns, String);
  cols.addEventListener('change', () => setDeviceState({ columns: Number(cols.value) }));
  const rows = optionList('rows', [1, 2, 4], st.rows, String);
  rows.addEventListener('change', () => setDeviceState({ rows: Number(rows.value) }));
  nodes.push(el('label', {}, 'Символов:', cols), el('label', {}, 'строк:', rows));
  return nodes;
}

const mediaCache = new Map(); // устройство:имя:время → data URI

async function displayHtml(html, media) {
  // Медиафайлы дисплея подставляем в src как data: — HTML показывается в iframe без скриптов.
  let out = html;
  for (const f of media || []) {
    if (!out.includes(f.name)) continue;
    const key = `${state.selected}:${f.name}:${f.at}`;
    if (!mediaCache.has(key)) {
      const res = await deviceAction('media', { name: f.name });
      if (!res) continue;
      const ext = (f.name.split('.').pop() || '').toLowerCase();
      const mime = { png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', svg: 'image/svg+xml', mp4: 'video/mp4', webp: 'image/webp' }[ext] || 'application/octet-stream';
      mediaCache.set(key, `data:${mime};base64,${res.data_base64}`);
    }
    out = out.split(`"${f.name}"`).join(`"${mediaCache.get(key)}"`).split(`'${f.name}'`).join(`'${mediaCache.get(key)}'`);
  }
  return out;
}

function renderDisplayPanel(st, info) {
  panel('display', () => {
    const elem = el('input', { class: 'pos-input scan-input', type: 'text', placeholder: 'имя элемента, на который нажали' });
    const press = el('button', { class: 'btn', type: 'button' }, 'Нажать');
    press.addEventListener('click', () => { if (elem.value.trim()) deviceAction('click', { element: elem.value.trim() }); });
    return [el('div', { class: 'cd-screen' }), el('div', { class: 'cd-lamps' }), el('div', { class: 'pos-controls cd-touch' }, elem, press)];
  }, (box) => {
    const scr = box.querySelector('.cd-screen');
    const key = `${st.updated_at}|${st.shown}|${st.broken}|${st.columns}|${st.rows}`;
    if (scr.dataset.key !== key) {
      scr.dataset.key = key;
      const nodes = [];
      if (st.broken) nodes.push(el('div', { class: 'cd-off' }, 'ДИСПЛЕЙ НЕ ОТВЕЧАЕТ'));
      else if (st.shown === 'html') {
        const frame = el('iframe', { class: 'cd-html', sandbox: '', title: 'HTML дисплея покупателя' });
        displayHtml(st.html_view, st.media).then((h) => { frame.srcdoc = h; });
        nodes.push(frame);
      } else {
        const vfd = el('div', { class: 'cd-vfd' });
        vfd.style.width = `${st.columns + 2}ch`;
        for (const l of st.lines || []) vfd.append(el('div', {}, l || ' '));
        nodes.push(vfd);
        if (st.shown === 'qr' && st.qr_view) {
          if (st.qr_view.picture) {
            const img = el('img', { class: 'cd-qr', alt: 'QR' });
            img.src = 'data:image/png;base64,' + st.qr_view.picture;
            nodes.push(img);
          }
          nodes.push(el('div', { class: 'mono cd-qrtext' }, 'QR: ' + st.qr_view.value));
        }
      }
      scr.replaceChildren(...nodes);
    }
    box.querySelector('.cd-lamps').replaceChildren(
      el('span', { class: 'chip' + (st.operation_status === 1 ? ' ok' : st.operation_status === 2 ? ' bad' : '') }, 'Операция: ' + OP_STATUS[st.operation_status]),
      el('span', { class: 'chip' + (st.workplace === 4 ? ' bad' : st.workplace === 3 ? ' ok' : '') }, 'Рабочее место: ' + WP_STATUS[st.workplace]),
      el('span', { class: 'muted' }, info.event_listeners ? '' : 'драйвер не слушает события'));
    box.querySelector('.cd-touch').hidden = !st.interactive;
  });
}

function tsdSummary(st) {
  const up = st.upload;
  return [
    ...summaryRow('Товаров в ТСД', String(st.goods_count)),
    ...summaryRow('Выгрузка из 1С', up ? `${new Date(up.at).toLocaleString('ru-RU')}: ${up.full_load ? 'полная' : 'частичная'}, пакетов ${up.packages}, записей ${up.records}` : 'не было'),
    ...summaryRow('Собранный документ', `строк ${(st.document || []).length}, загружался в 1С ${st.downloads} раз`),
  ];
}

function renderTsdPanel(st) {
  panel('tsd', () => {
    const code = el('input', { class: 'pos-input scan-input', type: 'text', autocomplete: 'off', placeholder: 'штрихкод' });
    const qty = el('input', { class: 'pos-input code-input', type: 'text', inputmode: 'decimal', value: '1', title: 'количество' });
    const add = el('button', { class: 'btn pos-btn primary', type: 'button' }, 'Отобрать');
    add.addEventListener('click', () => { if (code.value.trim()) deviceAction('collect', { barcode: code.value.trim(), quantity: parseKg(qty) || 1 }); });
    code.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') add.click(); });
    const clear = el('button', { class: 'btn', type: 'button' }, 'Очистить документ');
    clear.addEventListener('click', () => deviceAction('clear_document', {}));
    return [el('div', { class: 'pos-display idle' }, el('div', { class: 'pos-op' }, 'ТЕРМИНАЛ СБОРА ДАННЫХ'), el('div', { class: 'pos-text' }, 'Документ собирают здесь, 1С забирает его загрузкой из ТСД.')),
      el('div', { class: 'pos-controls' }, code, 'кол-во', qty, add, clear),
      el('h3', {}, 'Собранный документ'), el('div', { class: 'tsd-doc' }), el('h3', {}, 'Товары в ТСД'), el('div', { class: 'tsd-goods' })];
  }, (box) => {
    const doc = st.document || [];
    box.querySelector('.tsd-doc').replaceChildren(...(doc.length ? doc.map((it) => {
      const drop = el('button', { class: 'btn', type: 'button', title: 'убрать строку' }, '✕');
      drop.addEventListener('click', () => deviceAction('remove', { barcode: it.barcode }));
      return el('div', { class: 'input-log-row' }, el('span', { class: 'mono' }, it.barcode), el('span', {}, it.name || ''), el('b', {}, `× ${it.quantity}`), drop);
    }) : [el('div', { class: 'muted' }, 'Пусто.')]));
    const goods = (st.goods || []).slice(0, 50);
    const gkey = JSON.stringify(goods);
    const g = box.querySelector('.tsd-goods');
    if (g.dataset.key !== gkey) {
      g.dataset.key = gkey;
      g.replaceChildren(...(goods.length ? goods.map((x) => {
        const plus = el('button', { class: 'btn', type: 'button', title: 'отобрать 1' }, '+1');
        plus.addEventListener('click', () => deviceAction('collect', { barcode: x.barcode, quantity: 1 }));
        return el('div', { class: 'input-log-row' }, plus, el('span', { class: 'mono' }, x.barcode),
          el('span', {}, [x.name, x.characteristic, x.series].filter(Boolean).join(' · ')), el('span', { class: 'muted' }, x.price ? money(x.price) : ''));
      }) : [el('div', { class: 'muted' }, 'Товаров нет: выгрузите их в ТСД из 1С.')]),
      st.goods_count > goods.length ? el('div', { class: 'muted' }, `показаны первые ${goods.length} из ${st.goods_count}`) : '');
    }
  });
}

function rfidSummary(st, info) {
  return [
    ...summaryRow('Сессия чтения', st.session ? 'открыта — новые метки уходят в 1С' : 'закрыта — 1С метки не читает'),
    ...summaryRow('Меток в поле', String((st.field || []).length)),
    ...summaryRow('Записей в метки', String(st.writes)),
    ...summaryRow('Связь с драйвером', listenersText(info)),
  ];
}

function renderRfidPanel(st) {
  panel('rfid', () => {
    const gen = el('button', { class: 'btn pos-btn primary', type: 'button' }, 'Положить новую метку');
    gen.addEventListener('click', () => deviceAction('add_tag', {}));
    const tid = el('input', { class: 'pos-input scan-input', type: 'text', placeholder: 'TID (HEX)' });
    const epc = el('input', { class: 'pos-input scan-input', type: 'text', placeholder: 'EPC (HEX)' });
    const put = el('button', { class: 'btn', type: 'button' }, 'Положить');
    put.addEventListener('click', () => deviceAction('add_tag', { tid: tid.value.trim(), epc: epc.value.trim() }));
    const clear = el('button', { class: 'btn', type: 'button' }, 'Убрать все');
    clear.addEventListener('click', () => deviceAction('clear_field', {}));
    return [el('div', { class: 'pos-controls' }, gen, clear), el('div', { class: 'pos-controls tracks-input' }, tid, epc, put),
      el('h3', {}, 'Метки в поле антенны'), el('div', { class: 'rfid-field' }), el('h3', {}, 'Пакеты для 1С'), el('div', { class: 'rfid-pkgs' })];
  }, (box) => {
    const field = st.field || [];
    box.querySelector('.rfid-field').replaceChildren(...(field.length ? field.map((t) => {
      const drop = el('button', { class: 'btn', type: 'button', title: 'убрать из поля' }, '✕');
      drop.addEventListener('click', () => deviceAction('remove_tag', { tid: t.tid }));
      return el('div', { class: 'input-log-row' }, drop, el('span', { class: 'mono' }, 'TID ' + t.tid), el('span', { class: 'mono' }, 'EPC ' + t.epc),
        t.user ? el('span', { class: 'mono' }, 'USER ' + t.user) : null);
    }) : [el('div', { class: 'muted' }, 'Поле пустое.')]));
    const pk = st.packages || [];
    box.querySelector('.rfid-pkgs').replaceChildren(...(pk.length ? pk.map((p) => el('div', { class: 'input-log-row' },
      el('span', { class: 'muted' }, new Date(p.at).toLocaleTimeString('ru-RU')), el('b', {}, p.id), el('span', {}, `меток ${p.tags.length}`),
      el('span', { class: 'chip' + (p.delivered ? ' ok' : ' wait') }, p.delivered ? '1С забрала' : 'ждёт 1С')))
      : [el('div', { class: 'muted' }, 'Пакетов не было.')]));
  });
}

function recognizerSummary(st) {
  const l = st.last;
  return [
    ...summaryRow('На весах', (st.candidates || []).map((c) => `${c.code} (${Math.round(c.percent * 100)}%)`).join(', ') || 'пусто'),
    ...summaryRow('Последнее распознавание', l ? `${new Date(l.at).toLocaleTimeString('ru-RU')}: ${l.result.map((c) => c.code).join(', ') || 'ничего'}${l.training ? ', с обучением' : ''}` : 'не было'),
    ...summaryRow('Подтверждения', (st.confirmations || []).map((c) => c.code).join(', ') || 'нет'),
  ];
}

function recognizerActions(st) {
  return toggleButtons(st, [{ key: 'get_image', label: 'Отдаёт изображение' }, { key: 'hardware', label: 'Аппаратное распознавание' }]);
}

function renderRecognizerPanel(st) {
  panel('recognizer', () => {
    const area = el('textarea', { class: 'pos-input cand-input', rows: '4', title: 'код = вероятность от 0 до 1, по строке на товар' });
    area.value = (st.candidates || []).map((c) => `${c.code} = ${c.percent}`).join('\n');
    const save = el('button', { class: 'btn pos-btn primary', type: 'button' }, 'Положить на весы');
    save.addEventListener('click', () => {
      const list = area.value.split('\n').map((l) => l.trim()).filter(Boolean).map((l) => {
        const [code, pct] = l.split(/[=;\s]+/);
        return { code: Number(code), percent: Number(String(pct).replace(',', '.')) };
      });
      setDeviceState({ candidates: list });
    });
    const shot = el('button', { class: 'btn', type: 'button' }, 'Снимок камеры');
    shot.addEventListener('click', async () => {
      const res = await deviceAction('image', {});
      if (!res) return;
      const img = el('img', { alt: 'снимок' });
      img.src = 'data:image/png;base64,' + res.png_base64;
      $('#detail-screen').querySelector('.rec-shot').replaceChildren(img);
    });
    return [el('div', { class: 'pos-display idle' }, el('div', { class: 'pos-op' }, 'УСТРОЙСТВО РАСПОЗНАВАНИЯ'),
      el('div', { class: 'pos-text' }, 'Что «видит» камера: код товара и вероятность, по строке на товар.')),
    el('div', { class: 'pos-controls' }, area), el('div', { class: 'pos-controls' }, save, shot), el('div', { class: 'rec-shot' })];
  }, () => {});
}

// Переключатели состояния ККТ: ключ PUT /control/v1/devices/{id}/state.
// bad — включённое состояние означает неисправность.
const KKT_TOGGLES = [
  { key: 'drawer_open', label: 'Денежный ящик открыт', get: (s) => s.drawer_open },
  { key: 'paper', label: 'Бумага', get: (s) => s.faults.paper },
  { key: 'cover_open', label: 'Крышка открыта', get: (s) => s.faults.cover_open, bad: true },
  { key: 'ofd_online', label: 'Связь с ОФД', get: (s) => s.ofd.online },
  { key: 'fn_error', label: 'Срочная замена ФН', get: (s) => s.faults.fn_error, bad: true },
  { key: 'fn_overflow', label: 'Память ФН переполнена', get: (s) => s.faults.fn_overflow, bad: true },
  { key: 'fn_fail', label: 'Ресурс ФН исчерпан', get: (s) => s.faults.fn_fail, bad: true },
  { key: 'print_error_after_fiscal', label: 'Сбой печати после фискализации', get: (s) => s.faults.print_error_after_fiscal, bad: true },
  { key: 'fiscal', label: 'Зарегистрирована', get: (s) => s.fiscal },
  { key: 'marking_strict', label: 'Строгая проверка КМ', get: (s) => s.km.strict },
];

// Клик — только выделить строку; двойной клик (или Enter) — карточка вместо списка.
function highlightDevice(id) {
  state.highlighted = id;
  document.querySelectorAll('#devices-body tr[data-id]').forEach((tr) => tr.classList.toggle('selected', tr.dataset.id === id));
}

function selectDevice(id) {
  if (state.selected !== id) { state.tapeLimit = 20; state.tapeFilter = 'all'; }
  state.selected = id;
  state.highlighted = id;
  showCard(true);
  refreshTables();
  window.scrollTo(0, 0);
}

// showCard — карточка на месте списка устройств (список, фильтры и классы прячутся).
function showCard(on) {
  $('#view-devices .split').hidden = on;
  $('#view-devices .view-tools').hidden = on;
  $('#device-detail').hidden = !on;
}

function closeDetail() {
  state.selected = null;
  state.metaFor = null;
  showCard(false);
  refreshTables();
}
$('#detail-close').addEventListener('click', closeDetail);
document.addEventListener('keydown', (ev) => {
  if (state.view !== 'devices') return;
  if (ev.key === 'Escape' && state.selected && !ev.target.closest('input, textarea, select')) closeDetail();
  if (ev.key === 'Enter' && !state.selected && state.highlighted && ev.target === document.body) selectDevice(state.highlighted);
});

async function setDeviceState(patch) {
  const box = $('#detail-error');
  box.hidden = true;
  try {
    await api(`/control/v1/devices/${encodeURIComponent(state.selected)}/state`, {
      method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(patch),
    });
  } catch (e) {
    if (e instanceof AuthError) return;
    box.textContent = 'Не удалось изменить состояние: ' + e.message;
    box.hidden = false;
  }
  refreshTables();
}

function summaryRow(dt, dd) {
  return [el('dt', {}, dt), el('dd', {}, dd)];
}

function kktSummary(st) {
  const sh = st.shift || {};
  const rows = [
    ...summaryRow('Регистрация', st.fiscal ? `РН ${st.kkt_number}` : 'не зарегистрирована'),
    ...summaryRow('ФН', `${st.fn_serial}${st.fn_closed ? ' (закрыт)' : ''}`),
    ...summaryRow('Смена', sh.number ? `№${sh.number}, ${SHIFT_STATES[sh.state]}${sh.opened_at ? ', открыта ' + new Date(sh.opened_at).toLocaleString('ru-RU') : ''}` : 'ещё не открывалась'),
    ...summaryRow('Чеков за смену', String(sh.checks ?? 0)),
    ...summaryRow('Последний ФД', String(st.doc_number)),
    ...summaryRow('Наличные', Number(st.cash_balance).toFixed(2)),
    ...summaryRow('ОФД', st.ofd.online ? `на связи${st.ofd.backlog ? ', в очереди ' + st.ofd.backlog : ''}` : `нет связи, в очереди ${st.ofd.backlog}`),
    ...summaryRow('Маркировка', `сессия ${st.km.session ? 'открыта' : 'закрыта'}, запросов ${st.km.requests.length}, ответ ОИСМ: ${st.km.result}`),
  ];
  const faults = kktFaults(st);
  rows.push(...summaryRow('Неисправности', faults.length ? faults.join(', ') : 'нет'));
  return rows;
}

function kktActions(st) {
  const nodes = KKT_TOGGLES.map((t) => {
    const on = !!t.get(st);
    const b = el('button', { class: 'toggle' + (on ? ' on' : '') + (t.bad ? ' bad' : ''), type: 'button', title: t.key }, t.label);
    b.addEventListener('click', () => setDeviceState({ [t.key]: !on }));
    return b;
  });
  const expire = el('button', {
    class: 'toggle', type: 'button', disabled: !(st.shift && st.shift.state === 2),
    title: 'shift_opened_hours_ago = 25',
  }, 'Смена открыта 25 ч назад');
  expire.addEventListener('click', () => setDeviceState({ shift_opened_hours_ago: 25 }));
  nodes.push(expire);

  const sel = el('select', { title: 'km_result' },
    ...[['valid', 'КМ корректен'], ['invalid', 'КМ некорректен'], ['unavailable', 'ОИСМ недоступна']]
      .map(([v, l]) => { const o = el('option', { value: v }, l); o.selected = st.km.result === v; return o; }));
  sel.addEventListener('change', () => setDeviceState({ km_result: sel.value }));
  nodes.push(el('label', {}, 'Ответ ОИСМ:', sel));
  return nodes;
}

// qrCanvas — QR-код документа по готовой матрице сервера (PrintedDocument.qr): тёмные модули
// на белом с полем 4 модуля — так его читает телефон и с экрана.
function qrCanvas(q, scale = 4) {
  const quiet = 4, side = (q.size + quiet * 2) * scale;
  const c = el('canvas', { class: 'qr', width: String(side), height: String(side), title: q.data });
  const g = c.getContext('2d');
  g.fillStyle = '#fff';
  g.fillRect(0, 0, side, side);
  g.fillStyle = '#000';
  q.rows.forEach((row, y) => {
    for (let x = 0; x < row.length; x++) if (row[x] === '1') g.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale);
  });
  return c;
}

function receiptCard(doc) {
  const meta = el('div', { class: 'receipt-meta' },
    el('b', {}, KIND_NAMES[doc.kind] || doc.kind),
    doc.number ? el('span', {}, `ФД ${doc.number}`) : null,
    el('span', {}, new Date(doc.time).toLocaleString('ru-RU')),
    doc.electronic ? el('span', { class: 'chip' }, 'только электронно') : null,
    doc.print_error ? el('span', { class: 'chip bad' }, 'сбой печати') : null,
  );
  return el('div', { class: 'receipt' }, meta, el('pre', {}, (doc.lines || []).join('\n')),
    doc.qr ? el('div', { class: 'receipt-qr' }, qrCanvas(doc.qr), el('div', { class: 'muted mono' }, doc.qr.data)) : null);
}

// Лента: какие документы показывать и сколько (листание чеков смены).
const TAPE_FILTERS = { all: null, checks: ['check', 'correction', 'copy'], reports: ['open_shift', 'close_shift', 'x_report', 'settlements', 'fn_report'] };

function renderTapeTools(loaded, shown, limit) {
  const box = $('#tape-meta');
  if (box.contains(document.activeElement)) return; // идёт выбор в списке
  const select = el('select', { title: 'Какие документы' },
    el('option', { value: 'all' }, 'все документы'), el('option', { value: 'checks' }, 'чеки'), el('option', { value: 'reports' }, 'отчёты'));
  select.value = state.tapeFilter || 'all';
  select.addEventListener('change', () => { state.tapeFilter = select.value; refreshTables(); });
  const more = el('button', { type: 'button', class: 'btn' }, 'Показать ещё');
  more.hidden = loaded < limit || limit >= 200; // сервер отдаёт до 200 последних
  more.addEventListener('click', () => { state.tapeLimit = Math.min(200, limit + 20); refreshTables(); });
  box.replaceChildren(select, el('span', {}, ` ${shown} из последних ${loaded}, новые сверху `), more);
}

async function loadDetail(info) {
  const box = $('#device-detail');
  box.hidden = false;
  $('#detail-title').textContent = info.alias || info.instance_name || info.device_id;
  $('#detail-sub').textContent = `${info.device_id} · ${info.name} · ${info.equipment_type}`;
  renderMeta(info);
  renderSettings(info);
  renderOutput(info);
  const st = info.state || {};
  const isKKT = st.model === 'kkt';
  const isAcq = st.model === 'acquiring';
  const isScales = st.model === 'scales';
  const isLabel = st.model === 'label_scales';
  const isReceipt = st.model === 'receipt_printer';
  const isLabelPrinter = st.model === 'label_printer';
  const isInput = st.model === 'scanner' || st.model === 'card_reader';
  const OTHER = { display: [displaySummary, displayActions, renderDisplayPanel], tsd: [tsdSummary, () => [], renderTsdPanel],
    rfid: [rfidSummary, () => [], renderRfidPanel], recognizer: [recognizerSummary, recognizerActions, renderRecognizerPanel] };
  const other = OTHER[st.model];
  $('#detail-summary').replaceChildren(...(isKKT ? kktSummary(st) : isAcq ? acqSummary(st) : isScales ? scalesSummary(st)
    : isLabel ? labelSummary(st) : isReceipt ? receiptSummary(st) : isLabelPrinter ? labelPrinterSummary(st)
      : isInput ? inputSummary(st, info) : other ? other[0](st, info) : summaryRow('Модель', st.model || '—')));
  $('#detail-actions').replaceChildren(...(isKKT ? kktActions(st) : isAcq ? acqActions(st) : isScales ? scalesActions(st)
    : isLabel ? labelActions(st) : isReceipt ? receiptActions(st) : isLabelPrinter ? printerToggles(st, 'Этикетки есть')
      : isInput ? inputActions(st) : other ? other[1](st) : [el('span', { class: 'muted' }, 'Для этой модели управление пока не реализовано (заглушка).')]));
  $('#detail-raw').textContent = JSON.stringify(info, null, 2);
  if (isAcq) renderScreen(st);
  else if (isScales) renderScaleScreen(st);
  else if (isInput) renderInputPanel(st, info);
  else if (other) other[2](st, info);
  else $('#detail-screen').hidden = true;
  if (isAcq) renderOps(st);
  else $('#detail-ops').hidden = true;
  if (isLabel) renderGoods(st);
  else $('#detail-goods').hidden = true;

  // Ленты нет у устройств, которые ничего не печатают.
  const printing = !['scanner', 'card_reader', 'scales', 'display', 'tsd', 'rfid', 'recognizer'].includes(st.model);
  $('#tape-head').hidden = !printing;
  $('#detail-tape').hidden = !printing;
  if (!printing) return;
  const limit = state.tapeLimit || 20;
  const { prints: loaded } = await (await api(`/control/v1/devices/${encodeURIComponent(info.device_id)}/prints?limit=${limit}`)).json();
  const kinds = TAPE_FILTERS[state.tapeFilter || 'all'];
  const prints = kinds ? loaded.filter((d) => kinds.includes(d.kind)) : loaded;
  renderTapeTools(loaded.length, prints.length, limit);
  let tape = prints.map((doc) => (doc.layout ? labelsCard(doc) : receiptCard(doc)));
  if (!tape.length && isAcq && st.last_slip && st.last_slip.length) {
    // Слип печатает ККТ (print_slip_on_terminal=false) — показываем последний, что отдал терминал.
    tape = [el('div', { class: 'receipt' }, el('div', { class: 'receipt-meta' }, el('b', {}, 'Последний слип'),
      el('span', {}, 'печатает ККТ')), el('pre', {}, st.last_slip.join('\n')))];
  }
  $('#detail-tape').replaceChildren(...(tape.length ? tape
    : [el('div', { class: 'empty' }, 'Устройство ещё ничего не печатало.')]));
}

// ---------- журнал ----------

function filterParams() {
  const p = new URLSearchParams();
  for (const [k, v] of new FormData($('#filters'))) if (v) p.set(k, String(v).trim());
  return p;
}

function resultCell(e) {
  const td = el('td');
  const p = e.payload || {};
  if (e.direction === 'out') {
    // result — возвращаемое значение метода: BOOL, LONG или null.
    if (p.result === true) td.append(el('span', { class: 'res-ok' }, '✓ True'));
    else if (p.result === false) {
      td.append(el('span', { class: 'res-fail', title: p.error_text || '' }, `✗ ${p.error_code ?? ''} ${short(p.error_text || '', 28)}`));
    } else if (p.result != null) td.append(el('span', { class: 'mono' }, '= ' + p.result));
    else td.append(el('span', { class: 'muted' }, '—'));
  } else if (e.direction === 'system' && p.rejected) {
    td.append(el('span', { class: 'res-fail', title: p.rejected }, '✗ отказ'));
  } else if (e.direction === 'system' && p.new_workstation) {
    td.append(el('span', { class: 'tag warn' }, 'новое место'));
  }
  if (e.direction === 'system' && p.auth === 'workstation') {
    td.append(el('span', { class: 'tag', title: p.workstation_requested ? 'Место назвал токен; в драйвере — ' + p.workstation_requested : 'Подключение по токену рабочего места' }, '🔑 токен места'));
  }
  if (e.replay) td.append(el('span', { class: 'tag', title: 'Повтор запроса с тем же request_id — отдан сохранённый ответ' }, 'повтор'));
  for (const r of e.applied_rules || []) td.append(el('span', { class: 'tag warn', title: 'Сработало правило сценария' }, r));
  return td;
}

function entryRow(e) {
  const tr = el('tr', { class: 'entry', 'data-seq': e.seq },
    el('td', { class: 'num muted' }, e.seq),
    el('td', { class: 'mono', title: e.ts }, fmtTime(e.ts)),
    el('td', {}, el('span', { class: 'dir dir-' + e.direction }, e.direction)),
    el('td', {}, e.workstation_id || ''),
    el('td', { class: 'mono' }, e.device_id || ''),
    el('td', { class: 'mono' }, e.method || ''),
    el('td', { class: 'mono muted', title: e.correlation_id || '' }, short(e.correlation_id)),
    el('td', { class: 'num' }, e.latency_ms ?? ''),
    resultCell(e),
  );
  tr.addEventListener('click', () => toggleDetails(tr, e));
  return tr;
}

function toggleDetails(tr, e) {
  const next = tr.nextElementSibling;
  if (next && next.classList.contains('details')) { next.remove(); return; }
  const pre = el('pre', {}, JSON.stringify(e, null, 2));
  tr.after(el('tr', { class: 'details' }, el('td', { colspan: 9 }, pre)));
}

function addEntries(entries, fresh) {
  if (!entries.length) return;
  const body = $('#journal-body');
  const wrap = $('.journal-wrap');
  const atTop = wrap.scrollTop < 8;
  const heightBefore = wrap.scrollHeight;
  const frag = document.createDocumentFragment();
  // Новые сверху.
  for (let i = entries.length - 1; i >= 0; i--) {
    const e = entries[i];
    const tr = entryRow(e);
    if (fresh) tr.classList.add('fresh');
    frag.append(tr);
    if (e.device_id) state.devices.add(e.device_id);
    if (e.workstation_id) state.workstations.add(e.workstation_id);
  }
  body.prepend(frag);
  const rows = body.querySelectorAll('tr.entry');
  for (let i = MAX_ROWS; i < rows.length; i++) {
    const r = rows[i];
    if (r.nextElementSibling && r.nextElementSibling.classList.contains('details')) r.nextElementSibling.remove();
    r.remove();
  }
  // Пользователь читает ниже — сохраняем его место, новые строки не сдвигают вид.
  if (!atTop) wrap.scrollTop += wrap.scrollHeight - heightBefore;
  $('#journal-empty').hidden = body.children.length > 0;
  updateJournalMeta();
}

function updateJournalMeta() {
  const n = $('#journal-body').querySelectorAll('tr.entry').length;
  let text = n ? `показано ${n}` : '';
  if (state.paused && state.pending.length) text += ` · на паузе, новых: ${state.pending.length}`;
  else if (state.paused) text += ' · на паузе';
  $('#journal-meta').textContent = text;
}

function onEntry(e, fresh) {
  state.lastSeq = Math.max(state.lastSeq, e.seq);
  if (state.paused) { state.pending.push(e); updateJournalMeta(); }
  else addEntries([e], fresh);
  if (e.direction !== 'event') scheduleRefresh();
}

// Поток SSE через fetch: так токен уходит заголовком, а не в URL.
async function streamJournal() {
  const gen = ++state.streamGen;
  let backoff = 500;
  while (gen === state.streamGen) {
    const ctrl = new AbortController();
    state.streamAbort = ctrl;
    const params = filterParams();
    if (state.lastSeq) params.set('after_seq', state.lastSeq);
    else params.set('tail', TAIL);
    let ready = false;
    let batch = [];
    try {
      const res = await api('/control/v1/journal/stream?' + params, { signal: ctrl.signal });
      setLive(true);
      backoff = 500;
      const reader = res.body.pipeThrough(new TextDecoderStream()).getReader();
      let buf = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += value;
        let i;
        while ((i = buf.indexOf('\n\n')) >= 0) {
          const block = buf.slice(0, i);
          buf = buf.slice(i + 2);
          let event = 'message', data = '';
          for (const line of block.split('\n')) {
            if (line.startsWith('event: ')) event = line.slice(7);
            else if (line.startsWith('data: ')) data += line.slice(6);
          }
          if (event === 'entry') {
            const e = JSON.parse(data);
            if (ready) onEntry(e, true);
            else { batch.push(e); state.lastSeq = Math.max(state.lastSeq, e.seq); }
          } else if (event === 'ready') {
            ready = true;
            if (state.paused) state.pending.push(...batch); else addEntries(batch, false);
            batch = [];
            $('#journal-empty').hidden = $('#journal-body').children.length > 0;
            updateJournalMeta();
          }
          // lagged: сервер закроет поток, переподключимся с after_seq и дочитаем.
        }
      }
    } catch (e) {
      if (e instanceof AuthError || e.name === 'AbortError') return;
      console.warn('поток журнала:', e);
    }
    setLive(false);
    if (gen !== state.streamGen) return;
    await new Promise((r) => setTimeout(r, backoff));
    backoff = Math.min(backoff * 2, 10000);
  }
}

function restartJournal() {
  state.streamGen++;
  if (state.streamAbort) state.streamAbort.abort();
  state.lastSeq = 0;
  state.pending = [];
  $('#journal-body').replaceChildren();
  streamJournal();
}

let filterTimer = null;
$('#filters').addEventListener('input', () => {
  clearTimeout(filterTimer);
  filterTimer = setTimeout(restartJournal, 350);
});
$('#filters').addEventListener('submit', (ev) => ev.preventDefault());

$('#pause').addEventListener('click', () => {
  state.paused = !state.paused;
  $('#pause').textContent = state.paused ? 'Продолжить' : 'Пауза';
  $('#pause').classList.toggle('active', state.paused);
  if (!state.paused && state.pending.length) {
    addEntries(state.pending, true);
    state.pending = [];
  }
  updateJournalMeta();
});

$('#export').addEventListener('click', async () => {
  try {
    const params = filterParams();
    params.set('format', 'jsonl');
    const res = await api('/control/v1/journal?' + params);
    const blob = await res.blob();
    const cd = res.headers.get('Content-Disposition') || '';
    const name = (cd.match(/filename="([^"]+)"/) || [])[1] || 'emulkon-journal.jsonl';
    const a = el('a', { href: URL.createObjectURL(blob), download: name });
    document.body.append(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  } catch (e) {
    if (!(e instanceof AuthError)) alert('Выгрузка не удалась: ' + e.message);
  }
});

// ---------- сброс ----------

$('#reset').addEventListener('click', async () => {
  if (!confirm('Сбросить состояние всех виртуальных устройств? Сессии компонент сохранятся.')) return;
  try {
    const r = await (await api('/control/v1/reset', { method: 'POST' })).json();
    refreshTables();
    $('#reset').textContent = `Сброшено: ${r.reset_devices}`;
    setTimeout(() => { $('#reset').textContent = 'Сбросить состояние'; }, 2000);
  } catch (e) {
    if (!(e instanceof AuthError)) alert('Сброс не удался: ' + e.message);
  }
});

// ---------- старт ----------

async function start() {
  try {
    await loadInfo();
  } catch (e) {
    if (!(e instanceof AuthError)) $('#warnings').replaceChildren(el('div', { class: 'warning' }, 'Сервер недоступен: ' + e.message));
    return;
  }
  refreshTables();
  restartJournal();
}

setInterval(() => { if (!document.hidden) refreshTables(); }, 5000);

let initialView = location.hash.slice(1);
if (!initialView) {
  try { initialView = localStorage.getItem('emulkon.view') || 'devices'; } catch { initialView = 'devices'; }
}
switchView(['devices', 'sessions', 'classes', 'scenarios', 'journal'].includes(initialView) ? initialView : 'devices');
start();
