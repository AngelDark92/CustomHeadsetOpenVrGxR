use serde::Serialize;
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use zip::ZipArchive;

const PACKAGE: &str = "com.valvesoftware.steamlinkvr";
const BRIDGE_ENTRY: &str = "lib/arm64-v8a/libgxr_xr_bridge.so";
const CONTROL_PORT: i64 = 29981;
const TRACKING_PORT: i64 = 29982;
const NO_INDEX: u32 = u32::MAX;
const ANDROID_NAMESPACE: &str = "http://schemas.android.com/apk/res/android";
static SNAPSHOT_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GalaxyXrApkPreview {
    apk_path: String,
    file_name: String,
    package_name: String,
    version_code: i64,
    host: String,
    control_port: i64,
    tracking_port: i64,
    apk_sha256: String,
    bridge_sha256: String,
    pairing_token_fingerprint: String,
    signature_present: bool,
}

struct InspectedApk {
    preview: GalaxyXrApkPreview,
    pairing_token: String,
}

struct ApkSnapshot {
    path: PathBuf,
}

impl Drop for ApkSnapshot {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

fn snapshot_apk(path: &Path) -> Result<ApkSnapshot, String> {
    let mut source = File::open(path).map_err(|e| format!("open APK snapshot source: {e}"))?;
    let directory = std::env::temp_dir().join("GalaxyXR").join("ApkInspection");
    fs::create_dir_all(&directory).map_err(|e| format!("create APK snapshot directory: {e}"))?;
    for _ in 0..32 {
        let sequence = SNAPSHOT_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let candidate = directory.join(format!(
            "inspect-{}-{}-{}.apk",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_err(|_| "system clock precedes Unix epoch")?
                .as_nanos(),
            sequence
        ));
        let mut destination = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&candidate)
        {
            Ok(file) => file,
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(format!("create private APK snapshot: {error}")),
        };
        if let Err(error) =
            std::io::copy(&mut source, &mut destination).and_then(|_| destination.sync_all())
        {
            let _ = fs::remove_file(&candidate);
            return Err(format!(
                "copy APK into immutable inspection snapshot: {error}"
            ));
        }
        return Ok(ApkSnapshot { path: candidate });
    }
    Err("could not allocate a unique APK inspection snapshot".into())
}

