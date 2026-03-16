// WaveUI — everything is OSC
//
// miniwave exposes its entire state via OSC UDP on port 9000.
// This bridge translates WebSocket JSON <-> OSC for browser clients.
//
// You don't need this bridge or this UI. Any OSC client can control
// miniwave directly. The spec is at http://localhost:8080/osc-spec
//
// Build your own UI. Vibe code it. That's the point.

'use strict'

const http = require('http')
const crypto = require('crypto')
const dgram = require('dgram')
const fs = require('fs')
const path = require('path')

const HTTP_PORT = 8080
const OSC_HOST = '127.0.0.1'
const OSC_SEND_PORT = 9000
const OSC_RECV_PORT = 9001

// ── OSC Spec ─────────────────────────────────────────────────────────

const OSC_SPEC = {
  name: 'WaveUI OSC Protocol',
  version: '1.0',
  description: 'miniwave rack host control protocol. All communication is OSC over UDP.',
  transport: { send: { host: OSC_HOST, port: OSC_SEND_PORT, protocol: 'UDP' }, receive: { port: OSC_RECV_PORT, protocol: 'UDP' } },
  addresses: {
    '/rack/slot/set': { args: [{ type: 'i', name: 'channel', desc: 'MIDI channel 0-15' }, { type: 's', name: 'type', desc: 'Instrument type name (e.g. "fm-synth")' }], desc: 'Assign an instrument to a channel slot' },
    '/rack/slot/clear': { args: [{ type: 'i', name: 'channel' }], desc: 'Remove instrument from a channel slot' },
    '/rack/slot/volume': { args: [{ type: 'i', name: 'channel' }, { type: 'f', name: 'volume', range: [0, 1] }], desc: 'Set per-channel volume' },
    '/rack/slot/mute': { args: [{ type: 'i', name: 'channel' }, { type: 'i', name: 'mute', desc: '1=muted, 0=unmuted' }], desc: 'Toggle channel mute' },
    '/rack/slot/solo': { args: [{ type: 'i', name: 'channel' }, { type: 'i', name: 'solo', desc: '1=solo, 0=unsolo' }], desc: 'Toggle channel solo' },
    '/rack/master': { args: [{ type: 'f', name: 'volume', range: [0, 1] }], desc: 'Set master volume' },
    '/rack/status': { args: [], desc: 'Request rack status. Response: per-slot (active_i, type_s, volume_f, mute_i, solo_i) x16, then master_volume_f, midi_device_s', response: true },
    '/rack/types': { args: [], desc: 'Request registered instrument types. Response: list of type name strings', response: true },
    '/midi/devices': { args: [], desc: 'Request available MIDI devices. Response: pairs of (device_id_s, device_name_s)', response: true },
    '/midi/device': { args: [{ type: 's', name: 'device', desc: 'ALSA device string e.g. "hw:1,0"' }], desc: 'Switch MIDI input device' },
    '/ch/N/status': { args: [], desc: 'Request instrument status for channel N. Response format depends on instrument type.', response: true },
    '/ch/N/preset': { args: [{ type: 'i', name: 'preset_index' }], desc: 'Set preset by index (does not load params into live)' },
    '/ch/N/preset/load': { args: [{ type: 'i', name: 'preset_index' }], desc: 'Load preset and copy params to live sliders' },
    '/ch/N/volume': { args: [{ type: 'f', name: 'volume', range: [0, 1] }], desc: 'Set instrument internal volume' },
    '/ch/N/param/carrier_ratio': { args: [{ type: 'f', name: 'value', range: [0.1, 4] }], desc: 'FM synth carrier ratio (enables override)' },
    '/ch/N/param/mod_ratio': { args: [{ type: 'f', name: 'value', range: [0.1, 15] }], desc: 'FM synth modulator ratio' },
    '/ch/N/param/mod_index': { args: [{ type: 'f', name: 'value', range: [0, 15] }], desc: 'FM synth modulation index' },
    '/ch/N/param/attack': { args: [{ type: 'f', name: 'value', range: [0.001, 1] }], desc: 'Envelope attack time (seconds)' },
    '/ch/N/param/decay': { args: [{ type: 'f', name: 'value', range: [0.01, 3] }], desc: 'Envelope decay time' },
    '/ch/N/param/sustain': { args: [{ type: 'f', name: 'value', range: [0, 1] }], desc: 'Envelope sustain level' },
    '/ch/N/param/release': { args: [{ type: 'f', name: 'value', range: [0.01, 4] }], desc: 'Envelope release time' },
    '/ch/N/param/feedback': { args: [{ type: 'f', name: 'value', range: [0, 1] }], desc: 'FM feedback amount' },
    '/ch/N/param/reset': { args: [], desc: 'Disable parameter override, revert to preset defaults' },
    '/ch/N/note/on': { args: [{ type: 'i', name: 'note', range: [0, 127] }, { type: 'i', name: 'velocity', range: [1, 127] }], desc: 'Trigger note on' },
    '/ch/N/note/off': { args: [{ type: 'i', name: 'note', range: [0, 127] }], desc: 'Trigger note off' }
  }
}

