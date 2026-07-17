#!/usr/bin/env python3
"""NB systemd 滚动部署工具(正式)。

装了 systemd 守护后, 用本工具一键 build + deploy。
当前 `deploy.py deploy-socks` 也已切到 systemd 路径；本脚本只是把 build 一并串起来。

流程:
  1. kz 编译最新 src/nb_node.c, 产物 fetch 到本地 build/nb_node
  2. 分发 build/nb_node 到三跳 + `systemctl restart nb-{exit,middle,entry}`
     (顺序 exit->middle->entry, entry 依赖后两跳在线)
  3. 冒烟: 经 SOCKS5 1080 走三跳, 出口 IP 应为 kz(2.135.147.102)

用法: python tools/redeploy.py
"""
import sys, pathlib
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import deploy

KZ_IP = "2.135.147.102"


def main():
    # 1) 编译(kz); act_build 内部若编译失败会 sys.exit("编译失败, 中止")(基于产物存在性判断, 可靠)
    print(">>> [1/3] 编译最新源码(kz) ...")
    deploy.act_build(['exit'])
    bindata = (deploy.BUILD_DIR / 'nb_node').read_bytes()
    print(f"    OK, 产物 {len(bindata)} bytes")

    # 2) 分发安全材料 + 原子安装 systemd + 重启三跳 + 认证冒烟。
    print(">>> [2/3] 分发 + 重启三跳 ...")
    deploy.act_deploy_socks()

    # 3) 总结
    print(f">>> [3/3] 完成。目标出口 IP 应为 {KZ_IP}，上一步冒烟已给出结果。")


if __name__ == '__main__':
    main()
