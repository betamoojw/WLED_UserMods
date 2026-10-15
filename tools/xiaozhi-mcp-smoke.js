// Hardware smoke test for Xiaozhi_MCP. Uses only Node.js standard libraries.
// RFC 6455: https://www.rfc-editor.org/rfc/rfc6455
// This server only invokes read-only tools; it never sends LED state changes.
const net = require('node:net');
const { createHash } = require('node:crypto');
const assert = require('node:assert/strict');

const port = Number(process.env.MCP_TEST_PORT || 8765);
const pending = new Map();
let nextId = 1;
let active = false;
const cycles = Number(process.env.MCP_TEST_CYCLES || 1);
assert(Number.isInteger(cycles) && cycles >= 1 && cycles <= 20, 'MCP_TEST_CYCLES must be 1..20');
let completed = 0, reconnectTimer = null, disconnectedAt = 0;

function frame(opcode, data, final = true) {
	const payload = Buffer.from(data);
	const header = Buffer.alloc(payload.length < 126 ? 2 : 4);
	header[0] = (final ? 0x80 : 0) | opcode;
	header[1] = payload.length < 126 ? payload.length : 126;
	if (payload.length >= 126) header.writeUInt16BE(payload.length, 2);
	return Buffer.concat([header, payload]);
}

// Every expected response has a deadline, including pong control frames.
function expect(key) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			pending.delete(key);
			reject(new Error(`Timed out waiting for ${key}`));
		}, 15000);
		pending.set(key, value => { clearTimeout(timer); resolve(value); });
	});
}

function deliver(key, value) {
	const resolve = pending.get(key);
	if (resolve) { pending.delete(key); resolve(value); }
}

