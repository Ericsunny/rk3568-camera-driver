#!/bin/sh
# install_3a_autostart.sh —— 把 rkaiq 3A 做成开机自启的 systemd 服务
# 用法（板上）： sh install_3a_autostart.sh [install|uninstall|status]
set -e

UNIT=/etc/systemd/system/rkaiq_3A.service
WANT=/etc/systemd/system/multi-user.target.wants/rkaiq_3A.service

case "${1:-install}" in
  install)
    cat > $UNIT <<'EOF'
[Unit]
Description=RKAIQ 3A daemon (IMX415 / AIQ 2022 trio)
Documentation=file:///etc/iqfiles/README.md
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/bin/rkaiq_3A_server
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable rkaiq_3A
    echo "--- 已启用，尝试启动 ---"
    systemctl restart rkaiq_3A
    sleep 5
    systemctl is-active rkaiq_3A || true
    echo "--- 最近日志 ---"
    journalctl -u rkaiq_3A -n 6 --no-pager 2>/dev/null | tail -6 || true
    echo "--- 进程 ---"
    pidof rkaiq_3A_server || echo "没有进程"
    ;;
  uninstall)
    systemctl disable --now rkaiq_3A || true
    rm -f $UNIT $WANT
    systemctl daemon-reload
    echo "已卸载"
    ;;
  status)
    systemctl status rkaiq_3A --no-pager 2>&1 | head -12
    ;;
  *)
    echo "用法: sh $0 [install|uninstall|status]"
    exit 1
    ;;
esac
