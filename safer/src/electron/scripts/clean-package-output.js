const fs = require('fs')
const path = require('path')

const unpacked = path.resolve(__dirname, '..', '..', '..', 'dist', 'win-unpacked')
fs.rmSync(unpacked, { recursive: true, force: true })
console.log(`[clean-package-output] removed stale directory: ${unpacked}`)
