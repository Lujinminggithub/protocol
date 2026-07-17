const { app } = require('electron')

function fail(message) {
  console.error(`[policy-store] ${message}`)
  app.exit(1)
}

app.whenReady().then(() => {
  const modulePath = process.env.PS_POLICY_STORE_MODULE
  const phase = process.env.PS_POLICY_STORE_PHASE || 'write'
  if (!modulePath) return fail('PS_POLICY_STORE_MODULE is missing')

  try {
    const store = require(modulePath)
    store.initializePolicyStore()

    if (phase === 'write') {
      const invalidErrors = store.validatePolicy({
        blockedPorts: ['invalid'],
        blockedUrls: ['/1', '/2', '/3', '/4', '/5'],
      })
      if (invalidErrors.length < 2) return fail('invalid policy was not rejected')

      store.persistPolicy({
        fileFilterEnabled: true,
        networkFilterEnabled: true,
        auditEnabled: true,
        fileExtensions: ['.secret'],
        blockedPorts: [8443],
        blockedDomains: ['blocked.example'],
        blockedUrls: ['/private'],
      })
    }

    const policy = store.getPersistedPolicy()
    if (policy.fileExtensions[0] !== '.secret' ||
        policy.blockedPorts[0] !== 8443 ||
        policy.blockedDomains[0] !== 'blocked.example') {
      return fail(`persisted policy mismatch during ${phase}`)
    }

    console.log(JSON.stringify({ phase, path: store.getPolicyStoreInfo().path, policy }))
    app.exit(0)
  } catch (error) {
    fail(error?.stack || error?.message || String(error))
  }
})