// ── OSC Encoding ─────────────────────────────────────────────────────

function oscPad4(n) { return (n + 3) & ~3 }

function oscEncodeString(str) {
  const len = Buffer.byteLength(str, 'utf8') + 1
  const padded = oscPad4(len)
  const buf = Buffer.alloc(padded, 0)
  buf.write(str, 'utf8')
  return buf
}

function oscEncodeMessage(address, typeTags, args) {
  const parts = [oscEncodeString(address)]
  let tagStr = ','
  for (const t of typeTags) tagStr += t
  parts.push(oscEncodeString(tagStr))

  let ai = 0
  for (const t of typeTags) {
    if (t === 'i') {
      const b = Buffer.alloc(4)
      b.writeInt32BE(args[ai++])
      parts.push(b)
    } else if (t === 'f') {
      const b = Buffer.alloc(4)
      b.writeFloatBE(args[ai++])
      parts.push(b)
    } else if (t === 's') {
      parts.push(oscEncodeString(String(args[ai++])))
    }
  }
  return Buffer.concat(parts)
}

function oscDecode(buf) {
  if (buf.length < 4 || buf[0] !== 0x2F) return null // must start with '/'
  let pos = 0

  // Read address
  let addrEnd = pos
  while (addrEnd < buf.length && buf[addrEnd] !== 0) addrEnd++
  const address = buf.toString('utf8', pos, addrEnd)
  pos = oscPad4(addrEnd + 1)

  // Read type tag
  let types = ''
  if (pos < buf.length && buf[pos] === 0x2C) { // ','
    let tagEnd = pos
    while (tagEnd < buf.length && buf[tagEnd] !== 0) tagEnd++
    types = buf.toString('utf8', pos + 1, tagEnd)
    pos = oscPad4(tagEnd + 1)
  }

  // Read args
  const args = []
  for (let t = 0; t < types.length && pos < buf.length; t++) {
    if (types[t] === 'i') {
      if (pos + 4 <= buf.length) args.push({ type: 'i', value: buf.readInt32BE(pos) })
      pos += 4
    } else if (types[t] === 'f') {
      if (pos + 4 <= buf.length) args.push({ type: 'f', value: buf.readFloatBE(pos) })
      pos += 4
    } else if (types[t] === 's') {
      let sEnd = pos
      while (sEnd < buf.length && buf[sEnd] !== 0) sEnd++
      args.push({ type: 's', value: buf.toString('utf8', pos, sEnd) })
      pos = oscPad4(sEnd + 1)
    }
  }

  return { address, types, args }
}

// ── JSON -> OSC Translation ──────────────────────────────────────────

function jsonToOsc(msg) {
  switch (msg.type) {
    case 'slot_set':
      return oscEncodeMessage('/rack/slot/set', ['i', 's'], [msg.channel, msg.instrument])
    case 'slot_clear':
      return oscEncodeMessage('/rack/slot/clear', ['i'], [msg.channel])
    case 'slot_volume':
      return oscEncodeMessage('/rack/slot/volume', ['i', 'f'], [msg.channel, msg.value])
    case 'slot_mute':
      return oscEncodeMessage('/rack/slot/mute', ['i', 'i'], [msg.channel, msg.value])
    case 'slot_solo':
      return oscEncodeMessage('/rack/slot/solo', ['i', 'i'], [msg.channel, msg.value])
    case 'master_volume':
      return oscEncodeMessage('/rack/master', ['f'], [msg.value])
    case 'rack_status':
      return oscEncodeMessage('/rack/status', [], [])
    case 'rack_types':
      return oscEncodeMessage('/rack/types', [], [])
    case 'midi_devices':
      return oscEncodeMessage('/midi/devices', [], [])
    case 'midi_device':
      return oscEncodeMessage('/midi/device', ['s'], [msg.value])
    case 'ch': {
      const addr = `/ch/${msg.channel}${msg.path}`
      const tags = []
      const args = []
      if (msg.iargs) { for (const v of msg.iargs) { tags.push('i'); args.push(v) } }
      if (msg.fargs) { for (const v of msg.fargs) { tags.push('f'); args.push(v) } }
      return oscEncodeMessage(addr, tags, args)
    }
    case 'ch_status':
      return oscEncodeMessage(`/ch/${msg.channel}/status`, [], [])
    case 'note_on':
      return oscEncodeMessage(`/ch/${msg.channel}/note/on`, ['i', 'i'], [msg.note, msg.velocity])
    case 'note_off':
      return oscEncodeMessage(`/ch/${msg.channel}/note/off`, ['i'], [msg.note])
    default:
      return null
  }
}

