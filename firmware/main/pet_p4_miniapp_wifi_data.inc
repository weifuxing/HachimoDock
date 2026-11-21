/* Included by miniapp after runtime-action admission helpers. Runtime lock held. */
#if PET_P4_SYSTEM_WIFI
static EXT_RAM_BSS_ATTR pet_p4_data_view_t wifi_news;
static uint8_t wifi_news_index;
static uint32_t wifi_news_revision;

static bool wifi_news_available(uint64_t now, bool fresh_only) {
  if (!g_runtime.active || !g_runtime.wifi_news) return false;
  pet_p4_widget_data_view(PET_WIFI_DATA_NEWS, 0, now, &wifi_news);
  if (!wifi_news.count || (!wifi_news.restored && wifi_news.age_ms > PET_WIFI_NEWS_AGE_LIMIT_MS)) return false;
  if (!wifi_news.stale) return true;
  return !fresh_only && pet_app_host_mode(g_runtime.widget_id, now) == 7;
}

static bool wifi_news_owns_view(uint64_t now) {
  if (!g_runtime.active || !g_runtime.wifi_news) return false;
  if (wifi_news_available(now, false)) return true;
  wifi_news.count = 0;
  return pet_device_wifi_online();
}

static bool wifi_news_action_enabled(const char *action) {
  return !strcmp(action, "host.refresh") ||
    (wifi_news.count > 1 && (!strcmp(action, "host.previous") || !strcmp(action, "host.next")));
}

static void wifi_news_observation(uint64_t now, pet_host_observation_t *out) {
  *out = (pet_host_observation_t){
    .valid = true, .incarnation = UINT32_MAX, .revision = g_image_revision,
    .variable_epoch = wifi_news.count ? (now - wifi_news.age_ms) * 8 + wifi_news_index : 0,
  };
  copy_utf8(out->component_id, sizeof(out->component_id), g_runtime.widget_id);
  strcpy(out->context, "wifi-news");
}

static void wifi_news_sync_index(void) {
  if (wifi_news_revision != g_image_revision || wifi_news_index >= wifi_news.count) {
    wifi_news_index = 0;
    wifi_news_revision = g_image_revision;
  }
}

static void wifi_news_project(pet_p4_miniapp_view_t *view, pet_host_render_t *host, uint64_t now) {
  if (!wifi_news_owns_view(now)) return;
  if (host) {
    memset(host, 0, sizeof(*host));
    host->current = true;
    host->fresh = true;
    strcpy(host->actions[host->action_count++], "host.refresh");
    if (wifi_news.count > 1) {
      strcpy(host->actions[host->action_count++], "host.previous");
      strcpy(host->actions[host->action_count++], "host.next");
    }
  }
  if (!wifi_news.count) {
    const bool retrying = pet_wifi_data_retrying(PET_WIFI_DATA_NEWS);
    view->eyebrow[0] = 0;
    snprintf(view->headline, sizeof(view->headline), "%s",
      retrying ? "资讯暂时获取失败，稍后重试" : "正在通过设备 Wi‑Fi 获取资讯");
    view->note[0] = 0;
    view->footer[0] = 0;
    view->badge[0] = 0;
    view->metric_value[0] = 0;
    view->metric_unit[0] = 0;
    snprintf(view->metric_label, sizeof(view->metric_label), "%s", "刷新头条");
    view->revision ^= retrying ? 0x30000000U : 0x10000000U;
    return;
  }
  wifi_news_sync_index();
  const pet_p4_data_row_t *row = &wifi_news.rows[wifi_news_index];
  snprintf(view->eyebrow, sizeof(view->eyebrow), "%s", "Hacker News · 科技头条");
  snprintf(view->headline, sizeof(view->headline), "%s%s%s", row->label, row->value, row->detail);
  snprintf(view->note, sizeof(view->note), "%s", row->meta);
  if (wifi_news.restored)
    snprintf(view->footer, sizeof(view->footer), "离线缓存（已过期） · HN #%s", row->id);
  else
    snprintf(view->footer, sizeof(view->footer), "缓存 %llu 分钟%s · HN #%s",
      (unsigned long long)(wifi_news.age_ms / 60000), wifi_news.stale ? "（已过期）" : "", row->id);
  snprintf(view->badge, sizeof(view->badge), "%u/%u", wifi_news_index + 1, wifi_news.count);
  snprintf(view->metric_label, sizeof(view->metric_label), "%s", "刷新头条");
  snprintf(view->metric_unit, sizeof(view->metric_unit), "%s", "仅文字");
  view->metric_value[0] = 0;
  view->revision ^= (uint32_t)(now - wifi_news.age_ms) ^ ((uint32_t)wifi_news_index << 27)
    ^ ((uint32_t)(wifi_news.age_ms / 60000) << 16) ^ (wifi_news.stale ? 0x80000000U : 0);
}
#endif
