/* [Input] Device capabilities/diagnostics and ESP image headers.
 * [Output] Chip-family selection and fail-closed v1/v3 OTA compatibility.
 * [Pos] Pure firmware policy, independent of UI and host OS.
 * [Sync] Update pc/.folder.md when changing this contract. */
use serde_json::Value;

pub(crate) const BOARD_MODEL_REQUIRED: &str = "firmware_board_selection_required";

pub(crate) const V1_FIRMWARE_RESOURCE: &str = "firmware/esp32-p4/firmware.bin";
pub(crate) const V3_FIRMWARE_RESOURCE: &str = "firmware/esp32-p4/firmware-v3.bin";

const PICO_V1_FIRMWARE_RESOURCE: &str = "firmware/esp32-p4/firmware-pico-v1.bin";
const PICO_V3_FIRMWARE_RESOURCE: &str = "firmware/esp32-p4/firmware-pico-v3.bin";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum P4BoardModel { Wifi6, Pico }

impl P4BoardModel {
    fn parse(value: &str) -> Result<Self, String> {
        match value {
            "wifi6" => Ok(Self::Wifi6),
            "pico" => Ok(Self::Pico),
            _ => Err("设备板型无效".into()),
        }
    }

    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Wifi6 => "wifi6",
            Self::Pico => "pico",
        }
    }

    fn from_image_version(version: &str) -> Option<Self> {
        match version.rsplit_once('-')?.1 {
            "wifi6" => Some(Self::Wifi6),
            "pico" => Some(Self::Pico),
            _ => None,
        }
    }

    pub(crate) fn image_version_matches(self, version: &str) -> bool {
        Self::from_image_version(version) == Some(self)
    }
}