// ── OSC -> JSON Translation ──────────────────────────────────────────

function oscToJson(decoded) {
  if (decoded.address === '/rack/status') {
    const slots = []
    let i = 0
    for (let ch = 0; ch < 16; ch++) {
      if (i + 4 >= decoded.args.length) break
      slots.push({
        active: decoded.args[i++].value,
        type: decoded.args[i++].value,
        volume: decoded.args[i++].value,
        mute: decoded.args[i++].value,
        solo: decoded.args[i++].value
      })
    }
    const master_volume = i < decoded.args.length ? decoded.args[i++].value : 0.75
    const midi_device = i < decoded.args.length ? decoded.args[i++].value : ''
    return { type: 'rack_status', slots, master_volume, midi_device }
  }

  if (decoded.address === '/rack/types') {
    return { type: 'rack_types', types: decoded.args.map(a => a.value) }
  }

  if (decoded.address === '/midi/devices') {
    const devices = []
    for (let i = 0; i + 1 < decoded.args.length; i += 2) {
      devices.push({ id: decoded.args[i].value, name: decoded.args[i + 1].value })
    }
    return { type: 'midi_devices', devices }
  }

  // /status response from channel instrument (fm-synth format)
  if (decoded.address === '/status') {
    // isfiffffffffi
    const a = decoded.args
    if (a.length >= 13) {
      return {
        type: 'ch_status',
        preset_index: a[0].value,
        preset_name: a[1].value,
        volume: a[2].value,
        override: a[3].value,
        params: {
          carrier_ratio: a[4].value,
          mod_ratio: a[5].value,
          mod_index: a[6].value,
          attack: a[7].value,
          decay: a[8].value,
          sustain: a[9].value,
          release: a[10].value,
          feedback: a[11].value
        },
        active_voices: a[12].value
      }
    }
  }

  return { type: 'osc_raw', address: decoded.address, args: decoded.args }
}

// ── WebSocket Frame Handling ─────────────────────────────────────────

function wsAcceptKey(key) {
  return crypto
    .createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-5AB9DC85B575')
    .digest('base64')
}

function wsDecodeFrame(buf) {
  if (buf.length < 2) return null
  const fin = (buf[0] & 0x80) !== 0
  const opcode = buf[0] & 0x0F
  const masked = (buf[1] & 0x80) !== 0
  let payloadLen = buf[1] & 0x7F
  let offset = 2

  if (payloadLen === 126) {
    if (buf.length < 4) return null
    payloadLen = buf.readUInt16BE(2)
    offset = 4
  } else if (payloadLen === 127) {
    if (buf.length < 10) return null
    payloadLen = Number(buf.readBigUInt64BE(2))
    offset = 10
  }

  let maskKey = null
  if (masked) {
    if (buf.length < offset + 4) return null
    maskKey = buf.slice(offset, offset + 4)
    offset += 4
  }

  const totalLen = offset + payloadLen
  if (buf.length < totalLen) return null

  let payload = buf.slice(offset, totalLen)
  if (masked && maskKey) {
    payload = Buffer.from(payload)
    for (let i = 0; i < payload.length; i++) {
      payload[i] ^= maskKey[i & 3]
    }
  }

  return { fin, opcode, payload, totalLen }
}

function wsEncodeFrame(opcode, payload) {
  const data = typeof payload === 'string' ? Buffer.from(payload, 'utf8') : payload
  let header
  if (data.length < 126) {
    header = Buffer.alloc(2)
    header[0] = 0x80 | opcode
    header[1] = data.length
  } else if (data.length < 65536) {
    header = Buffer.alloc(4)
    header[0] = 0x80 | opcode
    header[1] = 126
    header.writeUInt16BE(data.length, 2)
  } else {
    header = Buffer.alloc(10)
    header[0] = 0x80 | opcode
    header[1] = 127
    header.writeBigUInt64BE(BigInt(data.length), 2)
  }
  return Buffer.concat([header, data])
}

// ── WebSocket Client Management ──────────────────────────────────────

const clients = new Set()
let activeDetailChannel = -1 // which channel is being polled for detail