fn u16_at(data: &[u8], offset: usize) -> Result<u16, String> {
    let bytes = data
        .get(offset..offset + 2)
        .ok_or("truncated Android binary XML")?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn u32_at(data: &[u8], offset: usize) -> Result<u32, String> {
    let bytes = data
        .get(offset..offset + 4)
        .ok_or("truncated Android binary XML")?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn utf8_length(data: &[u8], offset: &mut usize) -> Result<usize, String> {
    let first = *data.get(*offset).ok_or("truncated UTF-8 string length")?;
    *offset += 1;
    if first & 0x80 == 0 {
        Ok(first as usize)
    } else {
        let second = *data.get(*offset).ok_or("truncated UTF-8 string length")?;
        *offset += 1;
        Ok((((first & 0x7f) as usize) << 8) | second as usize)
    }
}

fn utf16_length(data: &[u8], offset: &mut usize) -> Result<usize, String> {
    let first = u16_at(data, *offset)?;
    *offset += 2;
    if first & 0x8000 == 0 {
        Ok(first as usize)
    } else {
        let second = u16_at(data, *offset)?;
        *offset += 2;
        Ok((((first & 0x7fff) as usize) << 16) | second as usize)
    }
}

fn parse_string_pool(data: &[u8], chunk: usize) -> Result<Vec<String>, String> {
    let header_size = u16_at(data, chunk + 2)? as usize;
    let chunk_size = u32_at(data, chunk + 4)? as usize;
    if header_size < 28 || chunk + chunk_size > data.len() {
        return Err("invalid Android string-pool chunk".into());
    }
    let count = u32_at(data, chunk + 8)? as usize;
    let flags = u32_at(data, chunk + 16)?;
    let strings_start = u32_at(data, chunk + 20)? as usize;
    let utf8 = flags & 0x100 != 0;
    let mut strings = Vec::with_capacity(count);
    for index in 0..count {
        let relative = u32_at(data, chunk + header_size + index * 4)? as usize;
        let mut cursor = chunk + strings_start + relative;
        if cursor >= chunk + chunk_size {
            return Err("Android string offset leaves its pool".into());
        }
        if utf8 {
            let _utf16_units = utf8_length(data, &mut cursor)?;
            let byte_count = utf8_length(data, &mut cursor)?;
            let bytes = data
                .get(cursor..cursor + byte_count)
                .ok_or("truncated UTF-8 Android string")?;
            strings.push(
                std::str::from_utf8(bytes)
                    .map_err(|_| "invalid UTF-8 Android string")?
                    .to_owned(),
            );
        } else {
            let units = utf16_length(data, &mut cursor)?;
            let byte_count = units
                .checked_mul(2)
                .ok_or("oversized UTF-16 Android string")?;
            let bytes = data
                .get(cursor..cursor + byte_count)
                .ok_or("truncated UTF-16 Android string")?;
            let words = bytes
                .chunks_exact(2)
                .map(|pair| u16::from_le_bytes([pair[0], pair[1]]));
            strings.push(
                String::from_utf16(&words.collect::<Vec<_>>())
                    .map_err(|_| "invalid UTF-16 Android string")?,
            );
        }
    }
    Ok(strings)
}

fn pool_string(pool: &[String], index: u32) -> Result<&str, String> {
    pool.get(index as usize)
        .map(String::as_str)
        .ok_or_else(|| format!("Android XML string index {index} is out of range"))
}

fn attribute_value(data: &[u8], attr: usize, pool: &[String]) -> Result<String, String> {
    let raw = u32_at(data, attr + 8)?;
    if raw != NO_INDEX {
        return Ok(pool_string(pool, raw)?.to_owned());
    }
    let value_type = *data.get(attr + 15).ok_or("truncated Android typed value")?;
    let value = u32_at(data, attr + 16)?;
    match value_type {
        0x03 => Ok(pool_string(pool, value)?.to_owned()),
        0x10 | 0x11 => Ok(value.to_string()),
        0x12 => Ok(if value == 0 { "false" } else { "true" }.to_owned()),
        _ => Err(format!(
            "unsupported Android manifest value type 0x{value_type:02x}"
        )),
    }
}

fn parse_manifest(data: &[u8]) -> Result<(String, HashMap<String, String>), String> {
    if u16_at(data, 0)? != 0x0003 || u16_at(data, 2)? < 8 {
        return Err("AndroidManifest.xml is not Android binary XML".into());
    }
    let declared_size = u32_at(data, 4)? as usize;
    if declared_size > data.len() || declared_size < 8 {
        return Err("invalid AndroidManifest.xml size".into());
    }
    let mut offset = u16_at(data, 2)? as usize;
    let mut pool: Option<Vec<String>> = None;
    let mut package = None;
    let mut metadata = HashMap::new();
    let mut depth = 0usize;
    let mut application_depth = None;
    while offset + 8 <= declared_size {
        let chunk_type = u16_at(data, offset)?;
        let header_size = u16_at(data, offset + 2)? as usize;
        let chunk_size = u32_at(data, offset + 4)? as usize;
        if header_size < 8 || chunk_size < header_size || offset + chunk_size > declared_size {
            return Err("invalid Android XML chunk bounds".into());
        }
        match chunk_type {
            0x0001 => pool = Some(parse_string_pool(data, offset)?),
            0x0102 => {
                let strings = pool
                    .as_ref()
                    .ok_or("Android XML element precedes string pool")?;
                let extension = offset + header_size;
                let element = pool_string(strings, u32_at(data, extension + 4)?)?;
                let attribute_start = u16_at(data, extension + 8)? as usize;
                let attribute_size = u16_at(data, extension + 10)? as usize;
                let attribute_count = u16_at(data, extension + 12)? as usize;
                if attribute_size < 20 {
                    return Err("invalid Android XML attribute size".into());
                }
                let attributes = extension + attribute_start;
                let mut values = HashMap::new();
                for index in 0..attribute_count {
                    let attr = attributes + index * attribute_size;
                    if attr + 20 > offset + chunk_size {
                        return Err("Android XML attribute leaves its chunk".into());
                    }
                    let namespace_index = u32_at(data, attr)?;
                    let namespace = if namespace_index == NO_INDEX {
                        ""
                    } else {
                        pool_string(strings, namespace_index)?
                    };
                    let name = pool_string(strings, u32_at(data, attr + 4)?)?;
                    values.insert(
                        format!("{namespace}\0{name}"),
                        attribute_value(data, attr, strings)?,
                    );
                }
                if element == "manifest" {
                    package = values.get("\0package").cloned();
                } else if element == "application" {
                    application_depth = Some(depth);
                } else if element == "meta-data" && application_depth == depth.checked_sub(1) {
                    let name_key = format!("{ANDROID_NAMESPACE}\0name");
                    let value_key = format!("{ANDROID_NAMESPACE}\0value");
                    if let (Some(name), Some(value)) =
                        (values.get(&name_key), values.get(&value_key))
                    {
                        metadata.insert(name.clone(), value.clone());
                    }
                }
                depth += 1;
            }
            0x0103 => {
                depth = depth.saturating_sub(1);
                if application_depth == Some(depth) {
                    application_depth = None;
                }
            }
            _ => {}
        }
        offset += chunk_size;
    }
    Ok((
        package.ok_or("Android manifest package is missing")?,
        metadata,
    ))
}

fn hex(bytes: &[u8]) -> String {
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write;
        let _ = write!(output, "{byte:02x}");
    }
    output
}

fn hash_reader(mut reader: impl Read) -> Result<String, String> {
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = reader
            .read(&mut buffer)
            .map_err(|e| format!("hash read: {e}"))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hex(&hasher.finalize()))
}

