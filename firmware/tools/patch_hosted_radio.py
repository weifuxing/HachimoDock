"""Generate the pinned ESP-Hosted P4 transport with C5/C6 SDIO admission."""
from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path

TRANSPORT_SHA = "f2291db3898b93d0afbc43656af559435d0401bf72e9212b797ab304d9e93dec"


def patch_transport(raw: bytes) -> str:
    if sha256(raw).hexdigest() != TRANSPORT_SHA:
        raise ValueError("Review C5/C6 compatibility against this ESP-Hosted source")
    source = raw.decode()
    start = source.index("static void verify_host_config_for_slave(")
    end = source.index("/** return values:", start)
    source = source[:start] + """static bool verify_host_config_for_slave(uint8_t chip_type)
{
#if H_TRANSPORT_IN_USE != H_TRANSPORT_SDIO || !H_SLAVE_TARGET_ESP32C6
#error "The common C5/C6 profile requires the existing P4 C6 SDIO build"
#endif
    if (chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C5 &&
        chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C6) {
        ESP_LOGE(TAG, "Unsupported P4 SDIO coprocessor: %u", chip_type);
        return false;
    }
    char name[20] = {0};
    get_chip_str_from_id(chip_type, name);
    ESP_LOGI(TAG, "Identified coprocessor %s", name);
    check_if_max_freq_used(chip_type);
    return true;
}

""" + source[end:]
    source = source.replace(
        "static char chip_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;",
        """#include <stdatomic.h>
static atomic_uint chip_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;
unsigned pet_p4_hosted_radio_chip_id(void)
{
    return atomic_load(&chip_type);
}""",
        1,
    )
    source = source.replace(
        "if (!evt_buf || !len)\n\t\treturn;",
        "if (!evt_buf || len < offsetof(struct esp_priv_event, event_data))\n\t\treturn;",
        1,
    )
    source = source.replace(
        "event = (struct esp_priv_event *) evt_buf;",
        """event = (struct esp_priv_event *) evt_buf;
    if (event->event_len > len - offsetof(struct esp_priv_event, event_data))
        return;""",
        1,
    )
    source = source.replace(
        "uint8_t len_left = len, tag_len;",
        """uint16_t len_left = len;
    uint8_t tag_len;
    uint8_t candidate_chip = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;""",
        1,
    )
    source = source.replace("if (!evt_buf)\n\t\treturn ESP_FAIL;",
                            "if (!evt_buf || len > 64)\n\t\treturn ESP_FAIL;", 1)
    start = source.index("\tif (len > 64)", source.index("uint8_t candidate_chip"))
    end = source.index("\n\t\tif (*pos == ESP_PRIV_CAPABILITY)", start)
    source = source[:start] + """    while (len_left) {
        if (len_left < 2) return ESP_ERR_INVALID_SIZE;
        tag_len = pos[1];
        if (tag_len > len_left - 2) return ESP_ERR_INVALID_SIZE;
        const unsigned need = (*pos == ESP_PRIV_FIRMWARE_VERSION ||
                               *pos == ESP_PRIV_CAP_EXT) ? 4 : 1;
        if (tag_len < need) return ESP_ERR_INVALID_SIZE;
""" + source[end:]
    source = source.replace(
        "chip_type = *(pos+2);\n\t\t\tverify_host_config_for_slave(chip_type);",
        """if (!verify_host_config_for_slave(pos[2]) ||
                (candidate_chip != ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED &&
                 candidate_chip != pos[2])) return ESP_ERR_NOT_SUPPORTED;
            candidate_chip = pos[2];""",
        1,
    )
    source = source.replace(
        'ESP_LOGE(TAG, "SDIO mode mismatch: slave is in streaming mode, but host is in packet mode. Aborting.");\n\t\t\t\tassert(0);',
        'ESP_LOGE(TAG, "SDIO mode mismatch: slave streaming, host packet");\n\t\t\t\treturn ESP_ERR_NOT_SUPPORTED;',
        1,
    )
    start = source.index("\tif ((chip_type !=")
    end = source.index("\n\tif (ext_cap)", start)
    source = source[:start] + """    if (candidate_chip == ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED)
        return ESP_ERR_NOT_SUPPORTED;
""" + source[end:]
    source = source.replace(
        "\ttransport_driver_event_handler(TRANSPORT_TX_ACTIVE);\n\n\tESP_ERROR_CHECK(send_slave_config(0, chip_type, raw_tp_config,",
        """    atomic_store(&chip_type, candidate_chip);
    transport_driver_event_handler(TRANSPORT_TX_ACTIVE);

    ESP_ERROR_CHECK(send_slave_config(0, candidate_chip, raw_tp_config,""",
        1,
    )
    return '#include <stddef.h>\n#line 1 "esp_hosted/transport_drv.c"\n' + source


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(patch_transport(args.source.read_bytes()))
