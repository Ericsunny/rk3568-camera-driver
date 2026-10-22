#!/bin/sh
# stop_status_check.sh —— 停流错误的"可定位"通道验证(只读参数)
P=/sys/module/ov8858_sm/parameters
LOG=/root/stop_status_$(date +%H%M).log

{
echo "===== 停流错误可定位性 ====="
echo "kernel: $(uname -r) $(uname -v)"
echo "参数默认值: last_stop_err=$(cat $P/last_stop_err) stop_err_cnt=$(cat $P/stop_err_cnt)"
echo
echo "--- A) 正常停流(注入关闭) ---"
echo 0 > $P/inject_stop_err
/root/streamoff_test /dev/video0 1632 1224 2>&1 | grep -E "STREAMOFF|RESULT"
echo "  参数: last_stop_err=$(cat $P/last_stop_err) stop_err_cnt=$(cat $P/stop_err_cnt)"
echo
echo "--- B) 注入停流失败(-EIO) ---"
echo 5 > $P/inject_stop_err
/root/streamoff_test /dev/video0 1632 1224 2>&1 | grep -E "STREAMOFF|RESULT"
echo "  参数: last_stop_err=$(cat $P/last_stop_err) stop_err_cnt=$(cat $P/stop_err_cnt)"
echo "  dmesg: $(dmesg | grep -c 'stop_stream failed') 条 'stop_stream failed'"
echo
echo "--- C) 恢复后错误码仍可追溯(不清零,便于事后定位) ---"
echo 0 > $P/inject_stop_err
/root/streamoff_test /dev/video0 1632 1224 2>&1 | grep -E "RESULT"
echo "  参数: last_stop_err=$(cat $P/last_stop_err) stop_err_cnt=$(cat $P/stop_err_cnt)"
} | tee $LOG