fn verify_signer(
    apk: &apksig::Apk,
    signed_data: &[u8],
    public_key: &[u8],
    signatures: &apksig::common::Signatures,
    digests: &apksig::common::Digests,
    certificates: &apksig::common::Certificates,
) -> Result<(), String> {
    if certificates.certificates_data.is_empty() {
        return Err("APK signer certificate is missing".into());
    }
    if signatures.signatures_data.is_empty() || digests.digests_data.is_empty() {
        return Err("APK signer signature or content digest is missing".into());
    }
    let mut verified = 0usize;
    for signature in &signatures.signatures_data {
        let algorithm_id = u32::from(&signature.signature_algorithm_id);
        let digest = digests
            .digests_data
            .iter()
            .find(|candidate| u32::from(&candidate.signature_algorithm_id) == algorithm_id)
            .ok_or_else(|| {
                format!("APK signer digest missing for algorithm 0x{algorithm_id:04x}")
            })?;
        signature
            .signature_algorithm_id
            .verify(public_key, signed_data, &signature.signature)
            .map_err(|e| format!("APK signer signature invalid: {e}"))?;
        let computed = apk
            .digest(&signature.signature_algorithm_id)
            .map_err(|e| format!("compute APK content digest: {e}"))?;
        if computed != digest.digest {
            return Err(format!(
                "APK content digest mismatch for algorithm 0x{algorithm_id:04x}"
            ));
        }
        verified += 1;
    }
    if verified == 0 {
        return Err("APK has no supported verified signer".into());
    }
    for (index, certificate) in certificates.certificates_data.iter().enumerate() {
        let (remaining, parsed) = x509_parser::parse_x509_certificate(&certificate.certificate)
            .map_err(|e| format!("APK signer certificate {index} is invalid X.509 DER: {e}"))?;
        if !remaining.is_empty() {
            return Err(format!("APK signer certificate {index} has trailing data"));
        }
        if index == 0 && parsed.tbs_certificate.subject_pki.raw != public_key {
            return Err("APK signer public key does not match its certificate".into());
        }
    }
    Ok(())
}

