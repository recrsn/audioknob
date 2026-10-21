"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const html = fs.readFileSync(path.join(__dirname, "../../firmware/s3/src/device/net/web/index.html"), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script); // Check the entire embedded page's syntax.
const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));
const elements = {};
const context = {
  uploading: false, installing: null, wentOffline: false,
  $: id => {
    assert(ids.has(id), `missing DOM element ${id}`);
    return elements[id] ||= {};
  },
  setText: (id, value) => { context.$(id).textContent = value; },
  setBar: value => { context.bar = value; },
  toast: value => { context.message = value; },
  UPDATE_HINT: "Choose a firmware.bin",
};
vm.createContext(context);
vm.runInContext(script.slice(script.indexOf("function when("), script.indexOf("async function poll(")), context);
const state = {
  version: "1.0.0",
  release: {checking: false, checked: true, lastCheck: 1780000000, nextCheck: 0, s3: true, bluetooth: true, tag: "v1.2.3", error: ""},
  update: {state: "idle", target: "s3", progress: 0, version: "", error: ""},
};
state.release.s3Version = "v1.2.3";
state.release.bluetoothVersion = "v1.0.0";
context.renderUpdate(state);
assert.match(elements.releasehint.textContent, /Update available: Main v1.2.3/);
assert.equal(elements.releases3.hidden, false);
assert.equal(elements.releasebt.textContent, "Download Bluetooth v1.0.0");
assert.equal(elements.releasebt.hidden, false);
assert.equal(elements.updbtn.disabled, false);
state.update.state = "ready";
state.update.version = "1.2.3";
context.renderUpdate(state);
for (const id of ["checkupd", "releases3", "releasebt", "updbtn"]) assert.equal(elements[id].disabled, true);
assert.match(elements.updhint.textContent, /confirm on the device/);
state.update.state = "receiving";
state.update.progress = 42;
context.renderUpdate(state);
assert.equal(context.bar, 42);
state.update.state = "idle";
state.release.checking = true;
context.renderUpdate(state);
assert.match(elements.releasehint.textContent, /Checking GitHub/);
assert.equal(elements.checkupd.disabled, true);
state.release = {checking: false, checked: true, lastCheck: 1780000000, nextCheck: 0, s3: false, bluetooth: false, tag: "v1.0.0", error: ""};
context.renderUpdate(state);
assert.match(elements.releasehint.textContent, /up to date/i);
assert.equal(elements.releases3.hidden, true);
state.release.error = "GitHub rate limit; try again later";
context.renderUpdate(state);
assert.match(elements.releasehint.textContent, /GitHub rate limit; try again later/);
context.uploading = true;
context.renderUpdate(state);
assert.equal(elements.checkupd.disabled, true);
assert.equal(elements.updbtn.disabled, false); // Upload handler owns this button while uploading.
state.release = {checking: false, checked: true, s3: false, bluetooth: false, lastCheck: 1780000000, nextCheck: 0};
context.uploading = false;
context.renderUpdate(state);
assert.match(elements.releasehint.textContent, /Last checked/);
assert.match(elements.releasehint.textContent, /Choose Check Now/);
assert.doesNotMatch(elements.releasehint.textContent, /1970|next check/);
console.log("Web update availability, progress, errors, timestamps and action locking passed");
