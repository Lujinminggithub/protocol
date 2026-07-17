/**
 * copy-native.js - 将编译好的原生产物复制到 Electron 打包目录
 *
 * 打包（electron-builder.yml 的 extraResources）会把 native/*.node 复制到
 * 应用的 resources/native/，把 driver/*.sys|*.inf 复制到 resources/driver/，
 * 与 native_loader.ts 打包后的查找路径保持一致。
 */
const fs = require('fs')
const path = require('path')

// ---- N-API 原生模块 ----
const nativeSrc = path.join(__dirname, '..', '..', 'native', 'build', 'Release', 'personal_safer.node')
const nativeDestDir = path.join(__dirname, '..', 'native')

if (fs.existsSync(nativeSrc)) {
  fs.mkdirSync(nativeDestDir, { recursive: true })
  fs.copyFileSync(nativeSrc, path.join(nativeDestDir, 'personal_safer.node'))
  console.log('[copy-native] 已复制原生模块到:', path.join(nativeDestDir, 'personal_safer.node'))
} else {
  console.warn('[copy-native] 未找到原生模块，跳过:', nativeSrc)
  console.warn('[copy-native] 如需系统监控真实数据，请先在 src/native 下运行: npx node-gyp rebuild')
}

// ---- 内核驱动 (.sys/.inf) ----
const canonicalDriverDir = path.join(__dirname, '..', '..', '..', 'build', 'kernel', 'x64', 'Release')
const legacyDriverDir = path.join(__dirname, '..', '..', 'kernel', 'x64', 'Release')
const driverSrcDir = fs.existsSync(path.join(canonicalDriverDir, 'PersonalSafer.sys'))
  ? canonicalDriverDir
  : legacyDriverDir
const driverDestDir = path.join(__dirname, '..', 'driver')
const driverFiles = ['PersonalSafer.sys', 'PersonalSafer.inf']

const driverBuilt = fs.existsSync(path.join(driverSrcDir, 'PersonalSafer.sys'))
if (driverBuilt) {
  fs.mkdirSync(driverDestDir, { recursive: true })
  for (const f of driverFiles) {
    const s = path.join(driverSrcDir, f)
    if (fs.existsSync(s)) {
      const existing = fs.readdirSync(driverDestDir).find((name) => name.toLowerCase() === f.toLowerCase())
      if (existing) fs.unlinkSync(path.join(driverDestDir, existing))
      fs.copyFileSync(s, path.join(driverDestDir, f))
      console.log('[copy-native] 已复制驱动文件到:', path.join(driverDestDir, f))
    }
  }
  // 随包携带测试签名证书（sign-driver.ps1 生成于仓库根），供目标机自动信任
  const cerSrc = path.join(__dirname, '..', '..', '..', 'PersonalSafer-Test.cer')
  if (fs.existsSync(cerSrc)) {
    fs.copyFileSync(cerSrc, path.join(driverDestDir, 'PersonalSafer-Test.cer'))
    console.log('[copy-native] 已复制测试证书到:', path.join(driverDestDir, 'PersonalSafer-Test.cer'))
  } else {
    console.warn('[copy-native] 未找到测试证书（driver 将无法在目标机自动信任）:', cerSrc)
    console.warn('[copy-native] 请先运行 sign-driver.ps1 生成并签名')
  }
} else {
  console.warn('[copy-native] 未找到内核驱动，跳过:', driverSrcDir)
  console.warn('[copy-native] 如需随包携带驱动，请先运行: build_kernel.cmd')
}
