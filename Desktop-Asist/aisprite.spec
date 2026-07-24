# -*- mode: python ; coding: utf-8 -*-

import certifi

block_cipher = None

a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[
        # 内置 ffmpeg/ffprobe(剧本成片流水线的视频拼接/转码/字幕烧录用)。
        # 构建前需先运行 python scripts/fetch_ffmpeg.py 下载这两个二进制。
        ('assets/ffmpeg/ffmpeg.exe', 'assets/ffmpeg'),
        ('assets/ffmpeg/ffprobe.exe', 'assets/ffmpeg'),
    ],
    datas=[
        ('assets/icons', 'assets/icons'),
        # 修复 PyInstaller 打包 requests 后找不到 CA 证书包的经典问题
        # (https://github.com/pyinstaller/pyinstaller/issues/6352)：
        # 显式把 certifi 的 cacert.pem 放进打包产物里，保持 certifi 包内部的
        # 目录结构(cacert.pem 与 certifi 模块同级)，这样 certifi.where() 在
        # frozen 环境下依然能正确解析出路径。
        (certifi.where(), 'certifi'),
    ],
    hiddenimports=[
        'PyQt6',
        'PyQt6.QtWidgets',
        'PyQt6.QtCore',
        'PyQt6.QtGui',
        'requests',
        'certifi',
        'psutil',
        'win32api',
        'win32con',
        'win32gui',
        'win32crypt',
        # edge-tts(剧本成片的旁白配音) 及其依赖，显式声明防止子模块漏检
        'edge_tts',
        'aiohttp',
        'yarl',
        'multidict',
        'frozenlist',
        'aiosignal',
        'aiohappyeyeballs',
        'propcache',
        'attr',
        'attrs',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    name='AISprite',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='assets/icons/sprite.ico',
    exclude_binaries=True,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='AISprite',
)
