use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const DRIVER_NAME: &str = "CustomHeadsetOpenVR";
const REQUIRED_FILES: &[&str] = &[
    "driver.vrdrivermanifest",
    "bin/win64/driver_CustomHeadsetOpenVR.dll",
    "resources/driver.vrresources",
    "resources/settings/default.vrsettings",
    "resources/input/galaxy_xr_hmd_profile.json",
    "resources/input/galaxy_xr_controller_profile.json",
    "resources/rendermodels/vst_controller_left/vst_controller_left.obj",
    "resources/rendermodels/vst_controller_right/vst_controller_right.obj",
    "resources/icons/galaxyxr/headset_galaxy_xr_status_ready.png",
];

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallReceipt {
    driver_path: String,
    package_sha256: String,
    file_count: usize,
    vrcft_module_installed: bool,
    vrcft_module_path: Option<String>,
    vrcft_module_sha256: Option<String>,
}

struct ModuleTransaction {
    target: PathBuf,
    backup: Option<PathBuf>,
    sha256: String,
}

fn unique_suffix() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    format!("{}-{nanos}", std::process::id())
}

fn reject_link(metadata: &fs::Metadata, path: &Path) -> Result<(), String> {
    if metadata.file_type().is_symlink() {
        return Err(format!(
            "reparse/symlink entries are not accepted: {}",
            path.display()
        ));
    }
    Ok(())
}

fn hash_file(path: &Path) -> Result<String, String> {
    let mut file = fs::File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|e| format!("read {}: {e}", path.display()))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

fn hash_tree(root: &Path) -> Result<BTreeMap<String, String>, String> {
    let mut pending = vec![root.to_path_buf()];
    let mut result = BTreeMap::new();
    while let Some(directory) = pending.pop() {
        for entry in fs::read_dir(&directory)
            .map_err(|e| format!("read directory {}: {e}", directory.display()))?
        {
            let entry = entry.map_err(|e| format!("read directory entry: {e}"))?;
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path)
                .map_err(|e| format!("metadata {}: {e}", path.display()))?;
            reject_link(&metadata, &path)?;
            if metadata.is_dir() {
                pending.push(path);
            } else if metadata.is_file() {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|e| format!("relative path {}: {e}", path.display()))?
                    .to_string_lossy()
                    .replace('\\', "/");
                result.insert(relative, hash_file(&path)?);
            } else {
                return Err(format!("unsupported filesystem entry: {}", path.display()));
            }
        }
    }
    Ok(result)
}

fn tree_identity(tree: &BTreeMap<String, String>) -> String {
    let mut hasher = Sha256::new();
    for (path, hash) in tree {
        hasher.update(path.as_bytes());
        hasher.update([0]);
        hasher.update(hash.as_bytes());
        hasher.update([b'\n']);
    }
    format!("{:x}", hasher.finalize())
}

fn receipt_path() -> Result<PathBuf, String> {
    let appdata = std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?;
    Ok(PathBuf::from(appdata)
        .join("CustomHeadset")
        .join("install-state.json"))
}

fn load_receipt() -> Result<Option<InstallReceipt>, String> {
    let path = receipt_path()?;
    if !path.exists() {
        return Ok(None);
    }
    serde_json::from_slice(&fs::read(&path).map_err(|e| format!("read install receipt: {e}"))?)
        .map(Some)
        .map_err(|e| format!("parse install receipt: {e}"))
}

fn write_receipt(receipt: &InstallReceipt) -> Result<(), String> {
    let path = receipt_path()?;
    let parent = path.parent().ok_or("install receipt has no parent")?;
    fs::create_dir_all(parent).map_err(|e| format!("create install receipt directory: {e}"))?;
    let stage = parent.join(format!(".install-state.{}.stage", unique_suffix()));
    let bytes = serde_json::to_vec_pretty(receipt)
        .map_err(|e| format!("serialize install receipt: {e}"))?;
    fs::write(&stage, bytes).map_err(|e| format!("stage install receipt: {e}"))?;
    let backup = parent.join(format!(".install-state.{}.backup", unique_suffix()));
    let had_receipt = path.exists();
    if had_receipt {
        fs::rename(&path, &backup).map_err(|e| format!("backup install receipt: {e}"))?;
    }
    if let Err(error) = fs::rename(&stage, &path) {
        let restore = if had_receipt {
            fs::rename(&backup, &path).err()
        } else {
            None
        };
        return Err(format!(
            "activate install receipt: {error}; restore={restore:?}"
        ));
    }
    if had_receipt {
        let _ = fs::remove_file(backup);
    }
    Ok(())
}

