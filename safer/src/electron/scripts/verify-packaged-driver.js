const crypto = require('crypto')
const fs = require('fs')
const path = require('path')

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').toUpperCase()
}

const stagingDirectory = path.resolve(__dirname, '..', 'driver')
const packageOutputDirectory = process.env.PS_PACKAGE_OUTPUT_DIR
  ? path.resolve(process.env.PS_PACKAGE_OUTPUT_DIR)
  : path.resolve(__dirname, '..', '..', '..', 'dist')
const packagedDirectory = path.join(packageOutputDirectory, 'win-unpacked', 'resources', 'driver')
const expectedName = 'PersonalSafer.sys'
const stagingNames = fs.readdirSync(stagingDirectory)
if (!stagingNames.includes(expectedName)) {
  throw new Error(`staging driver has incorrect or missing filename: ${stagingNames.join(', ')}`)
}

const packagedPath = path.join(packagedDirectory, expectedName)
if (!fs.existsSync(packagedPath)) {
  throw new Error(`packaged driver is missing: ${packagedPath}`)
}

const stagingPath = path.join(stagingDirectory, expectedName)
const stagingHash = sha256(stagingPath)
const packagedHash = sha256(packagedPath)
if (stagingHash !== packagedHash) {
  throw new Error(`packaged driver hash mismatch: ${stagingHash} != ${packagedHash}`)
}

console.log(`[verify-packaged-driver] ${packagedPath}`)
console.log(`[verify-packaged-driver] SHA256 ${packagedHash}`)
