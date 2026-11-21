import GlobalAudioCuesPanel from '../GlobalAudioCuesPanel.jsx';
import React,{useCallback,useEffect,useMemo,useRef,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {RefreshCw,Volume2,VolumeX} from 'lucide-react';
import Button from '../shell/Button';
import {createDeviceVolumeController,EMPTY_VOLUME_STATE} from './device-volume-controller.js';
import './device-volume.css';
import {useExitProtection} from '../lib/exit-protection.js';
import {wifiDeviceControl, wifiDeviceControlReady} from '../lib/wifi-device-control.js';

const DEFAULT_DEVICE_VOLUME = 80;
const VOLUME_PRESETS = [0, 25, 50, 75, 100];
const WIFI_VOLUME_RESULTS = {
  applied: value => `设备已确认本次音量设置为 ${value}%。`,
  failed: () => '设备未能确认实际音量。',
  cancelled: () => '音量请求已取消或过期。',
  unknown: () => '本次设置结果未确认，可重新查询结果。',
};

function UsbDeviceVolume({usb,active}) {
  const [state,setState]=useState({...EMPTY_VOLUME_STATE});
  const control=useMemo(()=>createDeviceVolumeController({invoke,onState:setState}),[]);
  const engine=usb?.capabilities?.audioMasterVolume;
  useEffect(()=>{void control.setTarget({connected:usb?.connected,boardDeviceId:usb?.boardDeviceId,bootId:usb?.bootId,connectionId:usb?.connectionId,
    portName:usb?.portName,runtime:usb?.runtime,engine});},[control,usb?.connectionId,usb?.connected,usb?.boardDeviceId,usb?.bootId,usb?.portName,usb?.runtime,engine]);
  useEffect(()=>()=>control.dispose(),[control]);
  useEffect(()=>{if(!active)return;const timer=setInterval(()=>{const s=control.getSnapshot();if(s.known&&s.draft===s.value&&!s.persisted&&!s.busy)void control.refresh();},5000);return()=>clearInterval(timer);},[active,control]);
  useExitProtection(state.busy && !state.loading ? "设备音量请求尚未完成" : "");
  const disabled=!state.available||!state.known||state.busy;
  const level=state.known?`${state.draft}%`:state.available&&state.draft===null?`默认 ${DEFAULT_DEVICE_VOLUME}%`:'—';
  return <div className="device-volume">
    <div className="device-volume__level">
      {state.known&&state.value===0?<VolumeX size={22}/>:<Volume2 size={22}/>}
      <input type="range" min="0" max="100" step="1" value={state.known?state.draft:DEFAULT_DEVICE_VOLUME} disabled={disabled}
        aria-label="设备总音量" aria-valuetext={state.known?level:'尚未读取设备音量'}
        onChange={event=>control.setDraft(Number(event.target.value))}
        onPointerUp={()=>{void control.apply();}} onKeyUp={()=>{void control.apply();}}/>
      <output aria-live="polite">{level}</output>
    </div>
    <div className="device-volume__presets">
      {VOLUME_PRESETS.map(value=><Button key={value} variant="ghost" size="small" disabled={disabled}
        onClick={()=>{void control.apply(value);}}>{value===0?'静音':`${value}%`}</Button>)}
      <Button variant="ghost" size="small" disabled={!state.available||state.busy}
        onClick={()=>{void control.refresh();}}><RefreshCw size={14}/>读取设备音量</Button>
    </div>
    <p className="device-volume__message" role={state.error?'alert':undefined}>
      {state.error || (!state.available ? state.reason : state.waitingForTransfer ? '设备正在传输，完成后将自动读取音量…' : state.loading ? '正在读取设备音量…' : state.busy ? '正在应用，请等待设备确认…'
        : !state.known ? '请先读取设备音量' : !state.persisted ? (state.savePending===false ? '当前音量已应用，但尚未保存到设备；重启后可能恢复原值。' : '已应用，等待设备空闲后保存。') : state.value===0 ? '设备已静音。' : '音量已保存到设备。')}
    </p>
  </div>;
}

function WifiDeviceVolume({ wifi, active }) {
  const [value, setValue] = useState('');
  const [request, setRequest] = useState(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [observedVolume, setObservedVolume] = useState(null);
  const [volumeReadError, setVolumeReadError] = useState('');
  const [persisted, setPersisted] = useState(null);
  const readSequence = useRef(0);
  const [reading, setReading] = useState(false);
  const awaiting = request?.checking === true;
  const authorized = wifi.trustedVolume && wifi.controlTrusted;
  const canReadVolume = wifiDeviceControlReady(wifi);
  const disabled = !authorized || busy || awaiting;
  const valid = /^[0-9]{1,3}$/.test(value) && Number(value) <= 100;
  useExitProtection(busy || awaiting ? '设备音量请求尚未完成' : '');

  const readVolume = useCallback(async () => {
    if (!canReadVolume) return;
    const sequence = ++readSequence.current;
    setReading(true);
    setVolumeReadError('');
    try {
      const reply = await wifiDeviceControl(invoke, wifi, 'readVolume');
      if (sequence !== readSequence.current) return;
      if (reply.engine !== 'p4-master-volume-v1' || !Number.isInteger(reply.volume) || reply.volume < 0 || reply.volume > 100) {
        throw new Error('设备没有返回有效的音量。');
      }
      setObservedVolume(reply.volume);
      setValue(String(reply.volume));
      setPersisted(reply.persisted);
    } catch (problem) {
      if (sequence !== readSequence.current) return;
      setObservedVolume(null);
      setPersisted(null);
      setVolumeReadError(String(problem?.message || problem));
    } finally {
      if (sequence === readSequence.current) setReading(false);
    }
  }, [canReadVolume, wifi]);

  useEffect(() => {
    if (active) void readVolume();
  }, [active, readVolume]);

  useEffect(() => {
    if (!active || persisted !== false || busy || awaiting || Number(value) !== observedVolume) return;
    const timer = setInterval(() => void readVolume(), 5000);
    return () => clearInterval(timer);
  }, [active, awaiting, busy, observedVolume, persisted, readVolume, value]);

  useEffect(() => {
    if (!awaiting) return;
    let retired = false;
    let timer;
    const deadline = Date.now() + 10_000;
    async function poll() {
      const result = await invoke('lan_volume_result', { id: request.id }).catch(() => 'unknown');
      if (retired) return;
      if (['applied', 'failed', 'cancelled'].includes(result)) {
        setRequest({ ...request, result, checking: false });
        void readVolume();
      } else if (Date.now() >= deadline) {
        setRequest({ ...request, result: 'unknown', checking: false });
        void readVolume();
      } else {
        timer = setTimeout(poll, 700);
      }
    }
    timer = setTimeout(poll, 700);
    return () => { retired = true; clearTimeout(timer); };
  }, [awaiting, readVolume, request]);

  async function apply(volume) {
    if (disabled || (canReadVolume && reading)) return;
    if (canReadVolume && observedVolume === volume && persisted === true) return;
    setBusy(true);
    setValue(String(volume));
    setRequest(null);
    setError('');
    try {
      const [id, result] = await invoke('lan_volume_request', { expectedBootId: wifi.bootId, volume });
      const checking = !['applied', 'failed', 'cancelled'].includes(result);
      setRequest({ id, result, value: volume, checking });
      if (!checking) void readVolume();
    } catch (problem) {
      setError(problem?.message || String(problem));
      void readVolume();
    } finally {
      setBusy(false);
    }
  }

  return <div className="device-volume">
    {!wifi.panelControl && <p className="device-volume__message">当前固件需要 USB 回读音量。</p>}
    {canReadVolume && <div className="device-volume__level">
      {observedVolume === 0 ? <VolumeX size={22} /> : <Volume2 size={22} />}
      <input type="range" min="0" max="100" step="1" value={valid ? value : DEFAULT_DEVICE_VOLUME}
        disabled={disabled || reading || observedVolume === null}
        aria-label="设备总音量" aria-valuetext={observedVolume === null ? '尚未读取设备音量' : `${value}%`}
        onChange={event => setValue(event.target.value)}
        onPointerUp={event => void apply(Number(event.currentTarget.value))}
        onKeyUp={event => void apply(Number(event.currentTarget.value))} />
      <output aria-live="polite">{observedVolume === null ? '—' : `${observedVolume}%`}</output>
    </div>}
    {canReadVolume && persisted === false && observedVolume !== null
      && <p className="device-volume__message">当前音量已应用，设备尚未保存。</p>}
    {volumeReadError && <p className="device-volume__message" role="alert">读取音量失败：{volumeReadError}</p>}
    {!wifi.trustedVolume ? <p className="device-volume__message">当前固件未开放无线音量调节。</p>
      : !wifi.controlTrusted ? <p className="device-volume__message">请通过 USB 连接同一台设备，授权无线控制。</p>
        : <>
          {!canReadVolume && <form className="device-volume__presets" onSubmit={event => { event.preventDefault(); if (valid) void apply(Number(value)); }}>
            <label className="ui-field"><span>设备音量（0–100%）</span><input type="text" inputMode="numeric" maxLength={3}
              value={value} onChange={event => setValue(event.target.value)} disabled={disabled} /></label>
            <Button type="submit" size="small" disabled={disabled || !valid}>设置音量</Button>
          </form>}
          <div className="device-volume__presets">
            {VOLUME_PRESETS.map(volume => <Button key={volume} size="small" variant="ghost" disabled={disabled || reading}
              onClick={() => void apply(volume)}>{volume === 0 ? '静音' : `${volume}%`}</Button>)}
            {canReadVolume && <Button size="small" variant="ghost" disabled={reading || busy || awaiting} onClick={() => void readVolume()}>
              <RefreshCw size={14} />{reading ? '正在读取…' : '读取设备音量'}
            </Button>}
            {request?.result === 'unknown' && !awaiting && <Button size="small" variant="ghost" disabled={disabled}
              onClick={() => setRequest({ ...request, checking: true })}>查询设置结果</Button>}
          </div>
        </>}
    {(busy || awaiting || request || error) && <p className="device-volume__message" role={error ? 'alert' : 'status'}>
      {error || (busy || awaiting ? '正在等待设备空闲并确认实际音量…' : WIFI_VOLUME_RESULTS[request?.result]?.(request.value))}
    </p>}
  </div>;
}

export default function DeviceVolumePanel({ usb, wifi, active = true }) {
  const wired = usb?.connected === true;
  return <>
    {!wired && wifi ? <WifiDeviceVolume key={`${wifi.boardDeviceId}:${wifi.bootId}`} wifi={wifi} active={active} />
      : <UsbDeviceVolume usb={usb} active={active} />}
    <GlobalAudioCuesPanel boardDeviceId={wired ? usb.boardDeviceId : undefined} wifiConnected={!wired && Boolean(wifi)} />
  </>;
}