fn validate_resource_value(
    value: &serde_json::Value,
    resources: &Path,
    document_parent: &Path,
) -> Result<(), String> {
    match value {
        serde_json::Value::String(text) => {
            if let Some(relative) = text.strip_prefix("{CustomHeadsetOpenVR}/") {
                let target =
                    resources.join(relative.replace('/', &std::path::MAIN_SEPARATOR.to_string()));
                if !target.exists() {
                    return Err(format!("missing managed resource reference: {text}"));
                }
            } else if !text.contains('/') && !text.contains('\\') {
                let extension = Path::new(text).extension().and_then(|value| value.to_str());
                if matches!(
                    extension,
                    Some("json" | "png" | "obj" | "mtl" | "svg" | "gif")
                ) && !document_parent.join(text).exists()
                {
                    return Err(format!("missing relative resource reference: {text}"));
                }
            }
        }
        serde_json::Value::Array(values) => {
            for child in values {
                validate_resource_value(child, resources, document_parent)?;
            }
        }
        serde_json::Value::Object(values) => {
            for child in values.values() {
                validate_resource_value(child, resources, document_parent)?;
            }
        }
        _ => {}
    }
    Ok(())
}

fn validate_json_tree(root: &Path) -> Result<(), String> {
    let resources = root.join("resources");
    let mut pending = vec![resources.clone()];
    while let Some(directory) = pending.pop() {
        for entry in fs::read_dir(&directory)
            .map_err(|e| format!("read JSON resource directory {}: {e}", directory.display()))?
        {
            let path = entry
                .map_err(|e| format!("read JSON resource entry: {e}"))?
                .path();
            let metadata = fs::symlink_metadata(&path)
                .map_err(|e| format!("metadata {}: {e}", path.display()))?;
            reject_link(&metadata, &path)?;
            if metadata.is_dir() {
                pending.push(path);
            } else if matches!(
                path.extension().and_then(|value| value.to_str()),
                Some("json" | "vrsettings")
            ) {
                let value: serde_json::Value = serde_json::from_slice(
                    &fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?,
                )
                .map_err(|e| format!("parse {}: {e}", path.display()))?;
                validate_resource_value(
                    &value,
                    &resources,
                    path.parent().ok_or("resource document has no parent")?,
                )?;
            } else if matches!(
                path.extension().and_then(|value| value.to_str()),
                Some("obj" | "mtl")
            ) {
                let text = String::from_utf8(
                    fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?,
                )
                .map_err(|e| format!("decode {}: {e}", path.display()))?;
                for line in text.lines() {
                    let trimmed = line.trim();
                    let relative = trimmed
                        .strip_prefix("mtllib ")
                        .or_else(|| trimmed.strip_prefix("map_Kd "))
                        .or_else(|| trimmed.strip_prefix("map_Ks "));
                    if let Some(relative) = relative {
                        let dependency = path
                            .parent()
                            .ok_or("model resource has no parent")?
                            .join(relative.trim());
                        if !dependency.is_file() {
                            return Err(format!(
                                "missing model dependency referenced by {}: {}",
                                path.display(),
                                relative.trim()
                            ));
                        }
                    }
                }
            }
        }
    }
    Ok(())
}

fn validate_package(root: &Path) -> Result<(), String> {
    for required in REQUIRED_FILES {
        let path = root.join(required);
        let metadata = fs::symlink_metadata(&path)
            .map_err(|_| format!("required package file missing: {}", path.display()))?;
        reject_link(&metadata, &path)?;
        if !metadata.is_file() {
            return Err(format!(
                "required package path is not a file: {}",
                path.display()
            ));
        }
    }
    let manifest: serde_json::Value = serde_json::from_slice(
        &fs::read(root.join("driver.vrdrivermanifest"))
            .map_err(|e| format!("read driver manifest: {e}"))?,
    )
    .map_err(|e| format!("parse driver manifest: {e}"))?;
    if manifest.get("name").and_then(|v| v.as_str()) != Some(DRIVER_NAME)
        || manifest.get("resourceOnly").and_then(|v| v.as_bool()) != Some(false)
    {
        return Err(
            "driver manifest identity is not the active CustomHeadsetOpenVR package".into(),
        );
    }
    validate_json_tree(root)?;
    Ok(())
}

