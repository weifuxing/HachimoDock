/**
 * [Input] Action prompt, reference image and Kling API Key / video settings.
 * [Output] Submitted task ID, polled status and downloaded video bytes via the API-key endpoints.
 * [Pos] provider node in pc/src/lib/avatar-pipeline/providers
 * [Sync] Update this header if the provider contract changes.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, sleep, withRetry } from "../http.js";
import { KLING_BASE_URL, requireKlingVideoModel, klingVideoParameters } from "../kling-models.js";

const POLL_TIMEOUT_MS = 20 * 60 * 1000;

function buildPayload({ model, settings, prompt, imageBase64 }) {
  const contents = [
    { type: "prompt", text: prompt },
    { type: "first_frame", url: imageBase64 },
  ];
  if (settings.endFrame) contents.push({ type: "last_frame", url: imageBase64 });
  return {
    contents,
    settings: {
      duration: settings.duration,
      resolution: settings.resolution,
      ...(model.audio ? { audio: settings.generateAudio ? "native" : "off" } : {}),
      ...(model.multiShot ? { multi_shot: false } : {}),
    },
    options: { watermark_info: { enabled: settings.watermark } },
  };
}

async function requestKling(path, { apiKey, signal, payload }) {
  const submitting = payload !== undefined;
  const method = submitting ? "POST" : "GET";
  return withRetry(async () => {
    const response = await pipelineFetch(`${KLING_BASE_URL}${path}`, {
      diagnosticStage: submitting ? "video_submit" : "video_poll",
      method,
      signal,
      headers: { Authorization: `Bearer ${apiKey}`, "Content-Type": "application/json" },
      ...(submitting ? { body: JSON.stringify(payload) } : {}),
    });
    let json;
    try {
      json = await readJsonOrThrow(response, "可灵 AI");
    } catch (error) {
      error.message = error.message.replaceAll(apiKey, "[已隐藏]");
      throw error;
    }
    if (json?.code != null && json.code !== 0) {
      throw new Error(`可灵 AI 返回错误（${json.code}）：${String(json.message || "").replaceAll(apiKey, "[已隐藏]").slice(0, 200)}`);
    }
    return json;
  }, { retries: submitting ? 0 : 3, signal });
}

export async function runKlingFamily({ config, prompt, imageBase64, signal, onStage }) {
  const model = requireKlingVideoModel(config);
  const apiKey = config.apiKey.trim();
  const settings = klingVideoParameters(model, config);
  const payload = buildPayload({ model, settings, prompt, imageBase64 });
  onStage?.({ stage: "submitting" });
  const submitted = await requestKling(`/image-to-video/${model.id}`, { apiKey, signal, payload });
  const taskId = submitted?.data?.id;
  if (typeof taskId !== "string" || !taskId) throw new Error("可灵 AI 提交响应缺少任务 ID，请在控制台核对任务。");

  const started = Date.now();
  let attempt = 0;
  while (true) {
    if (signal?.aborted) throw new DOMException("aborted", "AbortError");
    const elapsedMs = Date.now() - started;
    if (elapsedMs > POLL_TIMEOUT_MS) throw new Error(`可灵 AI 任务 ${taskId} 查询超时，请在控制台核对结果。`);
    onStage?.({ stage: "polling", detail: { taskId, attempt, elapsedMs } });
    const result = await requestKling(`/tasks?task_ids=${encodeURIComponent(taskId)}`, { apiKey, signal });
    const task = Array.isArray(result?.data) ? result.data.find(item => item.id === taskId) : null;
    if (!task) throw new Error("可灵 AI 查询响应缺少对应任务，请在控制台核对结果。");
    if (task.status === "succeeded") {
      const output = Array.isArray(task.outputs) && task.outputs.find(item => item.type === "video");
      const videoUrl = settings.watermark ? output?.watermark_url : output?.url;
      if (!/^https:\/\//i.test(videoUrl || "")) throw new Error("可灵 AI 任务成功，但未返回所选视频的下载地址。");
      onStage?.({ stage: "downloading", detail: { taskId, videoUrl } });
      const videoBytes = await withRetry(() => downloadBinary(videoUrl, signal), { retries: 3, signal });
      onStage?.({ stage: "done", detail: { taskId } });
      return { taskId, videoBytes, videoUrl, raw: { submit: submitted, poll: result } };
    }
    if (task.status !== "submitted" && task.status !== "processing") {
      throw new Error(`可灵 AI 任务 ${taskId} 状态 ${task.status}：${String(task.message || "").replaceAll(apiKey, "[已隐藏]").slice(0, 200)}`);
    }
    await sleep(elapsedMs < 30000 ? 5000 : 10000, signal);
    attempt += 1;
  }
}
