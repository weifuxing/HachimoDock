/** User-facing OTA outcomes; raw native errors and identifiers stay out of toasts. */
export function firmwareAlreadyLatest(error) {
 const text=typeof error==='string'?error:String(error?.message||error||'');
 return text==='firmware_already_latest' || text==='设备固件已是最新版本。' || /device firmware .* is already newer than or equal to bundled firmware/i.test(text);
}
export function firmwareRollback(error) {
 const text=typeof error==='string'?error:String(error?.message||error||'');
 return text==='ota_rollback' || text==='firmware_image_rolled_back';
}
export function firmwareUpdateFailureMessage(error, { wireless = false } = {}) {
 const text=typeof error==='string'?error:String(error?.message||error||'');
 if(firmwareAlreadyLatest(text))
  return '设备固件已是最新版本。';
 if(firmwareRollback(text))
  return '设备回到了升级前的固件。此设备已停用这份更新，请等待新版固件。';
 if(text==='firmware_rollback_record_unavailable')
  return '无法读取固件回退记录，尚未写入固件。';
 if(wireless) {
  if(text==='无线固件升级正在进行。' || text==='another firmware transfer is active') return '已有固件升级正在进行，请等待设备重启并校验。';
  if(/授权无线控制|尚未授权/.test(text)) return '请将同一台设备通过 USB 连接一次，授权无线控制后可直接更新。';
  if(/身份已改变|配对已变更/.test(text)) return '目标设备或配对已变化，请先检查当前连接和固件版本。';
  if(/启动状态尚未确认|升级结果未确认/.test(text)) return '无线升级结果尚未确认，请先查看设备当前固件版本。';
  if(/固件校验失败|板型标记不匹配/.test(text)) return '内置固件校验未通过，请核对设备型号和客户端版本。';
  return '无线升级未完成，请检查设备是否在线并核对当前版本。';
 }
 if(text==='firmware_board_selection_required')
  return '请先选择设备的实际板型。';
 if(text==='ota_version_mismatch'||text==='ota_partition_mismatch')
  return '设备重启后未运行预期的固件。请先查看设备当前版本。';
 if(text==='ota_image_state_invalid')
  return '新固件未通过设备启动验证。请先查看设备当前版本。';
 if(text==='ota_boot_count_missing'||text==='ota_runtime_missing'||text==='ota_image_state_missing')
  return '固件已提交，但设备返回的启动状态不完整。请先查看设备当前版本。';
 if(/timed out waiting for board[^\n]*imageState=valid/i.test(text))
  return '固件已提交，但设备重启后的版本尚未确认。请保持 USB 连接，先查看设备当前版本。';
 if(/firmware status query timed out|device response timed out for diagnostics\/query/i.test(text))
  return '未收到设备状态。请保持 USB 连接，确认设备已启动后再试。';
 if(/无法确认设备实际 OTA 分区容量|设备未提供独立的 OTA 更新槽/i.test(text))
  return '无法确认设备可用的升级分区，尚未写入固件。';
 if(/firmware begin failed:[\s\S]*timed out/i.test(text))
  return '准备升级时未收到设备确认。已停止后续传输，请保持 USB 连接，确认设备状态后再操作。';
 if(/broken pipe|not connected|connection[^\n]*(?:closed|changed|lost)|no such file or directory/i.test(text))
  return '升级过程中 USB 连接中断，本次结果尚未确认。请保持连接，检查设备当前版本和状态。';
 if(/无法确认设备芯片版本/i.test(text))
  return '无法确认设备芯片版本，尚未写入固件。请核对芯片版本后再操作。';
 if(/板型.*(?:不匹配|不一致|切换|无效)|无法识别设备板型/i.test(text))
  return '无法确认固件与设备板型匹配。请使用能报告板型的固件连接设备。';
 if(/chip revision|firmware chip|wrong chip|固件与设备芯片版本不匹配/i.test(text))
  return '固件与设备芯片版本不匹配，本次升级已停止。请使用该设备对应的固件。';
 if(/headroom|exceeds[^\n]*OTA|insufficient[^\n]*OTA|OTA 分区余量不足/i.test(text))
  return '设备的 OTA 分区空间不足，当前无法通过普通升级安装此固件。';
 if(/pending validation|pendingVerify|pending verification/i.test(text))
  return '设备尚在确认上次升级结果，请等待启动确认完成。';
 if(/appearance asset transfer is active|another firmware transfer is active|firmware transfer already active/i.test(text))
  return '设备仍在处理已有同步或升级任务，请先确认该任务状态。';
 if(/firmware (?:upload|commit) failed/i.test(text))
  return '升级传输中断或未收到确认。请保持 USB 连接，先检查设备当前版本和升级状态。';
 return '固件升级未完成。请保持 USB 连接，检查设备状态后再操作。';
}