fn verify_apk_signature(path: &Path) -> Result<(), String> {
    let apk =
        apksig::Apk::new(path.to_path_buf()).map_err(|e| format!("open APK signing data: {e}"))?;
    let block = apk
        .get_signing_block()
        .map_err(|e| format!("read APK signing block: {e}"))?;
    let mut schemes = 0usize;
    for value in &block.content {
        match value {
            apksig::ValueSigningBlock::SignatureSchemeV2Block(scheme) => {
                if scheme.signers.signers_data.is_empty() {
                    return Err("APK v2 signing block has no signer".into());
                }
                for signer in &scheme.signers.signers_data {
                    let serialized = signer.signed_data.to_u8();
                    let signed_data = serialized
                        .get(4..)
                        .ok_or("APK v2 signed-data record is truncated")?;
                    verify_signer(
                        &apk,
                        signed_data,
                        &signer.pub_key.data,
                        &signer.signatures,
                        &signer.signed_data.digests,
                        &signer.signed_data.certificates,
                    )?;
                }
                schemes += 1;
            }
            apksig::ValueSigningBlock::SignatureSchemeV3Block(scheme) => {
                if scheme.signers.signers_data.is_empty() {
                    return Err("APK v3 signing block has no signer".into());
                }
                for signer in &scheme.signers.signers_data {
                    if signer.min_sdk != signer.signed_data.min_sdk
                        || signer.max_sdk != signer.signed_data.max_sdk
                    {
                        return Err("APK v3 signer SDK bounds do not match signed data".into());
                    }
                    let serialized = signer.signed_data.to_u8();
                    let signed_data = serialized
                        .get(4..)
                        .ok_or("APK v3 signed-data record is truncated")?;
                    verify_signer(
                        &apk,
                        signed_data,
                        &signer.pub_key.data,
                        &signer.signatures,
                        &signer.signed_data.digests,
                        &signer.signed_data.certificates,
                    )?;
                }
                schemes += 1;
            }
            _ => {}
        }
    }
    if schemes == 0 {
        return Err("APK has no v2 or v3 signing scheme".into());
    }
    Ok(())
}

