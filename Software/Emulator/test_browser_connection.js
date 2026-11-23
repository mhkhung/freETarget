// Evaluate the actual URL function, without requiring a browser or DOM.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const vm = require('node:vm');

function resolve(source, pageUrl) {
  const page = new URL(pageUrl);
  const match = source.match(/^function wsUrl\(\) \{[\s\S]*?^\}/m);
  assert.ok(match, 'The athlete page must contain its WebSocket URL function');
  return vm.runInNewContext(match[0] + '\nwsUrl()', {
    location: page,
    URLSearchParams,
  }, {timeout: 1000});
}

function check(source) {
  for (const [pageUrl, expected] of [
    ['http://127.0.0.1:18000/', 'ws://127.0.0.1:18080/ws'],
    ['http://localhost:18000/', 'ws://localhost:18080/ws'],
    ['http://127.0.0.1:18100/', 'ws://127.0.0.1:18180/ws'],
    ['http://127.0.0.1:18000/?other=1', 'ws://127.0.0.1:18080/ws'],
    ['http://127.0.0.1:18000/?ws=localhost%3A19080', 'ws://localhost:19080/ws'],
  ]) {
    assert.equal(resolve(source, pageUrl), expected, pageUrl);
  }
}

if (process.argv[2]) {
  const pageUrl = new URL(process.argv[2]);
  assert.equal(pageUrl.hostname, '127.0.0.1');
  const request = http.get(pageUrl, response => {
    assert.equal(response.statusCode, 200);
    let source = '';
    response.setEncoding('utf8');
    response.on('data', chunk => { source += chunk; });
    response.on('end', () => {
      check(source);
      process.stdout.write(JSON.stringify({url: resolve(source, pageUrl)}));
    });
  });
  request.setTimeout(5000, () => request.destroy(new Error('athlete page timed out')));
  request.on('error', error => { console.error(error.message); process.exitCode = 1; });
} else {
  check(fs.readFileSync(__dirname + '/firmware/browser_connection.js', 'utf8'));
  console.log('PASS: athlete page chooses the forwarded WebSocket port without a URL override');
}