async function exercise(socket, accept) {
	const id = nextId++;
	const initialized = expect(id);
	// Coalesce the upgrade response and first RPC to catch handshake read-ahead loss.
	socket.write(Buffer.concat([
		Buffer.from(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`),
		frame(1, JSON.stringify({ jsonrpc: '2.0', id, method: 'initialize', params: {} }))
	]));
	assert.equal((await initialized).result.protocolVersion, '2024-11-05');
	console.log('PASS initialize immediately after upgrade');
	async function rpc(method, params = {}) {
		const id = nextId++;
		const response = expect(id);
		socket.write(frame(1, JSON.stringify({ jsonrpc: '2.0', id, method, params })));
		return response;
	}
	const list = await rpc('tools/list');
	const tools = list.result.tools;
	for (const prefix of ['led_status_', 'led_get_alias_', 'led_power_', 'led_brightness_', 'led_color_', 'led_effect_']) {
		assert(tools.some(tool => tool.name.startsWith(prefix)), `Missing ${prefix}`);
	}
	console.log('PASS all six tools listed');
	for (const prefix of ['led_status_', 'led_get_alias_']) {
		const name = tools.find(tool => tool.name.startsWith(prefix)).name;
		const response = await rpc('tools/call', { name, arguments: {} });
		assert.equal(response.result.isError, false);
		assert.equal(response.result.content[0].type, 'text');
		assert.equal(typeof JSON.parse(response.result.content[0].text), 'object');
	}
	console.log('PASS read-only status and alias calls');
	const fragmentedId = nextId++;
	const fragmented = expect(fragmentedId);
	const pong = expect('pong');
	const message = JSON.stringify({ jsonrpc: '2.0', id: fragmentedId, method: 'ping' });
	// A control frame may appear between fragments of one text message.
	socket.write(frame(1, message.slice(0, 9), false));
	socket.write(frame(9, 'probe'));
	setTimeout(() => socket.write(frame(0, message.slice(9))), 25);
	assert.equal((await pong).toString(), 'probe');
	assert.deepEqual((await fragmented).result, {});
	console.log('PASS fragmented RPC and interleaved WebSocket ping/pong');
	const splitId = nextId++;
	const splitResponse = expect(splitId);
	const splitFrame = frame(1, JSON.stringify({ jsonrpc: '2.0', id: splitId, method: 'ping', params: { padding: 'x'.repeat(256) } }));
	// Split both the base header and extended length across delayed TCP writes.
	socket.write(splitFrame.subarray(0, 1));
	setTimeout(() => socket.write(splitFrame.subarray(1, 3)), 250);
	setTimeout(() => socket.write(splitFrame.subarray(3)), 500);
	assert.deepEqual((await splitResponse).result, {});
	console.log('PASS delayed TCP header splits and extended payload length');

	completed++;
	if (completed < cycles) {
		console.log(`PASS cycle ${completed}; dropping TCP to check automatic reconnect`);
		disconnectedAt = Date.now();
		socket.destroy();
		reconnectTimer = setTimeout(() => {
			console.error('FAIL: no reconnect within 90 seconds');
			process.exitCode = 1;
			server.close();
		}, 90000);
	} else {
		socket.end(frame(8, Buffer.from([0x03, 0xe8])));
		server.close();
		console.log(`Hardware protocol smoke test passed (${completed} cycles). Test TLS and heap stability separately.`);
	}
}

const server = net.createServer(socket => {
	if (active) { socket.destroy(); return; }
	active = true;
	if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
	socket.on('close', () => { active = false; });
	let buffer = Buffer.alloc(0), upgraded = false;
	const fail = error => {
		console.error('FAIL:', error.message);
		socket.destroy();
		server.close();
		process.exitCode = 1;
	};
	socket.on('error', fail);
	socket.setTimeout(15000, () => fail(new Error('Client stalled during the protocol checks')));
	socket.on('data', chunk => {
		try {
			if (disconnectedAt) {
				const elapsed = Date.now() - disconnectedAt;
				assert(elapsed >= 55000, `Reconnect was too early: ${elapsed} ms`);
				console.log(`PASS reconnect after ${(elapsed / 1000).toFixed(1)} seconds`);
				disconnectedAt = 0;
			}
			buffer = Buffer.concat([buffer, chunk]);
			assert(buffer.length <= 64 * 1024, 'Client exceeded test buffer limit');
			if (!upgraded) {
				const end = buffer.indexOf('\r\n\r\n');
				if (end < 0) return;
				const request = buffer.subarray(0, end).toString();
				const key = /^Sec-WebSocket-Key:\s*(.+)$/im.exec(request)?.[1].trim();
				assert(key, 'Missing WebSocket key');
				buffer = buffer.subarray(end + 4);
				upgraded = true;
				const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
				exercise(socket, accept).catch(fail);
			}
			while (buffer.length >= 2) {
				assert(buffer[0] & 0x80, 'Expected a complete client frame');
				assert(buffer[1] & 0x80, 'Client frames must be masked');
				let length = buffer[1] & 0x7f, offset = 2;
				assert(length !== 127, 'Unexpected oversized client frame');
				if (length === 126) {
					if (buffer.length < 4) return;
					length = buffer.readUInt16BE(2);
					offset = 4;
				}
				if (buffer.length < offset + 4 + length) return;
				const opcode = buffer[0] & 0x0f;
				const mask = buffer.subarray(offset, offset + 4);
				const payload = Buffer.from(buffer.subarray(offset + 4, offset + 4 + length));
				for (let i = 0; i < length; i++) payload[i] ^= mask[i % 4];
				buffer = buffer.subarray(offset + 4 + length);
				if (opcode === 1) {
					const message = JSON.parse(payload.toString());
					if ('id' in message) deliver(message.id, message);
				} else if (opcode === 9) socket.write(frame(10, payload));
				else if (opcode === 10) deliver('pong', payload);
			}
		} catch (error) { fail(error); }
	});
});
server.on('error', error => { console.error(error.message); process.exitCode = 1; });
server.listen(port, '0.0.0.0', () => console.log(`Set the ESP32 MCP endpoint to ws://<this-machine-IP>:${port}/mcp and reboot it.`));
