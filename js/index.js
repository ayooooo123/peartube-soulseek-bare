const binding = require('../binding.js')

class Soulseek {
  constructor (opts = {}) {
    const listenPort = Number.isInteger(opts.listenPort) && opts.listenPort >= 0 ? opts.listenPort : 0
    this._handle = binding.create(listenPort)
    this._closed = false
    if (typeof opts.logger === 'function') {
      this.setLogger(opts.logger)
    }
  }

  setLogger (fn) {
    if (this._closed) throw new Error('Soulseek client is closed')
    binding.setLogger(this._handle, typeof fn === 'function' ? fn : null)
    return this
  }

  state () {
    return binding.state(this._handle)
  }

  async login (server, port, user, pass) {
    if (this._closed) throw new Error('Soulseek client is closed')
    return binding.login(this._handle, String(server || ''), Number(port) || 0, String(user || ''), String(pass || ''))
  }

  async search (query, timeoutMs = 5000) {
    if (this._closed) throw new Error('Soulseek client is closed')
    return binding.search(this._handle, String(query || ''), Number(timeoutMs) || 5000)
  }

  async browse (user, timeoutMs = 30000) {
    if (this._closed) throw new Error('Soulseek client is closed')
    return binding.browse(this._handle, String(user || ''), Number(timeoutMs) || 30000)
  }

  async download (user, filename, destPath, onProgress = null) {
    if (this._closed) throw new Error('Soulseek client is closed')
    return binding.download(
      this._handle,
      String(user || ''),
      String(filename || ''),
      String(destPath || ''),
      typeof onProgress === 'function' ? onProgress : null
    )
  }

  close () {
    if (this._closed) return
    this._closed = true
    binding.close(this._handle)
  }
}

module.exports = {
  Soulseek,
  createClient: (opts) => new Soulseek(opts),
  binding
}
