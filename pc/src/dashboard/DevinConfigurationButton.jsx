/**
 * [Input] Existing Devin cloud configuration commands and local Agent detection.
 * [Output] A Devin configuration entry and dialog for credentials and cloud session sync.
 * [Pos] Agent interaction, on the Devin row.
 * [Sync] Keep pc/src/dashboard/.folder.md current.
 */

import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { AlertCircle, CheckCircle2, ExternalLink, Eye, EyeOff, Loader2, Settings2, X } from "lucide-react";
import { useExitProtection } from "../lib/exit-protection.js";
import { useDeviceContext } from "../shell/DeviceContext.jsx";

export default function DevinConfigurationButton({ disabled }) {
  const [open, setOpen] = useState(false);
  const entryRef = useRef(null);
  const closeDialog = () => {
    setOpen(false);
    entryRef.current?.focus();
  };
  return (
    <>
      <button
        ref={entryRef}
        type="button"
        className="btn-secondary btn-sm channel-row__change-inline"
        aria-haspopup="dialog"
        disabled={disabled}
        title={disabled ? "检测到 Devin 后可配置" : undefined}
        onClick={() => setOpen(true)}
      >
        <Settings2 size={14} aria-hidden="true" />
        Devin 配置
      </button>
      {open && <DevinConfigurationDialog onClose={closeDialog} />}
    </>
  );
}