pub(crate) fn board_model_from_device(
    capabilities: &Value,
    confirmed: Option<&str>,
) -> Result<P4BoardModel, String> {
    // boardModel and the running image version describe the installed build.
    // Only the device-bound, factory-provisioned record identifies the hardware.
    let detected = capabilities.pointer("/firmwareUpdate/hardwareBoardModel")
        .map(|value| P4BoardModel::parse(value.as_str().ok_or("设备板型记录无效")?))
        .transpose()?;
    let confirmed = confirmed.map(P4BoardModel::parse).transpose()?;
    if let (Some(detected), Some(confirmed)) = (detected, confirmed) {
        if detected != confirmed {
            return Err("所选板型与设备的出厂板型记录不一致".into());
        }
    }
    detected.or(confirmed).ok_or_else(|| BOARD_MODEL_REQUIRED.into())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum P4ChipTarget {
    Revision(u16),
    // Pre-v3 ESP-IDF runtime diagnostics prove the family, not the exact revision.
    LegacyV1,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct FirmwareChipRange {
    pub min: u16,
    pub max: u16,
}

fn supported_revision(revision: u16) -> bool {
    revision < 200 || (300..400).contains(&revision)
}

pub(crate) fn firmware_chip_range(image: &[u8]) -> Result<FirmwareChipRange, String> {
    if image.len() < 24 || image[0] != 0xe9 || image[12..14] != [18, 0] {
        return Err("固件不是有效的 ESP32-P4 应用镜像".into());
    }
    let min = u16::from_le_bytes([image[15], image[16]]);
    let max = u16::from_le_bytes([image[17], image[18]]);
    if min > max || !((min < 200 && max < 200) || (min >= 300 && max < 400)) {
        return Err("固件未明确限定 v1/v3 芯片范围，已阻止升级，请使用匹配的新版固件".into());
    }
    Ok(FirmwareChipRange { min, max })
}

pub(crate) fn chip_target_from_device(
    capabilities: &Value,
    diagnostics: Option<&Value>,
) -> Result<P4ChipTarget, String> {
    let revision = capabilities
        .pointer("/firmwareUpdate/chipRevision")
        .or_else(|| diagnostics.and_then(|d| d.pointer("/runtime/chipRevision")));
    if let Some(value) = revision {
        let revision = value
            .as_u64()
            .and_then(|v| u16::try_from(v).ok())
            .filter(|v| supported_revision(*v))
            .ok_or("设备报告的 P4 芯片版本无效或不受支持")?;
        return Ok(P4ChipTarget::Revision(revision));
    }
    if let Some(runtime) = diagnostics.and_then(|d| d.get("runtime")) {
        let idf = runtime
            .get("idfVersion")
            .and_then(Value::as_str)
            .unwrap_or("");
        let parts: Vec<_> = idf
            .trim_start_matches('v')
            .split('.')
            .take(3)
            .map(|part| part.parse::<u32>())
            .collect();
        // These SDK lines predate v3 support. Do not infer from board SKU, baud,
        // app version, or a missing revision alone.
        let legacy_sdk = matches!(parts.as_slice(), [Ok(5), Ok(3 | 4), Ok(_)])
            || matches!(parts.as_slice(), [Ok(5), Ok(5), Ok(0..=2)]);
        if legacy_sdk
            && runtime.get("projectName").and_then(Value::as_str) == Some("pet_manager_p4_runtime")
        {
            return Ok(P4ChipTarget::LegacyV1);
        }
    }
    Err("无法确认设备芯片版本，已阻止自动选择固件；请先使用匹配芯片的完整烧录包".into())
}

pub(crate) fn bundled_resource_for_device(board: P4BoardModel, target: P4ChipTarget) -> &'static str {
    match (board, target) {
        (P4BoardModel::Pico, P4ChipTarget::Revision(300..=399)) => PICO_V3_FIRMWARE_RESOURCE,
        (P4BoardModel::Wifi6, P4ChipTarget::Revision(300..=399)) => V3_FIRMWARE_RESOURCE,
        (P4BoardModel::Pico, _) => PICO_V1_FIRMWARE_RESOURCE,
        (P4BoardModel::Wifi6, _) => V1_FIRMWARE_RESOURCE,
    }
}

pub(crate) fn validate_firmware_chip(
    range: FirmwareChipRange,
    target: P4ChipTarget,
) -> Result<(), String> {
    let compatible = match target {
        P4ChipTarget::Revision(revision) => revision >= range.min && revision <= range.max,
        P4ChipTarget::LegacyV1 => range.min <= 1 && range.max == 199,
    };
    if compatible {
        Ok(())
    } else {
        Err("固件与设备芯片版本不匹配，已阻止升级".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn board_selection_uses_device_evidence_and_combines_with_chip_revision() {
        let pico = json!({"firmwareUpdate":{"boardModel":"wifi6","hardwareBoardModel":"pico"}});
        let wifi = json!({"firmwareUpdate":{"boardModel":"pico","hardwareBoardModel":"wifi6"}});
        let legacy = json!({"firmwareUpdate":{"boardModel":"wifi6"}});
        assert_eq!(board_model_from_device(&legacy, None).unwrap_err(), BOARD_MODEL_REQUIRED);
        assert_eq!(board_model_from_device(&legacy, Some("pico")).unwrap(), P4BoardModel::Pico);
        assert!(board_model_from_device(&legacy, Some("other")).is_err());
        assert!(board_model_from_device(&pico, Some("wifi6")).is_err());
        let invalid = json!({"firmwareUpdate":{"hardwareBoardModel":"other"}});
        assert!(board_model_from_device(&invalid, Some("wifi6")).is_err());
        for (cap, revision, expected) in [
            (&pico, 100, PICO_V1_FIRMWARE_RESOURCE), (&pico, 302, PICO_V3_FIRMWARE_RESOURCE),
            (&wifi, 100, V1_FIRMWARE_RESOURCE), (&wifi, 302, V3_FIRMWARE_RESOURCE),
        ] {
            let board = board_model_from_device(cap, None).unwrap();
            assert_eq!(bundled_resource_for_device(board, P4ChipTarget::Revision(revision)), expected);
        }
    }
    fn image(min: u16, max: u16) -> Vec<u8> {
        let mut image = vec![0; 24];
        image[0] = 0xe9;
        image[12] = 18;
        image[15..17].copy_from_slice(&min.to_le_bytes());
        image[17..19].copy_from_slice(&max.to_le_bytes());
        image
    }
    #[test]
    fn chip_family_matrix_rejects_cross_flash_and_unbounded_images() {
        for (min, max) in [(0, 65535), (1, 399), (300, 199), (200, 299)] {
            assert!(firmware_chip_range(&image(min, max)).is_err());
        }
        for revision in [1, 100, 103, 300, 301, 302, 399] {
            for (min, max) in [(1, 199), (300, 399)] {
                let range = firmware_chip_range(&image(min, max)).unwrap();
                assert_eq!(
                    validate_firmware_chip(range, P4ChipTarget::Revision(revision)).is_ok(),
                    revision >= min && revision <= max
                );
            }
        }
        assert!(firmware_chip_range(&image(1, 199)[..23]).is_err());
        let mut wrong = image(1, 199);
        wrong[12] = 9;
        assert!(firmware_chip_range(&wrong).is_err());
    }
    #[test]
    fn unknown_revisions_are_not_guessed_from_sku_or_version() {
        for value in [
            json!("301"),
            json!(-1),
            json!(301.5),
            json!(200),
            json!(400),
            Value::Null,
        ] {
            assert!(chip_target_from_device(
                &json!({"firmwareUpdate":{"chipRevision":value}}),
                None
            )
            .is_err());
        }
        assert!(chip_target_from_device(&json!({"deviceModel":"ESP32-P4-WIFI6-M"}), None).is_err());
        for idf in ["5.5.3", "5.5.4", "6.0.0", "", "garbage"] {
            assert!(chip_target_from_device(
                &json!({}),
                Some(&json!({"runtime":{
                "idfVersion":idf,"projectName":"pet_manager_p4_runtime"}}))
            )
            .is_err());
        }
        let legacy = chip_target_from_device(
            &json!({}),
            Some(&json!({"runtime":{
            "idfVersion":"5.5.1.250929","projectName":"pet_manager_p4_runtime"}})),
        )
        .unwrap();
        assert_eq!(legacy, P4ChipTarget::LegacyV1);
        assert!(validate_firmware_chip(FirmwareChipRange { min: 300, max: 399 }, legacy).is_err());
        assert!(validate_firmware_chip(FirmwareChipRange { min: 1, max: 199 }, legacy).is_ok());
        assert_eq!(bundled_resource_for_device(P4BoardModel::Wifi6, legacy), V1_FIRMWARE_RESOURCE);
        assert_eq!(
            bundled_resource_for_device(P4BoardModel::Wifi6, P4ChipTarget::Revision(302)),
            V3_FIRMWARE_RESOURCE
        );
    }
}
