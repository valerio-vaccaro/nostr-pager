const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');
class Element {
  constructor() { this.children = []; this.events = {}; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
  addEventListener(name, cb) { this.events[name] = cb; }
  setCustomValidity(value) { this.validity = value; }
}
const elements = Object.fromEntries(['text', 'count', 'messages', 'unread', 'status', 'contactChoice', 'recipient', 'previous', 'next', 'position'].map(id => [id, new Element()]));
const hostile = '<img src=x onerror=alert(1)>';
const first = {id:'a?b&c', peer:'0123456789abc', peerName:hostile, text:hostile,
  kind:14, outgoing:false, read:false, timestamp:123, delivery:''};
let messages = [first];
let status = 200, redirect, hidden = false, fetches = 0, snapshotAge = 0;
let browserTime=Date.now();
class BrowserDate extends Date {static now() {return browserTime;}}
const timers=[];
const sandbox = {
  document: {
    get hidden() { return hidden; },
    getElementById(id) { return elements[id]; },
    createElement() { return new Element(); },
    addEventListener() {}
  },
  location: {replace(url) { redirect = url; }},
  TextEncoder, AbortController, setTimeout, clearTimeout, Date: BrowserDate, URLSearchParams, setInterval(fn,delay) {timers.push({fn,delay});},
  async fetch() {
    fetches++;
    return {status, ok: status === 200, headers: {get() {return String(snapshotAge);}}, async json() {
      return {status:'Online', messages};
    }};
  }
};
(async () => {
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.resolve(__dirname,'../../assets/web/app.js'),'utf8'), sandbox);
  await vm.runInContext('refresh()', sandbox);
  const card = elements.messages.children[0];
  assert.strictEqual(card.href, '/message?id=a%3Fb%26c');
  assert.strictEqual(card.children[1].textContent, hostile);
  assert.strictEqual(card.children[1].children.length, 0);
  assert.strictEqual(elements.unread.textContent, '1 unread');
  assert.strictEqual(card.children[0].children[1].textContent,'DM');
  assert.strictEqual(card.children[0].children[0].textContent,'✉ From '+hostile);
  assert.strictEqual(card.children[0].children[0].children.length,0);
  assert(elements.previous.disabled && elements.next.disabled);
  timers.find(timer=>timer.delay===1000).fn();
  assert.strictEqual(elements.messages.children[0],card);
  await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.messages.children[0],card); // No DOM replacement for unchanged data.
  const originalFetch=sandbox.fetch;
  let finishFetch;
  sandbox.fetch=()=>{fetches++;return new Promise(resolve=>{finishFetch=resolve;});};
  const slow=vm.runInContext('refresh()', sandbox), beforeOverlap=fetches;
  const overlap=vm.runInContext('refresh()', sandbox);
  assert.strictEqual(slow,overlap);
  assert.strictEqual(fetches,beforeOverlap);
  finishFetch(await originalFetch());await slow;
  sandbox.fetch=originalFetch;
  let abortTimeout;
  sandbox.setTimeout=fn=>{abortTimeout=fn;return 0;};
  sandbox.clearTimeout=()=>{};
  sandbox.fetch=(url,options)=>new Promise((resolve,reject)=>{
    options.signal.addEventListener('abort',()=>reject(new Error('timeout')));
  });
  const stalled=vm.runInContext('refresh()', sandbox);
  abortTimeout();await stalled;
  assert(elements.status.textContent.includes('Retrying'));
  sandbox.fetch=originalFetch;sandbox.setTimeout=setTimeout;sandbox.clearTimeout=clearTimeout;
  await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.status.textContent,'Online');
  snapshotAge=12000;await vm.runInContext('refresh()', sandbox);
  assert(elements.status.textContent.includes('Showing cached data'));
  snapshotAge=0;await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.status.textContent,'Online');
  const second={...first,id:'second',text:'Second message'},third={...first,id:'third',text:'Third message'};
  messages=[first,second,third];await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.messages.children.length,1);
  assert.strictEqual(elements.position.textContent,'1 / 3');
  let beforeNavigation=fetches;
  elements.previous.events.click();assert.strictEqual(elements.position.textContent,'1 / 3');
  elements.next.events.click();assert.strictEqual(elements.messages.children[0].children[1].textContent,'Second message');
  messages=[{...first,id:'new',text:'New arrival'},...messages];await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.position.textContent,'3 / 4');
  assert.strictEqual(elements.messages.children[0].children[1].textContent,'Second message');
  beforeNavigation=fetches;
  elements.next.events.click();elements.next.events.click();
  assert.strictEqual(elements.position.textContent,'4 / 4');assert(elements.next.disabled);
  assert.strictEqual(fetches,beforeNavigation);
  messages.pop();await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.position.textContent,'3 / 3');
  messages=[];await vm.runInContext('refresh()', sandbox);
  assert.strictEqual(elements.position.textContent,'0 / 0');assert(elements.previous.disabled && elements.next.disabled);
  messages=[{...first,id:'expiring',expiresAt:Math.floor(browserTime/1000)+2}];
  await vm.runInContext('refresh()', sandbox);assert.strictEqual(elements.position.textContent,'1 / 1');
  browserTime+=3000;timers.find(timer=>timer.delay===1000).fn();
  assert.strictEqual(elements.position.textContent,'0 / 0');
  assert.strictEqual(elements.unread.textContent,'0 unread');
  elements.contactChoice.value='pubkey-a';elements.contactChoice.events.change();assert.strictEqual(elements.recipient.value,'pubkey-a');
  elements.recipient.value='Alice';elements.recipient.events.input();assert.strictEqual(elements.contactChoice.value,'');
  elements.text.value = '💜'.repeat(129);
  elements.text.events.input();
  assert.strictEqual(elements.count.textContent, '516 / 512 bytes');
  assert(elements.text.validity);
  elements.text.value = 'Hello';elements.text.events.input();assert.strictEqual(elements.text.validity,'');
  status = 401;await vm.runInContext('refresh()', sandbox);assert.strictEqual(redirect,'/login');
  status = 503;await vm.runInContext('refresh()', sandbox);assert(elements.status.textContent.includes('Retrying'));
  hidden=true;const before=fetches;await vm.runInContext('refresh()', sandbox);assert.strictEqual(fetches,before);
  const content=new Element(),expiry=new Element();expiry.dataset={expires:String(Math.floor(browserTime/1000)+1)};
  const readerTimers=[];let readerRedirect;
  const reader={document:{getElementById(id) {return id==='messageExpiry'?expiry:id==='messageContent'?content:undefined;},
    createElement() {return new Element();},addEventListener() {}},TextEncoder,Date:BrowserDate,URLSearchParams,
    location:{replace(url) {readerRedirect=url;}},setInterval(fn,delay) {readerTimers.push({fn,delay});}};
  vm.createContext(reader);vm.runInContext(fs.readFileSync(path.resolve(__dirname,'../../assets/web/app.js'),'utf8'),reader);
  assert.strictEqual(readerRedirect,undefined);browserTime+=2000;
  for(const timer of readerTimers.filter(timer=>timer.delay===1000))timer.fn();
  assert.strictEqual(readerRedirect,'/messages');assert.strictEqual(content.children[0].textContent,'Message expired.');
  console.log('Browser button navigation, boundaries, new-arrival anchoring, cache eviction, escaped rendering, UTF-8 limits and expiry/offline/hidden tabs passed.');
})().catch(error => { console.error(error);process.exit(1); });
