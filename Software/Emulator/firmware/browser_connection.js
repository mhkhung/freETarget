// The emulator forwards HTTP to 18000 and WebSockets to 18080. Smoke tests
// shift both ports by the same offset, so derive the endpoint from this page.
function wsUrl() {
  const forced = new URLSearchParams(location.search).get("ws");
  const endpoint = location.hostname + ":" + (Number(location.port) + 80);
  return "ws://" + (forced || endpoint) + "/ws";
}
