const net = require('bare-net')
const fs = require('bare-fs')
const os = require('bare-os')
const path = require('bare-path')
const zlib = require('bare-zlib')
const b4a = require('b4a')
const assert = require('bare-assert')
const { Soulseek } = require('../js/index.js')

function u32le (n) {
  const b = b4a.allocUnsafe(4)
  b.writeUInt32LE(n >>> 0, 0)
  return b
}

function u64le (n) {
  const b = b4a.allocUnsafe(8)
  b.writeBigUInt64LE(BigInt(n), 0)
  return b
}

function packStr (s) {
  const raw = b4a.from(s, 'utf8')
  return b4a.concat([u32le(raw.byteLength), raw])
}

function frameU32 (code, parts) {
  const body = b4a.concat([u32le(code), ...parts])
  return b4a.concat([u32le(body.byteLength), body])
}

function frameU8 (code, parts) {
  const body = b4a.concat([b4a.from([code]), ...parts])
  return b4a.concat([u32le(body.byteLength), body])
}

function listenRandom (server) {
  return new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => {
      resolve(server.address().port)
    })
  })
}

async function main () {
  const logs = []
  const client = new Soulseek({
    logger: (ev) => logs.push(ev)
  })

  const initSt = client.state()
  assert(initSt.connected === false)
  assert(initSt.logged_in === false)
  assert(initSt.listen_port > 0)

  const sampleBytes = b4a.alloc(200 * 1024)
  for (let i = 0; i < sampleBytes.byteLength; i++) {
    sampleBytes[i] = (i * 17 + 3) & 0xff
  }

  let peerPort = 0

  const openSockets = new Set()
  // Mock peer TCP server (speaks PeerInit + Browse + Queue + F-transfer)
  const peerServer = net.createServer((sock) => {
    openSockets.add(sock)
    sock.on('close', () => openSockets.delete(sock))
    let buf = b4a.alloc(0)
    let seenInit = false
    sock.on('data', (chunk) => {
      buf = b4a.concat([buf, chunk])
      while (buf.byteLength >= 4) {
        const len = buf.readUInt32LE(0)
        if (buf.byteLength < 4 + len) break
        const payload = buf.subarray(4, 4 + len)
        buf = buf.subarray(4 + len)

        if (!seenInit) {
          seenInit = true
          assert(payload[0] === 1) // PeerInit
          continue
        }

        const code = payload.readUInt32LE(0)
        if (code === 15) {
          // UserInfoRequest -> UserInfoResponse (code 16)
          sock.write(frameU32(16, [
            packStr('Bare mock peer'),
            b4a.from([0]),
            u32le(2),
            u32le(1),
            b4a.from([1]),
            u32le(1)
          ]))
        } else if (code === 4) {
          // SharedFileListRequest -> SharedFileListResponse (code 5, zlib)
          const rawList = b4a.concat([
            u32le(1), // 1 folder
            packStr('@@lossless\\Aphex Twin'),
            u32le(1), // 1 file
            b4a.from([1]),
            packStr('01 - Xtal.flac'),
            u64le(sampleBytes.byteLength),
            packStr(''),
            u32le(3), // 3 attrs
            u32le(1), u32le(292),
            u32le(4), u32le(44100),
            u32le(5), u32le(16),
            u32le(0), // unknown
            u32le(0)  // 0 private folders
          ])
          const z = zlib.deflateSync(rawList)
          sock.write(frameU32(5, [z]))
        } else if (code === 43) {
          // QueueUpload -> PlaceInQueueResponse (44) + TransferRequest (40, dir=1)
          const fnLen = payload.readUInt32LE(4)
          const filename = b4a.toString(payload.subarray(8, 8 + fnLen), 'utf8')
          sock.write(frameU32(44, [packStr(filename), u32le(1)]))
          sock.write(frameU32(40, [
            u32le(1),
            u32le(777888),
            packStr(filename),
            u64le(sampleBytes.byteLength)
          ]))
        } else if (code === 41) {
          // TransferResponse(allowed=true) -> dial F socket back to client's listen_port
          const tok = payload.readUInt32LE(4)
          const allowed = payload[8]
          assert(tok === 777888 && allowed === 1)

          const fSock = net.connect(initSt.listen_port, '127.0.0.1', () => {
            fSock.write(frameU8(1, [packStr('bare_peer'), packStr('F'), u32le(0)]))
            fSock.write(u32le(777888))
          })
          let fBuf = b4a.alloc(0)
          fSock.on('data', (d) => {
            fBuf = b4a.concat([fBuf, d])
            if (fBuf.byteLength >= 8) {
              const offset = Number(fBuf.readBigUInt64LE(0))
              fSock.write(sampleBytes.subarray(offset))
            }
          })
        }
      }
    })
  })
  peerPort = await listenRandom(peerServer)

  // Mock Soulseek central server
  const slskServer = net.createServer((sock) => {
    openSockets.add(sock)
    sock.on('close', () => openSockets.delete(sock))
    let buf = b4a.alloc(0)
    let clientWaitPort = 0
    sock.on('data', (chunk) => {
      buf = b4a.concat([buf, chunk])
      while (buf.byteLength >= 4) {
        const len = buf.readUInt32LE(0)
        if (buf.byteLength < 4 + len) break
        const payload = buf.subarray(4, 4 + len)
        buf = buf.subarray(4 + len)

        const code = payload.readUInt32LE(0)
        if (code === 1) {
          // Login
          sock.write(frameU32(1, [
            b4a.from([1]),
            packStr('Welcome from Bare mock server'),
            b4a.from([1, 0, 0, 127]),
            packStr('5f4dcc3b5aa765d61d8327deb882cf99'),
            b4a.from([1])
          ]))
        } else if (code === 2) {
          clientWaitPort = payload.readUInt32LE(4)
        } else if (code === 92) {
          sock.write(frameU32(92, [u32le(3600)]))
        } else if (code === 26) {
          const searchTok = payload.readUInt32LE(4)
          const p = net.connect(clientWaitPort, '127.0.0.1', () => {
            p.write(frameU8(1, [packStr('bare_peer'), packStr('P'), u32le(0)]))
            const rawSearch = b4a.concat([
              packStr('bare_peer'),
              u32le(searchTok),
              u32le(1),
              b4a.from([1]),
              packStr('@@lossless\\Aphex Twin\\01 - Xtal.flac'),
              u64le(sampleBytes.byteLength),
              packStr(''),
              u32le(3),
              u32le(1), u32le(292),
              u32le(4), u32le(44100),
              u32le(5), u32le(16),
              b4a.from([1]), // slot_free
              u32le(1048576),
              u32le(0),
              u32le(0),
              u32le(0)
            ])
            p.write(frameU32(9, [zlib.deflateSync(rawSearch)]))
            p.end()
          })
        } else if (code === 3) {
          const uLen = payload.readUInt32LE(4)
          const uname = b4a.toString(payload.subarray(8, 8 + uLen), 'utf8')
          const portBuf = b4a.allocUnsafe(2)
          portBuf.writeUInt16LE(0, 0)
          sock.write(frameU32(3, [
            packStr(uname),
            b4a.from([1, 0, 0, 127]),
            u32le(peerPort),
            u32le(0),
            portBuf
          ]))
        }
      }
    })
  })
  const srvPort = await listenRandom(slskServer)

  // Verify JS event loop ticks freely while Rust runs network operations
  let timerTicks = 0
  const tickInterval = setInterval(() => { timerTicks++ }, 10)

  const loginState = await client.login('127.0.0.1', srvPort, 'bare_tester', 'SecretPass42')
  assert(loginState.connected === true)
  assert(loginState.logged_in === true)

  const hits = await client.search('xtal flac', 200)
  assert(hits.length === 1)
  assert(hits[0].username === 'bare_peer')
  assert(hits[0].slots === true)
  assert(hits[0].sample_rate === 44100)
  assert(hits[0].bit_depth === 16)
  assert(hits[0].length_secs === 292)
  assert(hits[0].bitrate === 1411)

  const shared = await client.browse('bare_peer', 3000)
  assert(shared.length === 1)
  assert(shared[0].filename === '@@lossless\\Aphex Twin\\01 - Xtal.flac')

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'bare-slsk-'))
  const outPath = path.join(tmpDir, '01 - Xtal.flac')
  const progressEvents = []

  const dl = await client.download('bare_peer', shared[0].filename, outPath, (prog) => {
    progressEvents.push(prog)
  })
  assert(dl.path === outPath)
  assert(dl.size === sampleBytes.byteLength)
  const disk = fs.readFileSync(outPath)
  assert(b4a.equals(disk, sampleBytes))
  assert(progressEvents.length >= 2)
  assert(timerTicks > 5)

  for (const l of logs) {
    assert(!l.message.includes('SecretPass42'))
    assert(!l.message.includes('@@lossless\\Aphex Twin'))
  }

  clearInterval(tickInterval)
  client.close()
  for (const s of openSockets) s.destroy()
  peerServer.close()
  slskServer.close()
  fs.rmSync(tmpDir, { recursive: true, force: true })

  console.log('Bare addon E2E passed:', {
    state: loginState,
    hits: hits.length,
    browsed: shared.length,
    downloadedBytes: dl.size,
    progressCallbacks: progressEvents.length,
    jsTimerTicksDuringSearch: timerTicks
  })
}

main().catch((err) => {
  console.error(err)
  Bare.exit(1)
})
