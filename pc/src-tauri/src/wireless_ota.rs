//! Transport-independent Wi-Fi 6 OTA client. The authenticated LAN driver
//! must implement these operations on one USB-verified paired connection.
use sha2::{Digest, Sha256};

pub(crate) type Hash = [u8; 32];
pub(crate) type TransferId = [u8; 16];
const CHUNK_BYTES: usize = 4096;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Boot {
    pub id: String,
    pub partition: u32,
    pub digest: Hash,
    pub valid: bool,
}

pub(crate) struct Offer<'a> {
    pub id: TransferId,
    pub prior: &'a Boot,
    pub bytes: u32,
    pub sha256: Hash,
    pub installed_digest: Hash,
    pub revision_min: u16,
    pub revision_max: u16,
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Receipt {
    pub partition: u32,
    pub installed_digest: Hash,
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Error {
    Image,
    Device,
    Rejected(String),
    Unknown(String),
}

pub(crate) trait TrustedOtaLink {
    /// Identity from the authenticated peer, not from discovery or an OTA reply.
    fn board_id(&self) -> &str;
    fn query_boot(&mut self) -> Result<Boot, Error>;
    fn begin(&mut self, offer: &Offer<'_>) -> Result<(), Error>;
    fn chunk(&mut self, id: TransferId, sequence: u32, bytes: &[u8]) -> Result<(), Error>;
    fn commit(&mut self, id: TransferId) -> Result<Receipt, Error>;
    /// Close the original connection. Never send a guessed abort or reconnect
    /// transparently for a mutation.
    fn close(&mut self);
}

pub(crate) struct VerifiedImage<'a> {
    bytes: &'a [u8],
    sha256: Hash,
    installed_digest: Hash,
    revision_min: u16,
    revision_max: u16,
}

impl<'a> VerifiedImage<'a> {
    /// The digests come from the trusted Wi-Fi 6 release-image selection.
    /// The revision is the selected board's authenticated chip revision.
    pub fn new(
        bytes: &'a [u8],
        sha256: Hash,
        installed_digest: Hash,
        revision: u16,
    ) -> Result<Self, Error> {
        if bytes.len() < 24
            || bytes.len() > u32::MAX as usize
            || bytes[0] != 0xe9
            || bytes[12..14] != [18, 0]
            || installed_digest == [0; 32]
            || <Hash>::from(Sha256::digest(bytes)) != sha256
        {
            return Err(Error::Image);
        }
        let revision_min = u16::from_le_bytes([bytes[15], bytes[16]]);
        let revision_max = u16::from_le_bytes([bytes[17], bytes[18]]);
        if !(revision_min..=revision_max).contains(&revision)
            || !((revision_max < 200) || (revision_min >= 300 && revision_max < 400))
        {
            return Err(Error::Image);
        }
        Ok(Self {
            bytes,
            sha256,
            installed_digest,
            revision_min,
            revision_max,
        })
    }
}

pub(crate) struct Pending {
    board_id: String,
    prior: Boot,
    receipt: Receipt,
}

impl Pending {
    pub fn rolled_back(&self, authenticated_board_id: &str, boot: &Boot) -> bool {
        authenticated_board_id == self.board_id
            && !boot.id.is_empty()
            && boot.id != self.prior.id
            && boot.valid
            && boot.partition == self.prior.partition
            && boot.digest == self.prior.digest
    }

    /// A commit receipt proves the requested reboot was scheduled. Only a new
    /// healthy boot of the exact verified image completes the update.
    pub fn validated(&self, authenticated_board_id: &str, boot: &Boot) -> bool {
        authenticated_board_id == self.board_id
            && !boot.id.is_empty()
            && boot.id != self.prior.id
            && boot.valid
            && boot.partition == self.receipt.partition
            && boot.digest == self.receipt.installed_digest
    }
}

pub(crate) fn install(
    link: &mut impl TrustedOtaLink,
    board_id: &str,
    id: TransferId,
    image: VerifiedImage<'_>,
    mut report_progress: impl FnMut(&'static str, u8),
) -> Result<Pending, Error> {
    if board_id.is_empty() || link.board_id() != board_id || id == [0; 16] {
        return Err(Error::Device);
    }
    let prior = link.query_boot()?;
    if !prior.valid || prior.id.is_empty() || prior.digest == [0; 32] {
        return Err(Error::Device);
    }
    let result = (|| {
        link.begin(&Offer {
            id,
            prior: &prior,
            bytes: image.bytes.len() as u32,
            sha256: image.sha256,
            installed_digest: image.installed_digest,
            revision_min: image.revision_min,
            revision_max: image.revision_max,
        })?;
        let mut written = 0_u64;
        let mut previous_percent = 0;
        report_progress("upload", 0);
        for (sequence, chunk) in image.bytes.chunks(CHUNK_BYTES).enumerate() {
            link.chunk(id, sequence as u32, chunk)?;
            written += chunk.len() as u64;
            let percent = (written * 100 / image.bytes.len() as u64) as u8;
            if percent != previous_percent {
                report_progress("upload", percent);
                previous_percent = percent;
            }
        }
        report_progress("verify", 100);
        let receipt = link.commit(id)?;
        if receipt.partition == prior.partition
            || receipt.installed_digest != image.installed_digest
        {
            return Err(Error::Unknown(
                "OTA receipt does not match the selected image".into(),
            ));
        }
        Ok(Pending {
            board_id: board_id.into(),
            prior,
            receipt,
        })
    })();
    link.close();
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Wire {
        events: Vec<&'static str>,
        failure: Option<&'static str>,
    }

    impl Wire {
        fn event(&mut self, event: &'static str) -> Result<(), Error> {
            self.events.push(event);
            if self.failure == Some(event) {
                return Err(Error::Unknown("response was lost".into()));
            }
            if event == "begin" && self.failure == Some("reject") {
                return Err(Error::Rejected("prior boot changed".into()));
            }
            Ok(())
        }
    }

    impl TrustedOtaLink for Wire {
        fn board_id(&self) -> &str {
            "test-board"
        }
        fn query_boot(&mut self) -> Result<Boot, Error> {
            self.event("boot")?;
            Ok(Boot {
                id: "first".into(),
                partition: 1,
                digest: [4; 32],
                valid: true,
            })
        }
        fn begin(&mut self, offer: &Offer<'_>) -> Result<(), Error> {
            assert_eq!(
                (offer.id, offer.prior.digest, offer.bytes),
                ([1; 16], [4; 32], 4100)
            );
            assert_eq!((offer.revision_min, offer.revision_max), (300, 399));
            assert_eq!(offer.installed_digest, [7; 32]);
            assert_eq!(offer.sha256, <Hash>::from(Sha256::digest(image())));
            self.event("begin")
        }
        fn chunk(&mut self, id: TransferId, sequence: u32, bytes: &[u8]) -> Result<(), Error> {
            assert_eq!(id, [1; 16]);
            assert_eq!(bytes.len(), if sequence == 0 { CHUNK_BYTES } else { 4 });
            self.event("chunk")
        }
        fn commit(&mut self, _: TransferId) -> Result<Receipt, Error> {
            self.event("commit")?;
            Ok(Receipt {
                partition: 2,
                installed_digest: [7; 32],
            })
        }
        fn close(&mut self) {
            self.events.push("close");
        }
    }

    fn image() -> Vec<u8> {
        let mut bytes = vec![0; 4100];
        bytes[0] = 0xe9;
        bytes[12] = 18;
        bytes[15..17].copy_from_slice(&300u16.to_le_bytes());
        bytes[17..19].copy_from_slice(&399u16.to_le_bytes());
        bytes
    }

    #[test]
    fn checks_exact_bytes_and_revision() {
        let bytes = image();
        let sha = Sha256::digest(&bytes).into();
        assert!(VerifiedImage::new(&bytes, [0; 32], [7; 32], 302).is_err());
        assert!(VerifiedImage::new(&bytes, sha, [7; 32], 100).is_err());
    }

    #[test]
    fn mutation_response_loss_never_replays() {
        let bytes = image();
        for (phase, events) in [
            ("begin", vec!["boot", "begin", "close"]),
            ("chunk", vec!["boot", "begin", "chunk", "close"]),
            (
                "commit",
                vec!["boot", "begin", "chunk", "chunk", "commit", "close"],
            ),
        ] {
            let mut link = Wire {
                events: vec![],
                failure: Some(phase),
            };
            let image =
                VerifiedImage::new(&bytes, Sha256::digest(&bytes).into(), [7; 32], 302).unwrap();
            assert!(matches!(
                install(&mut link, "test-board", [1; 16], image, |_, _| {}),
                Err(Error::Unknown(_))
            ));
            assert_eq!(link.events, events);
        }
    }

    #[test]
    fn explicit_begin_rejection_does_not_send_data() {
        let bytes = image();
        let mut link = Wire {
            events: vec![],
            failure: Some("reject"),
        };
        let image =
            VerifiedImage::new(&bytes, Sha256::digest(&bytes).into(), [7; 32], 302).unwrap();
        assert!(matches!(
            install(&mut link, "test-board", [1; 16], image, |_, _| {}),
            Err(Error::Rejected(_))
        ));
        assert_eq!(link.events, ["boot", "begin", "close"]);
    }

    #[test]
    fn only_a_new_valid_boot_of_the_exact_image_completes() {
        let bytes = image();
        let mut link = Wire {
            events: vec![],
            failure: None,
        };
        let image =
            VerifiedImage::new(&bytes, Sha256::digest(&bytes).into(), [7; 32], 302).unwrap();
        let mut progress = Vec::new();
        let update = install(&mut link, "test-board", [1; 16], image, |stage, percent| {
            progress.push((stage, percent));
        })
        .unwrap();
        assert_eq!(
            progress,
            [
                ("upload", 0),
                ("upload", 99),
                ("upload", 100),
                ("verify", 100)
            ]
        );
        let mut boot = Boot {
            id: "second".into(),
            partition: 2,
            digest: [7; 32],
            valid: false,
        };
        assert!(!update.validated("test-board", &boot));
        boot.valid = true;
        assert!(update.validated("test-board", &boot));
        assert!(!update.validated("different-board", &boot));
        boot.digest[0] ^= 1;
        assert!(!update.validated("test-board", &boot));
        assert!(!update.validated("test-board", &update.prior));
        assert!(!update.rolled_back("test-board", &update.prior));
        let mut restored = update.prior.clone();
        restored.id = "third".into();
        assert!(!update.rolled_back("different-board", &restored));
        assert!(update.rolled_back("test-board", &restored));
        restored.digest[0] ^= 1;
        assert!(!update.rolled_back("test-board", &restored));
    }
}
