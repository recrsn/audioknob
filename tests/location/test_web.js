"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const html = fs.readFileSync(path.join(__dirname, "../../firmware/s3/src/device/net/web/index.html"), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
const elements = {}, requests = [];
const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));
const context = {
  locManual: false, state: {locationAuto: false}, render: () => {},
  $: id => { assert(ids.has(id)); return elements[id] ||= {}; },
  setText: (id, text) => { context.$(id).textContent = text; },
  post: async (url, fields) => { requests.push({url, fields: {...fields}}); return context.ok; },
  toast: text => { context.message = text; }, ok: true,
};
vm.createContext(context);
vm.runInContext(script.slice(script.indexOf("function renderLocation("), script.indexOf("async function poll(")), context);
vm.runInContext(script.slice(script.indexOf('let locManual = false;'), script.indexOf("async function addStation(")), context);
(async () => {
  context.renderLocation({locationAuto: true, location: "London"});
  assert.equal(elements.loc.textContent, "London");
  assert.equal(elements.locAuto.checked, true);
  assert.equal(elements.locmanual.hidden, true);
  assert.equal(elements.locdetect.hidden, false);
  await elements.locdetect.onclick();
  assert.equal(requests[0].url, "/api/location");
  assert.deepEqual(requests[0].fields, {detect: 1});
  assert.match(context.message, /requested/);
  context.message = ""; context.ok = false;
  await elements.locdetect.onclick(); assert.equal(context.message, "");
  context.renderLocation({locationAuto: false, location: "Paris"});
  assert.equal(elements.loc.textContent, "Paris");
  assert.equal(elements.locAuto.checked, false); assert.equal(elements.locdetect.hidden, true);
  await elements.locAuto.onchange({target: {checked: true}}); assert.deepEqual(requests[2].fields, {auto: 1});
  context.renderLocation({locationAuto: true, location: ""});
  assert.equal(elements.loc.textContent, "Detecting…");
  console.log("Web location controls preserve auto reuse and explicit detect actions");
})().catch(error => { console.error(error); process.exitCode = 1; });