fn inspect(path: &Path) -> Result<InspectedApk, String> {
    if !path.is_file()
        || path
            .extension()
            .and_then(|value| value.to_str())
            .map(|value| !value.eq_ignore_ascii_case("apk"))
            .unwrap_or(true)
    {
        return Err("select the final signed .apk file".into());
    }
    let snapshot = snapshot_apk(path)?;
    let snapshot_path = snapshot.path.as_path();
    verify_apk_signature(snapshot_path)
        .map_err(|e| format!("APK cryptographic signature verification failed: {e}"))?;
    let apk_sha256 =
        hash_reader(File::open(snapshot_path).map_err(|e| format!("open APK snapshot: {e}"))?)?;
    let mut archive =
        ZipArchive::new(File::open(snapshot_path).map_err(|e| format!("open APK snapshot: {e}"))?)
            .map_err(|e| format!("open APK ZIP: {e}"))?;
    let mut manifest = None;
    let mut bridge_hash = None;
    let mut bridge_count = 0usize;
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|e| format!("read APK entry: {e}"))?;
        let name = entry.name().replace('\\', "/");
        if name == "AndroidManifest.xml" {
            if manifest.is_some() || entry.size() > 16 * 1024 * 1024 {
                return Err("APK must contain one bounded AndroidManifest.xml".into());
            }
            let mut bytes = Vec::with_capacity(entry.size() as usize);
            entry
                .read_to_end(&mut bytes)
                .map_err(|e| format!("read AndroidManifest.xml: {e}"))?;
            manifest = Some(bytes);
        } else if name == BRIDGE_ENTRY {
            bridge_count += 1;
            bridge_hash = Some(hash_reader(&mut entry)?);
        }
    }
    if bridge_count != 1 {
        return Err(format!(
            "APK must contain exactly one {BRIDGE_ENTRY}; found {bridge_count}"
        ));
    }
    let signature_present = true;
    let (package_name, metadata) = parse_manifest(
        manifest
            .as_deref()
            .ok_or("APK has no AndroidManifest.xml")?,
    )?;
    if package_name != PACKAGE {
        return Err(format!("unexpected APK package: {package_name}"));
    }
    let required = |key: &str| {
        metadata
            .get(key)
            .cloned()
            .ok_or_else(|| format!("APK metadata is missing {key}"))
    };
    if required("gxr.telemetry.enabled")? != "true" {
        return Err("APK native Galaxy XR telemetry is not enabled".into());
    }
    let version_code = required("gxr.build.versionCode")?
        .parse::<i64>()
        .map_err(|_| "invalid gxr.build.versionCode")?;
    if !matches!(version_code, 5002318 | 5002322) {
        return Err(format!(
            "unsupported Steam Link version code: {version_code}"
        ));
    }
    let control_port = required("gxr.telemetry.controlPort")?
        .parse::<i64>()
        .map_err(|_| "invalid control port")?;
    let tracking_port = required("gxr.telemetry.trackingPort")?
        .parse::<i64>()
        .map_err(|_| "invalid tracking port")?;
    if control_port != CONTROL_PORT || tracking_port != TRACKING_PORT {
        return Err(format!(
            "APK telemetry ports must be {CONTROL_PORT}/{TRACKING_PORT}"
        ));
    }
    let host = required("gxr.telemetry.host")?;
    let ip = host
        .parse::<Ipv4Addr>()
        .map_err(|_| "APK telemetry host must be IPv4")?;
    let octets = ip.octets();
    if ip.is_unspecified() || ip.is_loopback() || octets[0] == 0 || octets[0] >= 224 {
        return Err("APK telemetry host must be usable unicast non-loopback IPv4".into());
    }
    let pairing_token = required("gxr.telemetry.pairingTokenHex")?.to_ascii_lowercase();
    if pairing_token.len() != 64
        || !pairing_token.bytes().all(|byte| byte.is_ascii_hexdigit())
        || pairing_token
            .bytes()
            .all(|byte| byte == pairing_token.as_bytes()[0])
    {
        return Err("APK pairing token must be 64 nontrivial hexadecimal characters".into());
    }
    let fingerprint = hash_reader(pairing_token.as_bytes())?;
    Ok(InspectedApk {
        preview: GalaxyXrApkPreview {
            apk_path: path.to_string_lossy().into_owned(),
            file_name: path
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("GalaxyXR.apk")
                .to_owned(),
            package_name,
            version_code,
            host,
            control_port,
            tracking_port,
            apk_sha256,
            bridge_sha256: bridge_hash.expect("bridge count validated"),
            pairing_token_fingerprint: fingerprint[..12].to_owned(),
            signature_present,
        },
        pairing_token,
    })
}

#[tauri::command]
pub fn inspect_galaxyxr_apk(apk_path: String) -> Result<GalaxyXrApkPreview, String> {
    Ok(inspect(Path::new(&apk_path))?.preview)
}

fn object<'a>(value: &'a mut Value, key: &str) -> Result<&'a mut Map<String, Value>, String> {
    let root = value
        .as_object_mut()
        .ok_or("settings root must be a JSON object")?;
    let entry = root.entry(key.to_owned()).or_insert_with(|| json!({}));
    entry
        .as_object_mut()
        .ok_or_else(|| format!("settings.{key} must be an object"))
}

fn validate_settings_preimage(
    expected: &Option<String>,
    actual: &Option<Vec<u8>>,
) -> Result<(), String> {
    match (expected, actual) {
        (Some(expected), Some(actual)) if expected.as_bytes() == actual => Ok(()),
        (None, None) => Ok(()),
        _ => Err(
            "Galaxy XR settings changed before enrollment; review them and retry installation"
                .into(),
        ),
    }
}

