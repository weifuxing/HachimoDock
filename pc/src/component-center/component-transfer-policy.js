/** Read-only checks and explicit deletion intent; never issues a mutation. */
export async function syncAudioPreflight(request, boardDeviceId) {
  const reply=await request('usb_get_diagnostics',{expectedBoardDeviceId:boardDeviceId,componentSync:true});
  const audio=reply?.syncAudio;
  if(!audio||typeof audio.blocked!=='boolean')throw new Error('无法确认设备音频状态，尚未开始同步；请稍后重新检查。');
  return audio;
}
export function deviceDeletePolicy({clearSave=false,transport,capabilities={}}) {
  if(!clearSave&&transport!=='wifi'&&(transport!=='usb'||capabilities.widgetDeletePreserveSave!==true))
    throw new Error('当前设备或连接不支持保留存档卸载；未删除。请升级固件，或明确勾选清除设备存档。');
  return {preserveSave:!clearSave,clearDeviceSaveConfirmed:clearSave};
}
export function componentCommitOutcomeUnknown(error) {
  return /acknowledg.*timed out|commit.*(?:timeout|timed out)|回执.*超时|组件提交(?:结果尚未确认|回执格式未确认)|未收到板端组件 OTA 确认:.*phase=commit\b|component catalog outcome unknown|durable catalog media publication failed/i.test(String(error));
}

export function componentTransferError(error) {
  const raw = String(error);
  if (componentCommitOutcomeUnknown(raw)) return '设备尚未确认安装结果。请先刷新设备组件清单，核对后再决定是否重新同步。';
  if (/audio or component storage is busy|audio started; catalog commit deferred/i.test(raw)) {
    return '设备拒绝提交安装，可能是音频占用或组件存储忙。请等待当前操作结束；如正在播放或录音，请先停止，再重新同步。';
  }
  if (/device audio is busy; catalog not committed/i.test(raw)) {
    return '设备正在播放或使用语音，本次未完成安装。请停止播放或结束语音后再同步。';
  }
  if (/storage is busy or recovery blocked/i.test(raw)) {
    return '设备暂时无法写入组件。请先刷新设备组件清单，查看是否有存储异常。';
  }
  if (/component storage is busy; commit has not started/i.test(raw)) {
    return '设备正在处理其他存储操作，本次尚未开始安装。请稍等后重试。';
  }
  if (/component storage interrupted; catalog not committed|physical verification lost storage ownership/i.test(raw)) {
    return '组件提交因存储操作中断，安装未完成。请等待当前操作结束后再同步。';
  }
  if (/storage.*busy/i.test(raw)) return '设备正在处理组件存储。请等待当前操作结束并检查设备清单，再决定是否重新同步。';
  return raw;
}
