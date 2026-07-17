const { execFileSync } = require('child_process')
const fs = require('fs')
const path = require('path')

const driverPath = path.join(__dirname, '..', 'driver', 'PersonalSafer.sys')
const escapedPath = driverPath.replace(/'/g, "''")
const status = execFileSync(
  'powershell.exe',
  ['-NoProfile', '-Command', `(Get-AuthenticodeSignature -LiteralPath '${escapedPath}').Status`],
  { encoding: 'utf8' }
).trim()

if (status !== 'Valid') {
  throw new Error(`Refusing to package kernel driver with signature status: ${status || 'Unknown'}`)
}

const image = fs.readFileSync(driverPath)
const peOffset = image.readUInt32LE(0x3c)
const optionalHeaderOffset = peOffset + 24
const forceIntegrity = 0x0080
const dllCharacteristics = image.readUInt16LE(optionalHeaderOffset + 0x46)
if ((dllCharacteristics & forceIntegrity) === 0) {
  throw new Error('Refusing to package a driver without IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY')
}

console.log(`[verify-driver-signature] Driver signature: ${status}`)
console.log('[verify-driver-signature] Driver image: Force Integrity')
