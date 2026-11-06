// The setup page served at /setup: first admin PIN, then the gym's Wi-Fi. Small and
// self-contained on purpose: it must work on a phone joined to the wall's access point,
// with no internet, and it talks only to the same API the console uses.
namespace wall {
namespace net {

extern const char kSetupPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wall setup</title>
<style>
body{font:16px system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:28rem}
h1{font-size:1.3rem}label{display:block;margin:.8rem 0 .2rem}
input{width:100%;box-sizing:border-box;padding:.6rem;font:inherit;border-radius:6px;border:1px solid #555;background:#222;color:#eee}
button{margin-top:1rem;padding:.7rem 1.2rem;font:inherit;border:0;border-radius:6px;background:#2a7;color:#fff}
.err{color:#f77;min-height:1.2em}.ok{color:#7d7}section{margin-bottom:2rem}
</style></head><body>
<h1>Wall setup</h1>
<section id="pin"><h2>1. Admin PIN</h2>
<p id="pin-state">Checking...</p>
<form id="pin-form" hidden>
<label for="p1">New PIN (6 to 12 digits)</label><input id="p1" inputmode="numeric" autocomplete="new-password">
<label for="p2">Same PIN again</label><input id="p2" inputmode="numeric" autocomplete="new-password">
<button>Set PIN</button><p class="err" id="pin-err"></p></form></section>
<section><h2>2. The gym's Wi-Fi</h2>
<form id="wifi-form">
<label for="ssid">Network name</label><input id="ssid" autocapitalize="off" autocomplete="off">
<label for="pw">Password</label><input id="pw" type="password" autocomplete="off">
<label for="pin2">Admin PIN</label><input id="pin2" inputmode="numeric" type="password">
<button>Save and restart</button><p class="err" id="wifi-err"></p></form></section>
<script>
const $ = (s) => document.querySelector(s);
async function call(method, path, body, pin) {
  const h = {'Content-Type': 'application/json'};
  if (pin) h['X-Admin-Pin'] = pin;
  const r = await fetch(path, {method, headers: h, body: body && JSON.stringify(body)});
  let d = null; try { d = await r.json(); } catch (e) {}
  if (!r.ok) throw new Error((d && d.error) || ('The wall answered ' + r.status));
  return d;
}
async function load() {
  const s = await call('GET', '/api/settings');
  $('#pin-state').textContent = s.pin_set ? 'A PIN is set. Use it below.' : 'No PIN yet. Set one first.';
  $('#pin-form').hidden = s.pin_set;
}
$('#pin-form').onsubmit = async (e) => {
  e.preventDefault(); $('#pin-err').textContent = '';
  const p = $('#p1').value.trim();
  if (!/^[0-9]{6,12}$/.test(p)) return $('#pin-err').textContent = 'The PIN must be 6 to 12 digits.';
  if (p !== $('#p2').value.trim()) return $('#pin-err').textContent = 'The two PINs are different.';
  try { await call('PUT', '/api/settings', {new_pin: p}); $('#pin2').value = p; await load(); }
  catch (err) { $('#pin-err').textContent = err.message; }
};
$('#wifi-form').onsubmit = async (e) => {
  e.preventDefault(); $('#wifi-err').textContent = '';
  try {
    await call('PUT', '/api/wifi', {ssid: $('#ssid').value, password: $('#pw').value}, $('#pin2').value.trim());
    $('#wifi-err').className = 'ok';
    $('#wifi-err').textContent = 'Saved. The wall restarts and joins the network. Reconnect your phone to it, then open http://gymboard.local';
  } catch (err) { $('#wifi-err').className = 'err'; $('#wifi-err').textContent = err.message; }
};
load().catch((err) => { $('#pin-state').textContent = err.message; });
</script></body></html>
)HTML";

}  // namespace net
}  // namespace wall