#[tauri::command]
pub fn enroll_galaxyxr_apk(
    apk_path: String,
    expected_sha256: String,
    settings_path: String,
    expected_settings_contents: Option<String>,
) -> Result<GalaxyXrApkPreview, String> {
    let inspected = inspect(Path::new(&apk_path))?;
    if !inspected
        .preview
        .apk_sha256
        .eq_ignore_ascii_case(&expected_sha256)
    {
        return Err("APK changed after preview; inspect the final signed APK again".into());
    }
    let settings_file = Path::new(&settings_path);
    let settings_existed = settings_file.is_file();
    let settings_bytes = if settings_existed {
        Some(fs::read(settings_file).map_err(|e| format!("read settings: {e}"))?)
    } else {
        None
    };
    validate_settings_preimage(&expected_settings_contents, &settings_bytes)?;
    let mut settings: Value = if let Some(bytes) = settings_bytes {
        serde_json::from_slice(&bytes).map_err(|e| format!("parse settings: {e}"))?
    } else {
        json!({})
    };
    {
        let root = settings
            .as_object_mut()
            .ok_or("settings root must be a JSON object")?;
        if let Some(Value::Object(legacy)) = root.remove("galaxyXr") {
            let canonical = root
                .entry("galaxyXR")
                .or_insert_with(|| json!({}))
                .as_object_mut()
                .ok_or("settings.galaxyXR must be an object")?;
            for (key, value) in legacy {
                canonical.entry(key).or_insert(value);
            }
        }
        let galaxy = object(&mut settings, "galaxyXR")?;
        galaxy.insert("enable".into(), Value::Bool(true));
        galaxy.insert("enableVRLinkCompatibility".into(), Value::Bool(false));
        galaxy.insert(
            "vrlinkCompatibilityMode".into(),
            Value::String("off".into()),
        );
        let eye = galaxy
            .entry("eye")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or("settings.galaxyXR.eye must be an object")?;
        eye.insert("source".into(), Value::String("android_xr".into()));
        let face = galaxy
            .entry("face")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or("settings.galaxyXR.face must be an object")?;
        face.insert("enableLosslessOutput".into(), Value::Bool(true));
        let telemetry = galaxy
            .entry("telemetry")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or("settings.galaxyXR.telemetry must be an object")?;
        telemetry.insert("enable".into(), Value::Bool(true));
        telemetry.insert(
            "listenAddress".into(),
            Value::String(inspected.preview.host.clone()),
        );
        telemetry.insert("controlPort".into(), json!(CONTROL_PORT));
        telemetry.insert("trackingPort".into(), json!(TRACKING_PORT));
        telemetry.insert("requirePairing".into(), Value::Bool(true));
        telemetry.insert("pairingTokenFile".into(), Value::String(String::new()));
        telemetry.insert(
            "pairingTokenHex".into(),
            Value::String(inspected.pairing_token),
        );
        let mut clients = telemetry
            .remove("allowedClients")
            .and_then(|value| value.as_array().cloned())
            .unwrap_or_default();
        clients.retain(|client| {
            client.get("versionCode").and_then(Value::as_i64)
                != Some(inspected.preview.version_code)
        });
        clients.push(json!({
            "versionCode": inspected.preview.version_code,
            "apkSha256": inspected.preview.apk_sha256,
            "bridgeSha256": inspected.preview.bridge_sha256,
        }));
        telemetry.insert("allowedClients".into(), Value::Array(clients));
    }
    let serialized =
        serde_json::to_string_pretty(&settings).map_err(|e| format!("serialize settings: {e}"))?;
    crate::driver_installer::write_json_file_transactional_checked(
        settings_path,
        serialized,
        Some(expected_settings_contents.as_deref().map(str::as_bytes)),
    )?;
    Ok(inspected.preview)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rsa::pkcs8::EncodePublicKey;
    use std::io::Write;

    fn put_u16(output: &mut Vec<u8>, value: u16) {
        output.extend_from_slice(&value.to_le_bytes());
    }

    fn der(tag: u8, content: &[u8]) -> Vec<u8> {
        let mut output = vec![tag];
        if content.len() < 128 {
            output.push(content.len() as u8);
        } else {
            let bytes = (content.len() as u32).to_be_bytes();
            let first = bytes.iter().position(|byte| *byte != 0).unwrap_or(3);
            output.push(0x80 | (bytes.len() - first) as u8);
            output.extend_from_slice(&bytes[first..]);
        }
        output.extend_from_slice(content);
        output
    }

    fn test_certificate(public_key: &[u8]) -> Vec<u8> {
        let algorithm = der(
            0x30,
            &[
                der(
                    0x06,
                    &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b],
                ),
                der(0x05, &[]),
            ]
            .concat(),
        );
        let validity = der(
            0x30,
            &[der(0x17, b"240101000000Z"), der(0x17, b"490101000000Z")].concat(),
        );
        let tbs = der(
            0x30,
            &[
                der(0xa0, &der(0x02, &[2])),
                der(0x02, &[1]),
                algorithm.clone(),
                der(0x30, &[]),
                validity,
                der(0x30, &[]),
                public_key.to_vec(),
            ]
            .concat(),
        );
        der(0x30, &[tbs, algorithm, der(0x03, &[0, 0])].concat())
    }

    #[test]
    fn signed_apk_content_tamper_is_rejected() {
        let root = std::env::temp_dir().join(format!(
            "galaxyxr-apk-signature-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("clock")
                .as_nanos()
        ));
        fs::create_dir_all(&root).expect("create test directory");
        let raw_path = root.join("raw.apk");
        let signed_path = root.join("signed.apk");
        let tampered_path = root.join("tampered.apk");
        {
            let file = File::create(&raw_path).expect("create raw APK");
            let mut zip = zip::ZipWriter::new(file);
            zip.start_file(
                "payload.bin",
                zip::write::SimpleFileOptions::default()
                    .compression_method(zip::CompressionMethod::Stored),
            )
            .expect("start ZIP entry");
            zip.write_all(b"signed-payload-marker")
                .expect("write ZIP entry");
            zip.finish().expect("finish raw APK");
        }
        let key = rsa::RsaPrivateKey::new(&mut rand::thread_rng(), 2048).expect("RSA key");
        let certificate = test_certificate(
            key.to_public_key()
                .to_public_key_der()
                .expect("public key DER")
                .as_ref(),
        );
        let mut apk = apksig::Apk::new_raw(raw_path).expect("open raw APK");
        apk.sign_v2(
            &apksig::Algorithms::RSASSA_PKCS1_v1_5_256,
            &certificate,
            key,
        )
        .expect("sign APK");
        {
            let mut output = File::create(&signed_path).expect("create signed APK");
            apk.write_with_signature(&mut output)
                .expect("write signed APK");
        }
        verify_apk_signature(&signed_path).expect("untampered signature verifies");

        let mut bytes = fs::read(&signed_path).expect("read signed APK");
        let marker = b"signed-payload-marker";
        let offset = bytes
            .windows(marker.len())
            .position(|window| window == marker)
            .expect("payload marker");
        bytes[offset] ^= 0x01;
        fs::write(&tampered_path, bytes).expect("write tampered APK");
        let error = verify_apk_signature(&tampered_path).expect_err("tamper must fail");
        assert!(error.contains("content digest mismatch"), "{error}");
        let _ = fs::remove_dir_all(root);
    }

    fn put_u32(output: &mut Vec<u8>, value: u32) {
        output.extend_from_slice(&value.to_le_bytes());
    }

    fn string_pool(strings: &[&str]) -> Vec<u8> {
        let mut encoded = Vec::new();
        let mut offsets = Vec::new();
        for value in strings {
            assert!(value.len() < 128);
            offsets.push(encoded.len() as u32);
            encoded.push(value.chars().count() as u8);
            encoded.push(value.len() as u8);
            encoded.extend_from_slice(value.as_bytes());
            encoded.push(0);
        }
        while encoded.len() % 4 != 0 {
            encoded.push(0);
        }
        let strings_start = 28 + offsets.len() * 4;
        let chunk_size = strings_start + encoded.len();
        let mut output = Vec::new();
        put_u16(&mut output, 0x0001);
        put_u16(&mut output, 28);
        put_u32(&mut output, chunk_size as u32);
        put_u32(&mut output, strings.len() as u32);
        put_u32(&mut output, 0);
        put_u32(&mut output, 0x100);
        put_u32(&mut output, strings_start as u32);
        put_u32(&mut output, 0);
        for offset in offsets {
            put_u32(&mut output, offset);
        }
        output.extend_from_slice(&encoded);
        output
    }

    fn start_element(name: u32, attributes: &[(u32, u32, u32)]) -> Vec<u8> {
        let mut output = Vec::new();
        put_u16(&mut output, 0x0102);
        put_u16(&mut output, 16);
        put_u32(&mut output, (36 + attributes.len() * 20) as u32);
        put_u32(&mut output, 1);
        put_u32(&mut output, NO_INDEX);
        put_u32(&mut output, NO_INDEX);
        put_u32(&mut output, name);
        put_u16(&mut output, 20);
        put_u16(&mut output, 20);
        put_u16(&mut output, attributes.len() as u16);
        put_u16(&mut output, 0);
        put_u16(&mut output, 0);
        put_u16(&mut output, 0);
        for (namespace, attribute_name, value) in attributes {
            put_u32(&mut output, *namespace);
            put_u32(&mut output, *attribute_name);
            put_u32(&mut output, *value);
            put_u16(&mut output, 8);
            output.push(0);
            output.push(0x03);
            put_u32(&mut output, *value);
        }
        output
    }

    fn end_element(name: u32) -> Vec<u8> {
        let mut output = Vec::new();
        put_u16(&mut output, 0x0103);
        put_u16(&mut output, 16);
        put_u32(&mut output, 24);
        put_u32(&mut output, 1);
        put_u32(&mut output, NO_INDEX);
        put_u32(&mut output, NO_INDEX);
        put_u32(&mut output, name);
        output
    }

    #[test]
    fn hex_is_lowercase_and_stable() {
        assert_eq!(hex(&[0, 1, 0xab, 0xff]), "0001abff");
    }

    #[test]
    fn settings_preimage_requires_exact_file_state() {
        assert!(validate_settings_preimage(&None, &None).is_ok());
        assert!(validate_settings_preimage(&Some("{}".into()), &Some(b"{}".to_vec())).is_ok());
        assert!(validate_settings_preimage(&Some("{}".into()), &Some(b"{ }".to_vec())).is_err());
        assert!(validate_settings_preimage(&None, &Some(b"{}".to_vec())).is_err());
    }

    #[test]
    fn rejects_plain_xml_as_untrusted_manifest() {
        assert!(parse_manifest(b"<manifest package='fake'/>").is_err());
    }

    #[test]
    fn parses_binary_manifest_package_and_application_metadata() {
        let strings = [
            "manifest",
            "application",
            "meta-data",
            "package",
            "name",
            "value",
            PACKAGE,
            "gxr.telemetry.enabled",
            "true",
            ANDROID_NAMESPACE,
        ];
        let mut body = string_pool(&strings);
        body.extend(start_element(0, &[(NO_INDEX, 3, 6)]));
        body.extend(start_element(1, &[]));
        body.extend(start_element(2, &[(9, 4, 7), (9, 5, 8)]));
        body.extend(end_element(2));
        body.extend(end_element(1));
        body.extend(end_element(0));
        let mut xml = Vec::new();
        put_u16(&mut xml, 0x0003);
        put_u16(&mut xml, 8);
        put_u32(&mut xml, (8 + body.len()) as u32);
        xml.extend(body);
        let (package, metadata) = parse_manifest(&xml).expect("binary manifest parses");
        assert_eq!(package, PACKAGE);
        assert_eq!(
            metadata.get("gxr.telemetry.enabled").map(String::as_str),
            Some("true")
        );
    }

    #[test]
    fn ignores_unnamespaced_metadata_spoof() {
        let strings = [
            "manifest",
            "application",
            "meta-data",
            "package",
            "name",
            "value",
            PACKAGE,
            "gxr.telemetry.enabled",
            "true",
        ];
        let mut body = string_pool(&strings);
        body.extend(start_element(0, &[(NO_INDEX, 3, 6)]));
        body.extend(start_element(1, &[]));
        body.extend(start_element(2, &[(NO_INDEX, 4, 7), (NO_INDEX, 5, 8)]));
        body.extend(end_element(2));
        body.extend(end_element(1));
        body.extend(end_element(0));
        let mut xml = Vec::new();
        put_u16(&mut xml, 0x0003);
        put_u16(&mut xml, 8);
        put_u32(&mut xml, (8 + body.len()) as u32);
        xml.extend(body);
        let (_, metadata) = parse_manifest(&xml).expect("binary manifest parses");
        assert!(metadata.is_empty());
    }
}