fn copy_tree(source: &Path, destination: &Path) -> Result<(), String> {
    fs::create_dir(destination)
        .map_err(|e| format!("create staging directory {}: {e}", destination.display()))?;
    for entry in fs::read_dir(source).map_err(|e| format!("read {}: {e}", source.display()))? {
        let entry = entry.map_err(|e| format!("read package entry: {e}"))?;
        let source_path = entry.path();
        let destination_path = destination.join(entry.file_name());
        let metadata = fs::symlink_metadata(&source_path)
            .map_err(|e| format!("metadata {}: {e}", source_path.display()))?;
        reject_link(&metadata, &source_path)?;
        if metadata.is_dir() {
            copy_tree(&source_path, &destination_path)?;
        } else if metadata.is_file() {
            fs::copy(&source_path, &destination_path)
                .map_err(|e| format!("copy {}: {e}", source_path.display()))?;
        } else {
            return Err(format!(
                "unsupported package entry: {}",
                source_path.display()
            ));
        }
    }
    Ok(())
}

fn stage_vrcft_module(
    module_path: Option<&str>,
    previous: Option<&InstallReceipt>,
) -> Result<Option<ModuleTransaction>, String> {
    let Some(module_path) = module_path else {
        return Ok(None);
    };
    let source = Path::new(module_path);
    if !source.is_file() {
        return Ok(None);
    }
    let source_bytes = fs::read(source).map_err(|e| format!("read VRCFT module: {e}"))?;
    if source_bytes.len() < 4096 || source_bytes.get(..2) != Some(b"MZ") {
        return Err("VRCFT module is not a plausible Windows .NET assembly".into());
    }
    let appdata = std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?;
    let vrcft_root = PathBuf::from(appdata).join("VRCFaceTracking");
    if !vrcft_root.is_dir() {
        return Ok(None);
    }
    let target_dir = vrcft_root.join("CustomLibs");
    if !target_dir.is_dir() {
        return Ok(None);
    }
    let target = target_dir.join("GalaxyXR.VRCFaceTracking.dll");
    if target.exists() {
        let owned = previous
            .and_then(|receipt| {
                receipt
                    .vrcft_module_path
                    .as_deref()
                    .zip(receipt.vrcft_module_sha256.as_deref())
            })
            .map(|(path, hash)| {
                Path::new(path) == target && hash_file(&target).as_deref() == Ok(hash)
            })
            .unwrap_or(false);
        if !owned {
            return Err(format!(
                "refusing to overwrite an unowned VRCFT module: {}",
                target.display()
            ));
        }
    }
    let stage = target_dir.join(format!(
        ".GalaxyXR.VRCFaceTracking.{}.stage",
        unique_suffix()
    ));
    fs::copy(source, &stage).map_err(|e| format!("stage VRCFT module: {e}"))?;
    let source_hash = hash_file(source)?;
    if source_hash != hash_file(&stage)? {
        let _ = fs::remove_file(&stage);
        return Err("VRCFT module staging hash mismatch".into());
    }
    let backup = target_dir.join(format!(
        ".GalaxyXR.VRCFaceTracking.{}.backup",
        unique_suffix()
    ));
    let had_target = target.exists();
    if had_target {
        fs::rename(&target, &backup).map_err(|e| format!("backup VRCFT module: {e}"))?;
    }
    if let Err(error) = fs::rename(&stage, &target) {
        let restore = if had_target {
            fs::rename(&backup, &target).err()
        } else {
            None
        };
        return Err(format!(
            "activate VRCFT module: {error}; restore={restore:?}"
        ));
    }
    Ok(Some(ModuleTransaction {
        target,
        backup: had_target.then_some(backup),
        sha256: source_hash,
    }))
}

fn rollback_module(transaction: &ModuleTransaction) -> Result<(), String> {
    fs::remove_file(&transaction.target)
        .map_err(|e| format!("remove activated VRCFT module during rollback: {e}"))?;
    if let Some(backup) = &transaction.backup {
        fs::rename(backup, &transaction.target)
            .map_err(|e| format!("restore previous VRCFT module: {e}"))?;
    }
    Ok(())
}

fn rollback_driver(target: &Path, backup: &Path, had_target: bool) -> Result<(), String> {
    if target.exists() {
        fs::remove_dir_all(target)
            .map_err(|e| format!("remove activated driver during rollback: {e}"))?;
    }
    if had_target {
        fs::rename(backup, target).map_err(|e| format!("restore previous driver: {e}"))?;
    }
    Ok(())
}

#[tauri::command]
pub fn verify_driver_install(steamvr_dir: String) -> Result<bool, String> {
    let Some(receipt) = load_receipt()? else {
        return Ok(false);
    };
    let target = Path::new(&steamvr_dir).join("drivers").join(DRIVER_NAME);
    if !target.is_dir() || receipt.driver_path != target.to_string_lossy() {
        return Ok(false);
    }
    if validate_package(&target).is_err()
        || tree_identity(&hash_tree(&target)?) != receipt.package_sha256
    {
        return Ok(false);
    }
    if let Some(module_path) = receipt.vrcft_module_path.as_deref() {
        let Some(expected) = receipt.vrcft_module_sha256.as_deref() else {
            return Ok(false);
        };
        let module = Path::new(module_path);
        if !module.is_file() || hash_file(module)? != expected {
            return Ok(false);
        }
    }
    Ok(true)
}