function DevinConfigurationDialog({ onClose }) {
  const { refreshAgents } = useDeviceContext();
  const dialogRef = useRef(null);
  const [showSecret, setShowSecret] = useState(false);
  const [devin, setDevin] = useState({
    loading: true,
    pending: false,
    testPending: false,
    token: "",
    hasToken: false,
    orgId: "",
    enabled: false,
    tone: "muted",
    message: "正在读取 Devin 配置…",
  });
  const [savedDevinDraft, setSavedDevinDraft] = useState('["","",false]');
  const devinDraft = JSON.stringify([devin.token, devin.orgId, devin.enabled]);
  const dirty = savedDevinDraft !== devinDraft;
  const busy = devin.pending || devin.testPending;
  useExitProtection(busy ? "Devin 配置正在保存或测试" : dirty ? "Devin 配置有未保存的修改" : "");

  useEffect(() => {
    const dialog = dialogRef.current;
    dialog.showModal();
    return () => { dialog.close(); };
  }, []);

  const close = () => {
    if (busy) return;
    if (dirty && !window.confirm("Devin 配置有未保存的修改，确定放弃吗？")) return;
    dialogRef.current.close();
    onClose();
  };

  useEffect(() => {
    let cancelled = false;
    invoke("devin_cloud_get_config")
      .then((config) => {
        if (cancelled) return;
        const draft = JSON.stringify(["", config.orgId || "", config.enabled === true]);
        setSavedDevinDraft(draft);
        setDevin((current) => ({
          ...current,
          loading: false,
          hasToken: config.hasToken === true,
          orgId: config.orgId || "",
          enabled: config.enabled === true,
          tone: config.error ? "error" : "muted",
          message: config.error || (config.hasToken ? "已读取加密保存的 Devin 配置。" : "尚未配置 Devin PAT。"),
        }));
      })
      .catch((error) => {
        if (cancelled) return;
        setDevin((current) => ({
          ...current,
          loading: false,
          tone: "error",
          message: `读取 Devin 配置失败：${error}`,
        }));
      });
    return () => { cancelled = true; };
  }, []);

  const saveDevin = async () => {
    const orgId = devin.orgId.trim();
    setDevin((current) => ({ ...current, pending: true, tone: "muted", message: "正在保存 Devin 配置…" }));
    try {
      await invoke("devin_cloud_save_config", {
        token: devin.token.trim() ? devin.token : null,
        orgId,
        enabled: devin.enabled,
      });
      const hasToken = devin.hasToken || Boolean(devin.token.trim());
      const draft = JSON.stringify(["", orgId, devin.enabled]);
      setSavedDevinDraft(draft);
      setDevin((current) => ({
        ...current,
        pending: false,
        token: "",
        orgId,
        hasToken,
        tone: "success",
        message: "配置已加密保存，云端状态轮询已更新。",
      }));
      refreshAgents().catch(() => {});
    } catch (error) {
      setDevin((current) => ({
        ...current,
        pending: false,
        tone: "error",
        message: `保存 Devin 配置失败：${error}`,
      }));
    }
  };

  const testDevin = async () => {
    setDevin((current) => ({ ...current, testPending: true, tone: "muted", message: "正在测试 Devin 连接…" }));
    try {
      const result = await invoke("devin_cloud_test");
      setDevin((current) => ({
        ...current,
        testPending: false,
        tone: result.ok ? "success" : "error",
        message: result.message,
      }));
    } catch (error) {
      setDevin((current) => ({
        ...current,
        testPending: false,
        tone: "error",
        message: `Devin 连接测试失败：${error}`,
      }));
    }
  };

  return (
    <dialog
      ref={dialogRef}
      className="modal-card devin-config-dialog"
      aria-labelledby="devin-config-title"
      aria-describedby="devin-config-description"
      onCancel={(event) => {
        event.preventDefault();
        close();
      }}
    >
      <header className="modal-header">
        <h2 id="devin-config-title" className="modal-title">Devin 云端会话</h2>
        <button type="button" className="icon-btn" onClick={close} disabled={busy} aria-label="关闭 Devin 配置">
          <X size={16} />
        </button>
      </header>
      <div className="modal-body devin-config-dialog__body">
        <p id="devin-config-description" className="muted small">
          PAT 和组织 ID 加密保存在本机。启用后每 15 秒同步会话状态和标题到设备气泡。
        </p>
        <div className="api-settings__form api-settings__form--devin">
          <label className="ui-field" htmlFor="devin-config-token">
            <span className="ui-field__label">Personal Access Token（PAT）</span>
            <input
              id="devin-config-token"
              className="ui-control api-settings__secret-input"
              type={showSecret ? "text" : "password"}
              autoComplete="new-password"
              value={devin.token}
              onChange={(event) => setDevin((current) => ({ ...current, token: event.target.value }))}
              placeholder={devin.hasToken ? "PAT 已保存，留空可保留" : "粘贴 Devin PAT"}
              disabled={devin.loading || busy}
            />
          </label>
          <label className="ui-field" htmlFor="devin-config-org">
            <span className="ui-field__label">组织 ID</span>
            <input
              id="devin-config-org"
              className="ui-control"
              value={devin.orgId}
              onChange={(event) => setDevin((current) => ({ ...current, orgId: event.target.value }))}
              placeholder="org-..."
              disabled={devin.loading || busy}
            />
          </label>
          <label className="ui-field api-settings__devin-enable">
            <span>
              <input
                type="checkbox"
                checked={devin.enabled}
                onChange={(event) => setDevin((current) => ({ ...current, enabled: event.target.checked }))}
                disabled={devin.loading || busy}
              />
              启用 Devin 云端会话状态同步
            </span>
          </label>
          <div className="api-settings__devin-actions">
            <a className="api-settings__credential-link" href="https://docs.devin.ai/api-reference/personal-access-tokens"
              target="_blank" rel="noreferrer">
              PAT 创建说明 <ExternalLink size={11} aria-hidden="true" />
            </a>
            {devin.token && (
              <button type="button" className="btn-ghost btn-sm" onClick={() => setShowSecret((value) => !value)}>
                {showSecret ? <EyeOff size={14} /> : <Eye size={14} />}
                {showSecret ? "隐藏 PAT" : "显示 PAT"}
              </button>
            )}
          </div>
        </div>
        <div className={`api-settings__result${devin.tone ? ` is-${devin.tone}` : ""}`} role="status">
          {devin.loading ? <Loader2 size={13} className="spin" /> : devin.tone === "error" ? <AlertCircle size={13} /> : <CheckCircle2 size={13} />}
          {devin.message}
        </div>
      </div>
      <footer className="devin-config-dialog__footer">
        <button type="button" className="btn-secondary btn-sm" onClick={testDevin}
          disabled={devin.loading || busy || !devin.hasToken || dirty}>
          {devin.testPending ? <Loader2 size={13} className="spin" /> : null}
          测试连接
        </button>
        <button type="button" className="btn-primary btn-sm" onClick={saveDevin}
          disabled={devin.loading || busy}>
          {devin.pending ? <Loader2 size={13} className="spin" /> : null}
          保存
        </button>
      </footer>
    </dialog>
  );
}
