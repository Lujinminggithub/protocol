const path = require('path')

const actionArg = process.argv.find((arg) => arg.startsWith('--action=')) || '--action=status'
const action = actionArg.slice('--action='.length)
if (!['status', 'unlock', 'relock'].includes(action)) {
  process.stderr.write('action must be status, unlock, or relock\n')
  process.exit(1)
}

const addon = require(path.resolve(__dirname, '..', 'build', 'native', 'personal_safer.node'))
const kernel = addon?.dlp?.kernel_comm
if (!kernel?.connect?.()) {
  process.stderr.write('cannot connect or initialize PersonalSafer protection\n')
  process.exit(1)
}

try {
  if (action !== 'status' && !kernel.setProtectionMaintenanceMode(action === 'unlock')) {
    throw new Error(`${action} protection control failed`)
  }
  process.stdout.write(`${JSON.stringify(kernel.getProtectionState(), null, 2)}\n`)
} finally {
  kernel.disconnect?.()
}
