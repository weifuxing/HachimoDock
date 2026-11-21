// Kling's API-key image-to-video endpoints and supported output settings.
export const KLING_BASE_URL = "https://api-beijing.klingai.com";
export const KLING_KEY_URL = "https://klingai.com/app/dev/api-key";

const seconds = max => Array.from({ length: max - 2 }, (_, index) => index + 3);
const hd = ["720p", "1080p"];

export const KLING_MODELS = [
  { id: "kling-4.0-flash", label: "Kling 4.0 Flash", durations: seconds(20), resolutions: ["720p"] },
  { id: "kling-4.0", label: "Kling 4.0", durations: seconds(30), resolutions: hd, endFrame: true },
  { id: "kling-3.0-turbo", label: "Kling 3.0 Turbo", durations: seconds(15), resolutions: hd },
  { id: "kling-3.0", label: "Kling 3.0", durations: seconds(15), resolutions: [...hd, "4k"], endFrame: true, audio: true, multiShot: true },
  { id: "kling-2.6", label: "Kling 2.6", durations: [5, 10], resolutions: hd, endFrame: true, audio: true },
  { id: "kling-2.5-turbo", label: "Kling 2.5 Turbo", durations: [5, 10], resolutions: hd, endFrame: true },
];

export function klingModel(id) {
  return KLING_MODELS.find(model => model.id === id);
}

export function klingVideoParameters(model, value = {}) {
  const generateAudio = Boolean(model.audio && value.generateAudio === true);
  const audio26 = generateAudio && model.id === "kling-2.6";
  return {
    duration: model.durations.includes(Number(value.duration)) ? Number(value.duration) : 5,
    resolution: audio26 ? "1080p" : model.resolutions.includes(value.resolution) ? value.resolution : "720p",
    generateAudio,
    endFrame: Boolean(model.endFrame && !audio26 && value.endFrame === true),
    watermark: value.watermark === true,
  };
}

export function requireKlingVideoModel(config = {}) {
  if (!config.apiKey?.trim()) throw new Error("请先在 API 配置中保存可灵 AI 的 API Key。");
  const model = klingModel(config.model || KLING_MODELS[0].id);
  if (!model) throw new Error("请在 API 配置中选择新版可灵视频模型。");
  return model;
}
