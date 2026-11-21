import test from 'node:test';
import assert from 'node:assert/strict';
import {firmwareUpdateFailureMessage as format} from './firmware-update-error.js';
test('begin timeout stays unconfirmed and transport failure is distinguished',()=>{
 assert.match(format('firmware begin failed: firmware OTA acknowledgement timed out at begin; no replay'),/准备升级.*未收到设备确认/);
 assert.match(format(new Error('Broken pipe')),/USB 连接中断.*尚未确认/);
 assert.match(format('firmware chip revision mismatch'),/芯片版本不匹配/);
 assert.match(format('insufficient OTA headroom'),/分区空间不足/);
});
test('unknown native failures do not expose private identifiers or promise retry/success',()=>{
 const message=format('native-failure /Users/private/profile Bearer private-token board-private');
 assert.doesNotMatch(message,/private|Bearer|成功|自动重试/);
});

test('released native preflight messages distinguish old-partition, chip and status failures',()=>{
 assert.match(format('无法确认设备实际 OTA 分区容量；未开始更新，也未清除任何传输'),/无法确认设备可用的升级分区.*尚未写入/);
 assert.match(format('设备当前 OTA 分区余量不足，须保留至少 128 KiB。'),/分区空间不足/);
 assert.match(format('固件与设备芯片版本不匹配，已阻止升级'),/芯片版本不匹配/);
 assert.match(format('firmware status query timed out'),/未收到设备状态/);
 assert.match(format('timed out waiting for board p4-private to reconnect with firmware 1.2.3 in imageState=valid: device response timed out for diagnostics/query'),/已提交.*尚未确认/);
});

test('post-reboot failure messages distinguish rollback from incomplete or mismatched state',()=>{
 assert.match(format('ota_rollback'),/回到了升级前的固件/);
  assert.match(format('ota_rollback', { wireless: true }),/已停用这份更新/);
  assert.match(format('firmware_image_rolled_back'),/已停用这份更新/);
 assert.match(format('ota_partition_mismatch'),/未运行预期的固件/);
 assert.match(format('ota_image_state_invalid'),/未通过设备启动验证/);
 assert.match(format('ota_image_state_missing'),/启动状态不完整/);
});