#[tauri::command]
pub fn write_json_file_transactional(path: String, contents: String) -> Result<(), String> {
    let target = PathBuf::from(path);
    let parent = target.parent().ok_or("settings path has no parent")?;
    if !parent.is_dir() {
        return Err("settings directory does not exist".into());
    }
    let _: serde_json::Value = serde_json::from_str(&contents)
        .map_err(|e| format!("refusing invalid settings JSON: {e}"))?;
    let stage = parent.join(format!(".steamvr-settings.{}.stage", unique_suffix()));
    let backup = parent.join(format!(".steamvr-settings.{}.backup", unique_suffix()));
    fs::write(&stage, contents).map_err(|e| format!("stage SteamVR settings: {e}"))?;
    let had_target = target.exists();
    if had_target {
        fs::rename(&target, &backup).map_err(|e| format!("backup SteamVR settings: {e}"))?;
    }
    if let Err(error) = fs::rename(&stage, &target) {
        let restore = if had_target {
            fs::rename(&backup, &target).err()
        } else {
            None
        };
        return Err(format!(
            "activate SteamVR settings: {error}; restore={restore:?}"
        ));
    }
    if had_target {
        let _ = fs::remove_file(backup);
    }
    Ok(())
}

#[tauri::command]
pub fn install_driver_transactional(
    source_dir: String,
    steamvr_dir: String,
    vrcft_module_path: Option<String>,
) -> Result<InstallReceipt, String> {
    let previous_receipt = load_receipt()?;
    let source =
        fs::canonicalize(&source_dir).map_err(|e| format!("resolve source package: {e}"))?;
    validate_package(&source)?;
    let source_tree = hash_tree(&source)?;
    let package_sha256 = tree_identity(&source_tree);

    let drivers_root = Path::new(&steamvr_dir).join("drivers");
    fs::create_dir_all(&drivers_root)
        .map_err(|e| format!("create SteamVR drivers directory: {e}"))?;
    let target = drivers_root.join(DRIVER_NAME);
    let suffix = unique_suffix();
    let stage = drivers_root.join(format!(".{DRIVER_NAME}.{suffix}.stage"));
    let backup = drivers_root.join(format!(".{DRIVER_NAME}.{suffix}.backup"));

    if let Err(error) = copy_tree(&source, &stage) {
        let _ = fs::remove_dir_all(&stage);
        return Err(error);
    }
    if let Err(error) = validate_package(&stage) {
        let _ = fs::remove_dir_all(&stage);
        return Err(error);
    }
    let staged_tree = match hash_tree(&stage) {
        Ok(tree) => tree,
        Err(error) => {
            let _ = fs::remove_dir_all(&stage);
            return Err(error);
        }
    };
    if staged_tree != source_tree {
        let _ = fs::remove_dir_all(&stage);
        return Err("staged driver tree does not match the source package".into());
    }

    let had_target = target.exists();
    if had_target {
        let Some(previous) = &previous_receipt else {
            let _ = fs::remove_dir_all(&stage);
            return Err("existing driver is unowned because its managed receipt is missing; refusing replacement".into());
        };
        if let Err(error) = validate_package(&target) {
            let _ = fs::remove_dir_all(&stage);
            return Err(format!("validate installed driver: {error}"));
        }
        let installed_tree = match hash_tree(&target) {
            Ok(tree) => tree,
            Err(error) => {
                let _ = fs::remove_dir_all(&stage);
                return Err(error);
            }
        };
        let installed_identity = tree_identity(&installed_tree);
        if previous.driver_path != target.to_string_lossy()
            || previous.package_sha256 != installed_identity
        {
            let _ = fs::remove_dir_all(&stage);
            return Err("installed driver drifted from its receipt; refusing replacement".into());
        }
        if let Err(error) = fs::rename(&target, &backup) {
            let _ = fs::remove_dir_all(&stage);
            return Err(format!("backup installed driver: {error}"));
        }
    }
    if let Err(error) = fs::rename(&stage, &target) {
        let restore = if had_target {
            fs::rename(&backup, &target).err()
        } else {
            None
        };
        return Err(format!(
            "activate staged driver: {error}; restore={restore:?}"
        ));
    }
    if let Err(error) = validate_package(&target) {
        let rollback = rollback_driver(&target, &backup, had_target).err();
        return Err(format!(
            "activated package validation failed: {error}; rollback={rollback:?}"
        ));
    }
    let module_transaction =
        match stage_vrcft_module(vrcft_module_path.as_deref(), previous_receipt.as_ref()) {
            Ok(transaction) => transaction,
            Err(error) => {
                let rollback = rollback_driver(&target, &backup, had_target).err();
                return Err(format!(
                    "VRCFT module installation failed: {error}; driver rollback={rollback:?}"
                ));
            }
        };
    let receipt = InstallReceipt {
        driver_path: target.to_string_lossy().into_owned(),
        package_sha256,
        file_count: source_tree.len(),
        vrcft_module_installed: module_transaction.is_some(),
        vrcft_module_path: module_transaction
            .as_ref()
            .map(|transaction| transaction.target.to_string_lossy().into_owned()),
        vrcft_module_sha256: module_transaction
            .as_ref()
            .map(|transaction| transaction.sha256.clone()),
    };
    if let Err(error) = write_receipt(&receipt) {
        let module_rollback = module_transaction
            .as_ref()
            .and_then(|transaction| rollback_module(transaction).err());
        let driver_rollback = rollback_driver(&target, &backup, had_target).err();
        return Err(format!(
            "commit install receipt: {error}; module rollback={module_rollback:?}; driver rollback={driver_rollback:?}"
        ));
    }
    if had_target {
        let _ = fs::remove_dir_all(&backup);
    }
    if let Some(transaction) = &module_transaction {
        if let Some(module_backup) = &transaction.backup {
            let _ = fs::remove_file(module_backup);
        }
    }
    Ok(receipt)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn built_release_package_has_a_closed_resource_graph() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("..")
            .join("output")
            .join(DRIVER_NAME);
        assert!(
            root.is_dir(),
            "build the native driver before running this test"
        );
        validate_package(&root).expect("built package validation failed");
    }
}

