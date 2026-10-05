// Matrix rain background (shares this file to save a request)
(() => {
  if (typeof matchMedia !== 'function' || matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  const canvas = document.createElement('canvas');
  canvas.setAttribute('aria-hidden', 'true');
  canvas.style.cssText = 'position:fixed;left:0;top:0;width:100%;height:100%;z-index:-1;pointer-events:none;opacity:.6';
  document.body.prepend(canvas);
  const ctx = canvas.getContext('2d');
  const glyphs = Array.from('ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ0123456789:.=*+-<>|ｦ');
  const pick = () => glyphs[(Math.random() * glyphs.length) | 0];
  // Three depth layers: small/slow/dim at the back, large/fast/bright in front.
  const layers = [
    {size: 12, speed: [5, 11], alpha: .38, glow: 0},
    {size: 17, speed: [9, 18], alpha: .65, glow: 0},
    {size: 24, speed: [14, 28], alpha: 1, glow: 10}
  ];
  let width = 0, height = 0, dpr = 1, columns = [];
  const reset = (c, initial) => {
    const l = c.layer;
    c.rows = Math.ceil(height / l.size) + 2;
    c.length = 8 + ((Math.random() * 22) | 0);
    c.speed = l.speed[0] + Math.random() * (l.speed[1] - l.speed[0]);
    c.y = initial ? -Math.random() * (c.rows + c.length) : -((Math.random() * 24) | 0);
    c.cells = Array.from({length: c.rows}, pick);
  };
  const build = () => {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    width = innerWidth; height = innerHeight;
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
    columns = [];
    layers.forEach((layer, index) => {
      const step = layer.size * .92, count = Math.ceil(width / step);
      const density = [.55, .6, .45][index];
      for (let i = 0; i < count; i++) {
        if (Math.random() > density) continue;
        const c = {layer, x: i * step + (Math.random() - .5) * 2};
        reset(c, true); columns.push(c);
      }
    });
  };
  let last = 0;
  const frame = now => {
    requestAnimationFrame(frame);
    if (now - last < 33) return; // ~30 fps keeps the pager's page light on phones
    const dt = Math.min((now - last) / 1000, .1); last = now;
    // Mirrored, like the film's reversed katakana.
    ctx.setTransform(-dpr, 0, 0, dpr, width * dpr, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    let font = '';
    for (const c of columns) {
      const l = c.layer;
      const before = Math.floor(c.y);
      c.y += c.speed * dt;
      const head = Math.floor(c.y);
      for (let r = before + 1; r <= head; r++) if (r >= 0 && r < c.rows) c.cells[r] = pick();
      if (Math.random() < .08) { // glyphs flicker as the stream falls
        const r = head - ((Math.random() * c.length) | 0);
        if (r >= 0 && r < c.rows) c.cells[r] = pick();
      }
      if (head - c.length > c.rows) { reset(c, false); continue; }
      const f = l.size + 'px "MS Gothic","Noto Sans Mono CJK JP",monospace';
      if (f !== font) { ctx.font = f; font = f; }
      for (let k = 0; k < c.length; k++) {
        const r = head - k;
        if (r < 0 || r >= c.rows) continue;
        const fade = Math.pow(1 - k / c.length, 1.6);
        if (k === 0) {
          ctx.shadowColor = '#7dffa8'; ctx.shadowBlur = l.glow;
          ctx.fillStyle = 'rgba(225,255,235,' + l.alpha + ')';
        } else {
          ctx.shadowBlur = 0;
          ctx.fillStyle = k === 1 ? 'rgba(150,255,175,' + l.alpha * .9 + ')' : 'rgba(0,' + (150 + 105 * fade | 0) + ',70,' + (fade * l.alpha).toFixed(3) + ')';
        }
        ctx.fillText(c.cells[r], c.x + l.size / 2, r * l.size);
      }
    }
    ctx.shadowBlur = 0;
  };
  let timer;
  addEventListener('resize', () => { clearTimeout(timer); timer = setTimeout(build, 200); });
  build();
  requestAnimationFrame(frame);
})();
const $ = id => document.getElementById(id);
const text = $('text');
const contactChoice = $('contactChoice'), recipient = $('recipient');
if (contactChoice && recipient) {
  contactChoice.addEventListener('change', () => { recipient.value = contactChoice.value; });
  recipient.addEventListener('input', () => { contactChoice.value = ''; });
}
if (text) text.addEventListener('input', () => {
  const bytes = new TextEncoder().encode(text.value).length;
  $('count').textContent = `${bytes} / 512 bytes`;
  text.setCustomValidity(bytes > 512 ? 'Use at most 512 UTF-8 bytes.' : '');
});
function element(tag, value, cls) {
  const el = document.createElement(tag);
  if (value !== undefined) el.textContent = value;
  if (cls) el.className = cls;
  return el;
}
let inboxMessages = [], cursor = 0;
let renderedCard;
let anchor = new URLSearchParams(location.search).get('anchor') || '';
function renderInbox() {
  inboxMessages = inboxMessages.filter(m => !m.expiresAt || Date.now() / 1000 < m.expiresAt);
  const anchored = inboxMessages.findIndex(m => m.id === anchor);
  cursor = anchored >= 0 ? anchored : Math.min(cursor, Math.max(0, inboxMessages.length - 1));
  const m = inboxMessages[cursor];
  anchor = m ? m.id : '';
  $('previous').disabled = !m || cursor === 0;
  $('next').disabled = !m || cursor + 1 >= inboxMessages.length;
  $('position').textContent = `${m ? cursor + 1 : 0} / ${inboxMessages.length}`;
  $('unread').textContent = `${inboxMessages.filter(m => !m.read && !m.outgoing).length} unread`;
  const cardState = JSON.stringify(m || null);
  if (cardState === renderedCard) return;
  renderedCard = cardState;
  if (!m) {
    $('messages').replaceChildren(element('section', 'Your quiet corner. Waiting for a message.', 'empty'));
    return;
  }
  const card = element('a', undefined, `card${!m.read && !m.outgoing ? ' fresh' : ''}`);
  card.href = '/message?id=' + encodeURIComponent(m.id);
  const meta = element('div', undefined, 'meta');
  meta.append(element('span', `${m.outgoing ? '↑ To' : '✉ From'} ${m.peerName || m.peer.slice(0,10)}`), element('span', m.kind !== 1 ? 'DM' : 'NOTE', `badge${m.kind !== 1 ? '' : ' public'}`));
  card.append(meta, element('p', m.text, 'preview'), element('small', `${new Date(m.timestamp * 1000).toLocaleString()}${m.outgoing ? ' · ' + m.delivery : !m.read ? ' · New message' : ''}`));
  $('messages').replaceChildren(card);
}
if ($('previous')) $('previous').addEventListener('click', () => {
  if (cursor === 0 || !inboxMessages.length) return;
  anchor = inboxMessages[--cursor].id; renderInbox();
});
if ($('next')) $('next').addEventListener('click', () => {
  if (cursor + 1 >= inboxMessages.length) return;
  anchor = inboxMessages[++cursor].id; renderInbox();
});
let refreshPending = null, stateTag = '', lastState = null;
function refresh() {
  if ((!$('messages') && !$('connection-required')) || document.hidden) return Promise.resolve();
  if (refreshPending) return refreshPending;
  refreshPending = fetchState().finally(() => { refreshPending = null; });
  return refreshPending;
}
async function fetchState() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 10000);
  try {
    const response = await fetch('/api/state', {cache:'no-store', signal:controller.signal, headers: stateTag && lastState ? {'If-None-Match': stateTag} : {}});
    if (response.status === 401) { location.replace('/login'); return; }
    let state;
    if (response.status === 304 && lastState) state = lastState; // unchanged: nothing was sent again
    else {
      if (!response.ok) throw new Error('Device busy');
      state = await response.json();
      lastState = state; stateTag = response.headers?.get?.('ETag') || '';
    }
    const offline = state.wifiConnected === false || state.configured === false;
    const prompt = $('connection-required'), content = $('connected-content');
    if (prompt) {
      prompt.hidden = !offline;
      if (content) content.hidden = offline;
      else if (!offline) { location.replace('/messages'); return; }
      if ($('connection-hint')) $('connection-hint').textContent = state.wifiConnected === false
        ? 'Connect the pager to Wi-Fi. Open configuration to choose a network and check your identity.'
        : 'Configure your Nostr identity to start using messages.';
    }
    if (offline) {
      inboxMessages = []; renderedCard = undefined;
      if ($('messages')) $('messages').replaceChildren();
      return;
    }
    if (!$('messages')) return; // Preserve form fields while checking connectivity.
    const age = Number(response.headers?.get('X-Pager-Snapshot-Age-Ms') || 0);
    $('status').textContent = state.status + (age > 5000 ? ' · Showing cached data while messaging is busy.' : '');
    inboxMessages = state.messages;
    renderInbox();
  } catch (error) {
    if ($('status')) $('status').textContent = 'Connection interrupted. Retrying…';
    if ($('connection-required')) $('connection-required').hidden = false;
    if ($('connected-content')) $('connected-content').hidden = true;
    if ($('connection-hint')) $('connection-hint').textContent = 'Cannot reach the pager. Check its Wi-Fi connection or open Setup on the display.';
  }
  finally { clearTimeout(timeout); }
}
refresh();
setInterval(refresh, 5000);
setInterval(() => {
  if ($('messages') && !document.hidden && inboxMessages.some(m => m.expiresAt && Date.now() / 1000 >= m.expiresAt)) renderInbox();
}, 1000);
const expiry = $('messageExpiry');
if (expiry) {
  const checkExpiry = () => {
    if (Date.now() / 1000 >= Number(expiry.dataset.expires)) {
      $('messageContent').replaceChildren(element('p', 'Message expired.'));
      location.replace('/messages');
    }
  };
  checkExpiry();
  setInterval(checkExpiry, 1000);
}
document.addEventListener('visibilitychange', refresh);