function broadcast(jsonObj) {
  const data = wsEncodeFrame(0x01, JSON.stringify(jsonObj))
  for (const client of clients) {
    try { client.socket.write(data) } catch (e) { removeClient(client) }
  }
}

function removeClient(client) {
  clients.delete(client)
  // If no clients have a detail open, stop detail polling
  updateDetailPolling()
}

function updateDetailPolling() {
  // Find the most recently requested detail channel from any client
  let ch = -1
  for (const c of clients) {
    if (c.detailChannel >= 0) ch = c.detailChannel
  }
  activeDetailChannel = ch
}

// ── UDP OSC Socket ───────────────────────────────────────────────────

// Single UDP socket — bind to OSC_RECV_PORT so replies from miniwave come back here
const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true })

udp.on('message', (buf, rinfo) => {
  const decoded = oscDecode(buf)
  if (!decoded) return
  const json = oscToJson(decoded)
  if (json) broadcast(json)
})

udp.on('error', (err) => {
  console.error(`[bridge] UDP error: ${err.message}`)
})

udp.bind(OSC_RECV_PORT, '0.0.0.0', () => {
  console.log(`[bridge] OSC on UDP :${OSC_RECV_PORT} → ${OSC_HOST}:${OSC_SEND_PORT}`)
})

function sendOsc(buf) {
  if (buf) udp.send(buf, 0, buf.length, OSC_SEND_PORT, OSC_HOST)
}

// ── Polling ──────────────────────────────────────────────────────────

// Rack status every 500ms
setInterval(() => {
  if (clients.size === 0) return
  sendOsc(oscEncodeMessage('/rack/status', [], []))
}, 500)

// Channel detail every 200ms
setInterval(() => {
  if (clients.size === 0 || activeDetailChannel < 0) return
  sendOsc(oscEncodeMessage(`/ch/${activeDetailChannel}/status`, [], []))
}, 200)

// ── HTTP + WebSocket Server ──────────────────────────────────────────

const server = http.createServer((req, res) => {
  if (req.url === '/osc-spec') {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' })
    res.end(JSON.stringify(OSC_SPEC, null, 2))
    return
  }

  // Serve index.html
  const filePath = path.join(__dirname, 'index.html')
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404)
      res.end('not found')
      return
    }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' })
    res.end(data)
  })
})

server.on('upgrade', (req, socket, head) => {
  const key = req.headers['sec-websocket-key']
  if (!key) { socket.destroy(); return }

  const accept = wsAcceptKey(key)
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n` +
    '\r\n'
  )

  const client = { socket, detailChannel: -1, buffer: Buffer.alloc(0) }
  clients.add(client)
  console.log(`[bridge] client connected (${clients.size} total)`)

  socket.on('data', (data) => {
    client.buffer = Buffer.concat([client.buffer, data])

    while (client.buffer.length >= 2) {
      const frame = wsDecodeFrame(client.buffer)
      if (!frame) break
      client.buffer = client.buffer.slice(frame.totalLen)

      if (frame.opcode === 0x08) {
        // Close
        try { socket.write(wsEncodeFrame(0x08, Buffer.alloc(0))) } catch (e) {}
        socket.end()
        removeClient(client)
        return
      }

      if (frame.opcode === 0x09) {
        // Ping -> Pong
        try { socket.write(wsEncodeFrame(0x0A, frame.payload)) } catch (e) {}
        continue
      }

      if (frame.opcode === 0x0A) continue // Pong, ignore

      if (frame.opcode === 0x01) {
        // Text frame
        try {
          const msg = JSON.parse(frame.payload.toString('utf8'))

          // Track detail channel for polling
          if (msg.type === 'ch_status' || msg.type === 'ch' || msg.type === 'note_on' || msg.type === 'note_off') {
            client.detailChannel = msg.channel
            updateDetailPolling()
          }
          if (msg.type === 'detail_close') {
            client.detailChannel = -1
            updateDetailPolling()
            continue
          }

          const osc = jsonToOsc(msg)
          sendOsc(osc)
        } catch (e) {
          console.error('[bridge] bad JSON:', e.message)
        }
      }
    }
  })

  socket.on('close', () => {
    removeClient(client)
    console.log(`[bridge] client disconnected (${clients.size} total)`)
  })

  socket.on('error', () => {
    removeClient(client)
  })
})

server.listen(HTTP_PORT, () => {
  console.log(`[bridge] WaveUI at http://localhost:${HTTP_PORT}`)
  console.log(`[bridge] OSC spec at http://localhost:${HTTP_PORT}/osc-spec`)
  console.log(`[bridge] OSC target: ${OSC_HOST}:${OSC_SEND_PORT}, recv: :${OSC_RECV_PORT}`)
})
