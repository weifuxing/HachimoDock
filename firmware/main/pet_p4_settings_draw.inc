/* Shared 640 x 480 logical surface. Password text never enters the view. */
static void render_settings_page(const pet_settings_view_t *settings) {
  const uint16_t paper = rgb565(247, 240, 230);
  const uint16_t panel = rgb565(255, 251, 244);
  const uint16_t ink = rgb565(55, 43, 35);
  const uint16_t muted = rgb565(112, 90, 74);
  const uint16_t outline = rgb565(226, 208, 187);
  const uint16_t accent = rgb565(159, 74, 35);
  const uint16_t faint = rgb565(247, 222, 198);
  const uint16_t green = rgb565(48, 113, 73);

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, paper);
  fill_round_rect(24, 16, 92, 54, 13, faint);
  draw_text_center("返回", 70, 33, 80, ink, 1);

  if (settings->screen == PET_SETTINGS_HOME) {
    draw_text_line_vcenter("系统设置", 139, 16, 54, 450, ink, 2, true);
    static const char *const labels[] = {"WIFI 配置", "电脑配对", "时区设置", "蓝牙键盘"};
    const unsigned count = 4;
    const int step = pet_settings_home_step(settings);
    for (unsigned i = 0; i < count; ++i) {
      int y = 103 + step * (int)i;
      fill_round_rect_outline(24, y, 592, step - 16, 17, panel, settings->selected == i ? accent : outline);
      draw_text_line_vcenter(labels[i], 42, y, step - 16, 502, ink, 2, true);
      draw_text_center(">", 587, y + (step - 16) / 3, 27, accent, 2);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_BLE) {
    draw_text_line_vcenter("蓝牙键盘",139,16,54,450,ink,2,true);
    const char *message=settings->ble_available<0?"蓝牙暂不可用":
      !settings->ble_available?"正在准备蓝牙":settings->ble_paired?"已配对":
      settings->ble_pairing?"在电脑蓝牙设置中选 HachimiDock":"选择开始配对";
    draw_text_line_vcenter(message,24,98,72,592,ink,2,true);
    if (settings->ble_code) {
      char code[10];
      snprintf(code,sizeof(code),"%06u",settings->ble_code);
      draw_text_center(code,320,235,590,accent,2);
    }
    if (settings->ble_available>0 && !settings->ble_pairing) {
      fill_round_rect_outline(24,392,592,60,13,faint,outline);
      draw_text_center(settings->ble_paired?"解除配对":"开始配对",320,412,540,ink,1);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_TIMEZONE) {
    const bool automatic = settings->timezone_draft == PET_WIFI_TZ_AUTO;
    const int offset = automatic ? settings->timezone_effective : settings->timezone_draft;
    const int magnitude = offset < 0 ? -offset : offset;
    char utc[16];
    snprintf(utc, sizeof(utc), "UTC%c%02d:%02d", offset < 0 ? '-' : '+',
             magnitude / 60, magnitude % 60);
    draw_text_line_vcenter("时区设置", 139, 16, 54, 450, ink, 2, true);
    draw_text_center(automatic ? "跟随电脑（默认 UTC）" : "固定时区", 320, 105, 592, muted, 1);
    fill_round_rect_outline(24, 160, 106, 72, 13, panel, outline);
    fill_round_rect_outline(510, 160, 106, 72, 13, panel, outline);
    draw_text_center("-", 77, 181, 96, accent, 2);
    draw_text_center("+", 563, 181, 96, accent, 2);
    draw_text_center(utc, 320, 181, 360, ink, 2);
    fill_round_rect_outline(24, 260, 592, 70, 13, panel, outline);
    draw_text_center(automatic ? "使用固定时区" : "跟随电脑", 320, 284, 540, ink, 1);
    fill_round_rect_outline(24, 392, 592, 60, 13, faint, accent);
    draw_text_center("保存", 320, 412, 540, ink, 1);
    return;
  }

  if (settings->screen == PET_SETTINGS_LAN) {
    const pet_lan_pair_view_t *lan = &settings->lan;
    draw_text_line_vcenter("电脑配对", 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(lan->busy ? "正在配对" : lan->paired ? "已配对" :
      !lan->ip[0] ? "请先连接 WIFI" : lan->ready ? "等待电脑配对" : "配对状态暂不可用",
      25, 81, 46, 590, lan->paired ? green : muted, 1, true);
    if (lan->ip[0]) draw_text_line_vcenter(lan->ip, 25, 131, 45, 590, ink, 2, true);
    draw_text_line_vcenter(lan->paired ? "配对已保存，电脑可自动连接" : "在电脑设置中发现并选择本设备",
      25, 195, 40, 590, muted, 1, true);
    draw_text_line_vcenter(lan->paired ? "更换电脑前，请先解除配对" : "电脑确认后即可完成，无需设备操作",
      25, 245, 40, 590, muted, 1, true);
    if (lan->failed && !lan->paired)
      draw_text_line_vcenter("配对未完成，请在电脑重试",25,295,40,590,accent,1,true);
    if (lan->paired && !lan->busy) {
      fill_round_rect_outline(24, 392, 592, 60, 13, faint, outline);
      draw_text_center("解除配对", 320, 412, 540, ink, 1);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_FEED) {
    draw_text_line_vcenter("连接服务账号", 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(settings->feed_host, 25, 90, 48, 590, ink, 1, true);
    draw_text_line_vcenter(settings->feed_target, 25, 144, 40, 590, muted, 1, true);
    const char *status = !settings->feed_can_save ? "设备安全存储尚未配置" :
      settings->request_busy ? "操作失败，请重试" :
      settings->feed_result == PET_PRIVATE_BUSY ? "正在保存" :
      settings->feed_result == PET_PRIVATE_SET ? "已保存" :
      settings->feed_result == PET_PRIVATE_ABSENT ? "已清除" :
      settings->feed_result == PET_PRIVATE_STORAGE_ERROR ? "凭证存储已满或不可用" :
      "授权后在此组件查看私人数据";
    draw_text_line_vcenter(status, 25, 200, 44, 590, accent, 1, true);
    if (settings->feed_can_save) {
      static const char *const actions[] = {"连接账号", "移除授权"};
      for (unsigned i = 0; i < 2; ++i) {
        int y = 280 + 70 * (int)i;
        fill_round_rect_outline(24, y, 592, 58, 13, panel, settings->selected == i ? accent : outline);
        draw_text_center(actions[i], 320, y + 20, 540, ink, 1);
      }
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_PASSWORD || settings->screen == PET_SETTINGS_FEED_ENTRY) {
    const bool feed = settings->screen == PET_SETTINGS_FEED_ENTRY;
    const char *label = !feed ? "输入密码" : settings->feed_username ? "服务账号" :
        settings->feed_kind == PET_PRIVATE_MQTT ? "服务密码" : "输入 Token";
    draw_text_line_vcenter(label, 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(feed ? settings->feed_host : settings->target.ssid, 27, 71, 29, 586, muted, 1, true);
    fill_round_rect_outline(24, 102, 592, 43, 12, panel, outline);
    if (settings->password_length) {
      char masked[35];
      unsigned length = settings->password_length;
      if (length >= sizeof(masked)) length = sizeof(masked) - 1;
      memset(masked, '*', length);
      masked[length] = 0;
      draw_text_line_vcenter(masked, 39, 102, 43, 455, ink, 1, true);
    } else draw_text_line_vcenter(label, 39, 102, 43, 455, muted, 1, true);
    if (settings->short_password)
      draw_text_line_vcenter(feed ? "请核对后重试" : "至少 8 位", 455, 102, 43, 140, accent, 1, true);
    else if (settings->request_busy)
      draw_text_line_vcenter("请稍候", 496, 102, 43, 100, accent, 1, true);
    for (unsigned row = 0; row < PET_SETTINGS_KEY_ROWS; ++row) {
      unsigned columns = pet_settings_key_columns(row);
      int width = PET_SETTINGS_KEY_WIDTH / (int)columns;
      for (unsigned col = 0; col < columns; ++col) {
        int x = PET_SETTINGS_KEY_LEFT + (int)col * width + PET_SETTINGS_KEY_GAP / 2;
        int y = PET_SETTINGS_KEY_TOP + (int)row * PET_SETTINGS_KEY_ROW_HEIGHT;
        bool focused = row == settings->key_row && col == settings->key_column;
        bool connect = row == 4 && col == 4;
        bool mode_selected = row == 4 && ((col == 0 && settings->keyboard == 1) ||
                                             (col == 1 && settings->keyboard == 2));
        int key_height = PET_SETTINGS_KEY_ROW_HEIGHT - PET_SETTINGS_KEY_GAP;
        fill_round_rect_outline(x, y, width - PET_SETTINGS_KEY_GAP, key_height, 10,
          connect ? accent : (focused || mode_selected) ? faint : panel,
          (focused || mode_selected) ? accent : outline);
        char ascii[2];
        const char *key = feed && connect ? (settings->feed_username ? "下一步" : "保存") :
          pet_settings_key_label(settings->keyboard, row, col, ascii);
        draw_text_center(key, x + (width - PET_SETTINGS_KEY_GAP) / 2, y + (row == 4 ? 20 : 14),
                         width - PET_SETTINGS_KEY_GAP - 4, connect ? panel : ink, row == 4 ? 1 : 2);
      }
    }
    return;
  }

  draw_text_line_vcenter("WIFI配置", 139, 16, 54, 450, ink, 2, true);
  const pet_wifi_snapshot_t *wifi = &settings->wifi;
  const char *status = "请选择网络";
  if (settings->request_busy) status = "请稍候";
  else if (settings->unsupported) status = "此网络暂不支持";
  else if (wifi->status == PET_WIFI_UNAVAILABLE) status = "无线模块不可用";
  else if (wifi->status == PET_WIFI_SCANNING) status = "正在扫描";
  else if (wifi->status == PET_WIFI_CONNECTING) status = "正在连接";
  else if (wifi->status == PET_WIFI_FAILED) {
    switch (wifi->join_reason) {
      case 201: case 212: status = "未找到热点，请重新扫描"; break;
      case 15: case 204: status = "热点握手超时"; break;
      case 202: status = "热点认证未完成"; break;
      case 210: case 211: status = "热点加密方式不兼容"; break;
      default: status = "操作失败，请重试"; break;
    }
  }
  else if (wifi->connected[0]) status = "已连接";
  else if (!wifi->count) status = "未找到网络，请重新扫描";
  draw_text_line_vcenter(status, 26, 74, 40, 307, wifi->connected[0] ? green : muted, 1, true);
  if (wifi->connected[0])
    draw_text_right(wifi->connected, 612, 85, 284, green, 1);
  for (unsigned row = 0; row < PET_SETTINGS_WIFI_ROWS; ++row) {
    const unsigned index = settings->list_start + row;
    if (index >= wifi->count) break;
    const pet_wifi_ap_t *ap = &wifi->aps[index];
    const bool focused = index == settings->selected;
    const int y = 126 + (int)row * 64;
    fill_round_rect_outline(24, y, 592, 58, 13, focused ? faint : panel, focused ? accent : outline);
    draw_text_line_vcenter(ap->ssid, 43, y, 58, 420, ink, 1, true);
    draw_text_line_vcenter(ap->security == PET_WIFI_OPEN ? "开放" :
      ap->security == PET_WIFI_UNSUPPORTED ? "不支持" : "加密", 484, y, 58, 68, muted, 1, true);
    unsigned strength = ap->rssi >= -55 ? 4 : ap->rssi >= -68 ? 3 : ap->rssi >= -80 ? 2 : 1;
    for (unsigned bar = 0; bar < 4; ++bar)
      fill_rect(560 + (int)bar * 9, y + 39 - (int)bar * 5, 5, 5 + (int)bar * 5,
                bar < strength ? accent : outline);
  }
  fill_round_rect_outline(24, 395, 186, 60, 13,
    settings->selected == wifi->count ? faint : panel, outline);
  draw_text_center("重新扫描", 117, 416, 164, ink, 1);
  if (wifi->saved) {
    fill_round_rect_outline(230, 395, 185, 60, 13,
      settings->selected > wifi->count ? faint : panel, outline);
    draw_text_center("忘记网络", 322, 416, 164, ink, 1);
  }
  if (wifi->count > PET_SETTINGS_WIFI_ROWS) {
    const unsigned current_page = settings->list_start / PET_SETTINGS_WIFI_ROWS + 1;
    const unsigned pages = (wifi->count + PET_SETTINGS_WIFI_ROWS - 1) / PET_SETTINGS_WIFI_ROWS;
    char navigation[48];
    snprintf(navigation, sizeof(navigation), "%s %u/%u",
             current_page < pages ? "下一页" : "回到第1页", current_page, pages);
    fill_round_rect_outline(435, 395, 181, 60, 13, panel, outline);
    draw_text_center(navigation, 525, 416, 163, ink, 1);
  } else if (wifi->count) {
    char count[32];
    snprintf(count, sizeof(count), "共 %u 个网络", (unsigned)wifi->count);
    draw_text_center(count, 525, 416, 163, muted, 1);
  }
}