#[tauri::command]
pub fn uninstall_driver_transactional(steamvr_dir: String) -> Result<bool, String> {
    let receipt =
        load_receipt()?.ok_or("managed install receipt is missing; refusing uninstall")?;
    let target = Path::new(&steamvr_dir).join("drivers").join(DRIVER_NAME);
    if !target.exists() {
        return Err("managed receipt exists but the driver target is missing; preserve the receipt and inspect pending tombstones before retrying".into());
    }
    validate_package(&target)?;
    if receipt.driver_path != target.to_string_lossy()
        || tree_identity(&hash_tree(&target)?) != receipt.package_sha256
    {
        return Err("installed driver drifted from its receipt; refusing uninstall".into());
    }
    let module = receipt.vrcft_module_path.as_ref().map(PathBuf::from);
    if let Some(module_path) = &module {
        let expected = receipt
            .vrcft_module_sha256
            .as_deref()
            .ok_or("VRCFT receipt hash is missing")?;
        if !module_path.is_file() || hash_file(module_path)? != expected {
            return Err(
                "installed VRCFT module drifted from its receipt; refusing uninstall".into(),
            );
        }
    }
    let tombstone = target
        .parent()
        .unwrap()
        .join(format!(".{DRIVER_NAME}.{}.remove", unique_suffix()));
    fs::rename(&target, &tombstone).map_err(|e| format!("detach installed driver: {e}"))?;
    let module_tombstone = module.as_ref().map(|path| {
        path.parent().unwrap().join(format!(
            ".GalaxyXR.VRCFaceTracking.{}.remove",
            unique_suffix()
        ))
    });
    if let (Some(module_path), Some(detached)) = (&module, &module_tombstone) {
        if let Err(error) = fs::rename(module_path, detached) {
            let restore = fs::rename(&tombstone, &target).err();
            return Err(format!(
                "detach VRCFT module: {error}; driver restore={restore:?}"
            ));
        }
    }
    if let Err(error) = fs::remove_file(receipt_path()?) {
        let driver_restore = fs::rename(&tombstone, &target).err();
        let module_restore =
            if let (Some(module_path), Some(detached)) = (&module, &module_tombstone) {
                fs::rename(detached, module_path).err()
            } else {
                None
            };
        return Err(format!(
            "remove install receipt: {error}; driver restore={driver_restore:?}; module restore={module_restore:?}"
        ));
    }
    // Detachment plus receipt removal is the uninstall commit. Tombstones are
    // now inert cleanup debt, so locked files must not turn a committed
    // uninstall into a reported rollback failure.
    let _ = fs::remove_dir_all(&tombstone);
    if let Some(detached) = &module_tombstone {
        let _ = fs::remove_file(detached);
    }
    Ok(true)
}
