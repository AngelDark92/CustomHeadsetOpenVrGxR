use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};
use sysinfo::{ProcessesToUpdate, System};

const ACTIVE_DRIVER_NAME: &str = "CustomHeadsetOpenVR";
const RESOURCE_DRIVER_NAME: &str = "galaxyxrresources";
const ACTIVE_REQUIRED_FILES: &[&str] = &[
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
const RESOURCE_REQUIRED_FILES: &[&str] = &[
    "driver.vrdrivermanifest",
    "resources/driver.vrresources",
    "resources/settings/default.vrsettings",
    "resources/input/galaxy_xr_hmd_profile.json",
    "resources/input/galaxy_xr_controller_profile.json",
    "resources/rendermodels/galaxy_xr_hmd/galaxy_xr_hmd.obj",
    "resources/rendermodels/galaxy_xr_hmd/galaxy_xr_hmd.mtl",
    "resources/rendermodels/galaxy_xr_hmd/galaxy_xr_hmd.png",
    "resources/rendermodels/vst_controller_left/vst_controller_left.obj",
    "resources/rendermodels/vst_controller_right/vst_controller_right.obj",
    "resources/icons/galaxyxr/headset_galaxy_xr_status_ready.png",
];

#[derive(Clone, Copy)]
struct PackageSpec {
    name: &'static str,
    required_files: &'static [&'static str],
    resource_only: bool,
}

const ACTIVE_SPEC: PackageSpec = PackageSpec {
    name: ACTIVE_DRIVER_NAME,
    required_files: ACTIVE_REQUIRED_FILES,
    resource_only: false,
};
const RESOURCE_SPEC: PackageSpec = PackageSpec {
    name: RESOURCE_DRIVER_NAME,
    required_files: RESOURCE_REQUIRED_FILES,
    resource_only: true,
};

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackageReceipt {
    name: String,
    path: String,
    sha256: String,
    file_count: usize,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallReceipt {
    #[serde(default = "receipt_schema_version")]
    schema_version: u32,
    #[serde(default)]
    packages: Vec<PackageReceipt>,
    // Legacy v1 fields are accepted only to prove ownership of the active package during upgrade.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    driver_path: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    package_sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    file_count: Option<usize>,
    vrcft_module_installed: bool,
    vrcft_module_path: Option<String>,
    vrcft_module_sha256: Option<String>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanupPreview {
    plan_token: String,
    actions: Vec<String>,
    preserved: Vec<String>,
    blockers: Vec<String>,
    steam_vr_running: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanupReport {
    removed: Vec<String>,
    unregistered: Vec<String>,
    preserved: Vec<String>,
    warnings: Vec<String>,
}

struct CleanupPlan {
    preview: CleanupPreview,
    targets: Vec<PathBuf>,
    registrations: Vec<PathBuf>,
    vrpathreg: PathBuf,
}

fn receipt_schema_version() -> u32 {
    2
}

impl InstallReceipt {
    fn package(&self, name: &str) -> Option<&PackageReceipt> {
        self.packages.iter().find(|package| package.name == name)
    }

    fn owns(&self, spec: PackageSpec, target: &Path, sha256: &str) -> bool {
        self.package(spec.name)
            .map(|package| package.path == target.to_string_lossy() && package.sha256 == sha256)
            .unwrap_or_else(|| {
                spec.name == ACTIVE_DRIVER_NAME
                    && self.driver_path.as_deref() == Some(target.to_string_lossy().as_ref())
                    && self.package_sha256.as_deref() == Some(sha256)
            })
    }
}

struct PackageTransaction {
    spec: PackageSpec,
    target: PathBuf,
    backup: PathBuf,
    stage: PathBuf,
    had_target: bool,
    activated: bool,
    sha256: String,
    file_count: usize,
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
    driver_name: &str,
) -> Result<(), String> {
    match value {
        serde_json::Value::String(text) => {
            let prefix = format!("{{{driver_name}}}/");
            if let Some(relative) = text.strip_prefix(&prefix) {
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
                validate_resource_value(child, resources, document_parent, driver_name)?;
            }
        }
        serde_json::Value::Object(values) => {
            for child in values.values() {
                validate_resource_value(child, resources, document_parent, driver_name)?;
            }
        }
        _ => {}
    }
    Ok(())
}

fn validate_json_tree(root: &Path, driver_name: &str) -> Result<(), String> {
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
                    driver_name,
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

fn validate_package(root: &Path, spec: PackageSpec) -> Result<(), String> {
    for required in spec.required_files {
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
    if manifest.get("name").and_then(|v| v.as_str()) != Some(spec.name)
        || manifest.get("resourceOnly").and_then(|v| v.as_bool()) != Some(spec.resource_only)
    {
        return Err(format!(
            "driver manifest identity does not match package {}",
            spec.name
        ));
    }
    if spec.resource_only {
        if manifest.get("alwaysActivate").and_then(|v| v.as_bool()) != Some(true)
            || manifest
                .get("hmd_presence")
                .and_then(|v| v.as_array())
                .map(Vec::len)
                != Some(0)
            || root.join("bin").exists()
        {
            return Err("Galaxy XR companion must be resource-only, always-active, binary-free, and have empty hmd_presence".into());
        }
    }
    validate_json_tree(root, spec.name)?;
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

fn prepare_package(
    source_dir: &str,
    drivers_root: &Path,
    spec: PackageSpec,
    previous: Option<&InstallReceipt>,
) -> Result<PackageTransaction, String> {
    let source = fs::canonicalize(source_dir)
        .map_err(|e| format!("resolve {} source package: {e}", spec.name))?;
    validate_package(&source, spec)?;
    let source_tree = hash_tree(&source)?;
    let sha256 = tree_identity(&source_tree);
    let target = drivers_root.join(spec.name);
    let suffix = unique_suffix();
    let stage = drivers_root.join(format!(".{}.{suffix}.stage", spec.name));
    let backup = drivers_root.join(format!(".{}.{suffix}.backup", spec.name));

    if let Err(error) = copy_tree(&source, &stage) {
        let _ = fs::remove_dir_all(&stage);
        return Err(error);
    }
    if let Err(error) = validate_package(&stage, spec) {
        let _ = fs::remove_dir_all(&stage);
        return Err(error);
    }
    if hash_tree(&stage)? != source_tree {
        let _ = fs::remove_dir_all(&stage);
        return Err(format!(
            "staged {} tree does not match its source",
            spec.name
        ));
    }

    let had_target = target.exists();
    if had_target {
        let installed_sha = tree_identity(&hash_tree(&target)?);
        if !previous
            .map(|receipt| receipt.owns(spec, &target, &installed_sha))
            .unwrap_or(false)
        {
            let _ = fs::remove_dir_all(&stage);
            return Err(format!(
                "existing {} package is unowned or drifted; refusing replacement: {}",
                spec.name,
                target.display()
            ));
        }
        validate_package(&target, spec)?;
    }
    Ok(PackageTransaction {
        spec,
        target,
        backup,
        stage,
        had_target,
        activated: false,
        sha256,
        file_count: source_tree.len(),
    })
}

fn activate_package(transaction: &mut PackageTransaction) -> Result<(), String> {
    if transaction.had_target {
        fs::rename(&transaction.target, &transaction.backup)
            .map_err(|e| format!("backup installed {}: {e}", transaction.spec.name))?;
    }
    if let Err(error) = fs::rename(&transaction.stage, &transaction.target) {
        let restore = if transaction.had_target {
            fs::rename(&transaction.backup, &transaction.target).err()
        } else {
            None
        };
        return Err(format!(
            "activate staged {}: {error}; restore={restore:?}",
            transaction.spec.name
        ));
    }
    transaction.activated = true;
    validate_package(&transaction.target, transaction.spec).map_err(|error| {
        format!(
            "activated {} validation failed: {error}",
            transaction.spec.name
        )
    })
}

fn rollback_packages(transactions: &[PackageTransaction]) -> Vec<String> {
    transactions
        .iter()
        .rev()
        .filter_map(|transaction| {
            let result = if transaction.activated {
                rollback_driver(
                    &transaction.target,
                    &transaction.backup,
                    transaction.had_target,
                )
            } else if transaction.stage.exists() {
                fs::remove_dir_all(&transaction.stage)
                    .map_err(|error| format!("remove prepared stage: {error}"))
            } else {
                Ok(())
            };
            result
                .err()
                .map(|error| format!("{}: {error}", transaction.spec.name))
        })
        .collect()
}

#[tauri::command]
pub fn verify_driver_install(steamvr_dir: String) -> Result<bool, String> {
    let Some(receipt) = load_receipt()? else {
        return Ok(false);
    };
    let drivers_root = Path::new(&steamvr_dir).join("drivers");
    for spec in [ACTIVE_SPEC, RESOURCE_SPEC] {
        let target = drivers_root.join(spec.name);
        let Some(package) = receipt.package(spec.name) else {
            return Ok(false);
        };
        if !target.is_dir()
            || package.path != target.to_string_lossy()
            || validate_package(&target, spec).is_err()
            || tree_identity(&hash_tree(&target)?) != package.sha256
        {
            return Ok(false);
        }
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
    resource_source_dir: String,
    steamvr_dir: String,
    vrcft_module_path: Option<String>,
) -> Result<InstallReceipt, String> {
    let previous_receipt = load_receipt()?;
    let drivers_root = Path::new(&steamvr_dir).join("drivers");
    fs::create_dir_all(&drivers_root)
        .map_err(|e| format!("create SteamVR drivers directory: {e}"))?;
    let mut packages = Vec::new();
    for (source, spec) in [
        (&source_dir, ACTIVE_SPEC),
        (&resource_source_dir, RESOURCE_SPEC),
    ] {
        match prepare_package(source, &drivers_root, spec, previous_receipt.as_ref()) {
            Ok(transaction) => packages.push(transaction),
            Err(error) => {
                let rollback = rollback_packages(&packages);
                return Err(format!(
                    "install {} failed: {error}; rollback={rollback:?}",
                    spec.name
                ));
            }
        }
    }
    for index in 0..packages.len() {
        if let Err(error) = activate_package(&mut packages[index]) {
            let rollback = rollback_packages(&packages);
            return Err(format!(
                "activate packages failed: {error}; rollback={rollback:?}"
            ));
        }
    }
    let module_transaction =
        match stage_vrcft_module(vrcft_module_path.as_deref(), previous_receipt.as_ref()) {
            Ok(transaction) => transaction,
            Err(error) => {
                let rollback = rollback_packages(&packages);
                return Err(format!(
                    "VRCFT module installation failed: {error}; package rollback={rollback:?}"
                ));
            }
        };
    let retained_module = if module_transaction.is_none() {
        previous_receipt.as_ref().and_then(|previous| {
            previous.vrcft_module_path.as_ref().zip(previous.vrcft_module_sha256.as_ref())
        }).map(|(path, sha256)| -> Result<(String, String), String> {
            let module = Path::new(path);
            if !module.is_file() || hash_file(module)? != *sha256 {
                return Err("previously managed VRCFT module is missing or drifted; refusing to drop its ownership".into());
            }
            Ok((path.clone(), sha256.clone()))
        }).transpose()
    } else {
        Ok(None)
    };
    let retained_module = match retained_module {
        Ok(value) => value,
        Err(error) => {
            let package_rollback = rollback_packages(&packages);
            return Err(format!(
                "preserve VRCFT ownership: {error}; package rollback={package_rollback:?}"
            ));
        }
    };
    let receipt = InstallReceipt {
        schema_version: receipt_schema_version(),
        packages: packages
            .iter()
            .map(|transaction| PackageReceipt {
                name: transaction.spec.name.into(),
                path: transaction.target.to_string_lossy().into_owned(),
                sha256: transaction.sha256.clone(),
                file_count: transaction.file_count,
            })
            .collect(),
        driver_path: None,
        package_sha256: None,
        file_count: None,
        vrcft_module_installed: module_transaction.is_some() || retained_module.is_some(),
        vrcft_module_path: module_transaction
            .as_ref()
            .map(|transaction| transaction.target.to_string_lossy().into_owned())
            .or_else(|| retained_module.as_ref().map(|(path, _)| path.clone())),
        vrcft_module_sha256: module_transaction
            .as_ref()
            .map(|transaction| transaction.sha256.clone())
            .or_else(|| retained_module.as_ref().map(|(_, hash)| hash.clone())),
    };
    if let Err(error) = write_receipt(&receipt) {
        let module_rollback = module_transaction
            .as_ref()
            .and_then(|transaction| rollback_module(transaction).err());
        let package_rollback = rollback_packages(&packages);
        return Err(format!(
            "commit install receipt: {error}; module rollback={module_rollback:?}; package rollback={package_rollback:?}"
        ));
    }
    for transaction in &packages {
        if transaction.had_target {
            let _ = fs::remove_dir_all(&transaction.backup);
        }
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
        let output = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("..")
            .join("output");
        let active = output.join(ACTIVE_DRIVER_NAME);
        let resources = output.join(RESOURCE_DRIVER_NAME);
        assert!(
            active.is_dir() && resources.is_dir(),
            "build both driver packages first"
        );
        validate_package(&active, ACTIVE_SPEC).expect("active package validation failed");
        validate_package(&resources, RESOURCE_SPEC).expect("resource package validation failed");
    }

    #[test]
    fn cleanup_identity_accepts_only_valid_allowlisted_packages() {
        let root = std::env::temp_dir().join(format!("galaxyxr-cleanup-test-{}", unique_suffix()));
        let active = root.join(ACTIVE_DRIVER_NAME);
        let resources = root.join(RESOURCE_DRIVER_NAME);
        let unrelated = root.join("UnrelatedDriver");

        fs::create_dir_all(active.join("bin").join("win64")).unwrap();
        fs::write(
            active.join("driver.vrdrivermanifest"),
            r#"{"name":"CustomHeadsetOpenVR"}"#,
        )
        .unwrap();
        fs::write(
            active
                .join("bin")
                .join("win64")
                .join("driver_CustomHeadsetOpenVR.dll"),
            b"test",
        )
        .unwrap();

        fs::create_dir_all(&resources).unwrap();
        fs::write(
            resources.join("driver.vrdrivermanifest"),
            r#"{"name":"galaxyxrresources","resourceOnly":true}"#,
        )
        .unwrap();

        fs::create_dir_all(&unrelated).unwrap();
        fs::write(
            unrelated.join("driver.vrdrivermanifest"),
            r#"{"name":"UnrelatedDriver"}"#,
        )
        .unwrap();

        assert_eq!(
            cleanup_manifest_identity(&active, Some(ACTIVE_DRIVER_NAME)).unwrap(),
            ACTIVE_DRIVER_NAME
        );
        assert_eq!(
            cleanup_manifest_identity(&resources, Some(RESOURCE_DRIVER_NAME)).unwrap(),
            RESOURCE_DRIVER_NAME
        );
        assert!(cleanup_manifest_identity(&active, Some(RESOURCE_DRIVER_NAME)).is_err());
        assert!(cleanup_manifest_identity(&unrelated, None).is_err());

        fs::create_dir_all(resources.join("bin")).unwrap();
        assert!(cleanup_manifest_identity(&resources, Some(RESOURCE_DRIVER_NAME)).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}

#[tauri::command]
pub fn uninstall_driver_transactional(steamvr_dir: String) -> Result<bool, String> {
    let receipt =
        load_receipt()?.ok_or("managed install receipt is missing; refusing uninstall")?;
    let drivers_root = Path::new(&steamvr_dir).join("drivers");
    let mut targets = Vec::new();
    for spec in [ACTIVE_SPEC, RESOURCE_SPEC] {
        let target = drivers_root.join(spec.name);
        let Some(package) = receipt.package(spec.name) else {
            return Err(format!(
                "receipt does not own required package {}; run repair before uninstall",
                spec.name
            ));
        };
        if !target.is_dir() || package.path != target.to_string_lossy() {
            return Err(format!(
                "managed target missing for {}: {}",
                spec.name,
                target.display()
            ));
        }
        validate_package(&target, spec)?;
        if tree_identity(&hash_tree(&target)?) != package.sha256 {
            return Err(format!(
                "installed {} drifted from its receipt; refusing uninstall",
                spec.name
            ));
        }
        targets.push((spec, target));
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
    let mut tombstones = Vec::new();
    for (spec, target) in &targets {
        let tombstone = drivers_root.join(format!(".{}.{}.remove", spec.name, unique_suffix()));
        if let Err(error) = fs::rename(target, &tombstone) {
            for (restore_target, detached) in tombstones.iter().rev() {
                let _ = fs::rename(detached, restore_target);
            }
            return Err(format!("detach installed {}: {error}", spec.name));
        }
        tombstones.push((target.clone(), tombstone));
    }
    let module_tombstone = module.as_ref().map(|path| {
        path.parent().unwrap().join(format!(
            ".GalaxyXR.VRCFaceTracking.{}.remove",
            unique_suffix()
        ))
    });
    if let (Some(module_path), Some(detached)) = (&module, &module_tombstone) {
        if let Err(error) = fs::rename(module_path, detached) {
            let restore: Vec<_> = tombstones
                .iter()
                .rev()
                .filter_map(|(target, tombstone)| fs::rename(tombstone, target).err())
                .collect();
            return Err(format!(
                "detach VRCFT module: {error}; package restore={restore:?}"
            ));
        }
    }
    if let Err(error) = fs::remove_file(receipt_path()?) {
        let package_restore: Vec<_> = tombstones
            .iter()
            .rev()
            .filter_map(|(target, tombstone)| fs::rename(tombstone, target).err())
            .collect();
        let module_restore =
            if let (Some(module_path), Some(detached)) = (&module, &module_tombstone) {
                fs::rename(detached, module_path).err()
            } else {
                None
            };
        return Err(format!(
            "remove install receipt: {error}; package restore={package_restore:?}; module restore={module_restore:?}"
        ));
    }
    // Detachment plus receipt removal is the uninstall commit. Tombstones are
    // now inert cleanup debt, so locked files must not turn a committed
    // uninstall into a reported rollback failure.
    for (_, tombstone) in &tombstones {
        let _ = fs::remove_dir_all(tombstone);
    }
    if let Some(detached) = &module_tombstone {
        let _ = fs::remove_file(detached);
    }
    Ok(true)
}

fn steamvr_process_running() -> bool {
    let mut system = System::new_all();
    system.refresh_processes(ProcessesToUpdate::All, false);
    system.processes().values().any(|process| {
        matches!(
            process
                .name()
                .to_string_lossy()
                .to_ascii_lowercase()
                .as_str(),
            "vrmonitor.exe"
                | "vrmonitor"
                | "vrserver.exe"
                | "vrserver"
                | "vrcompositor.exe"
                | "vrcompositor"
        )
    })
}

fn cleanup_manifest_identity(root: &Path, expected_name: Option<&str>) -> Result<String, String> {
    let metadata = fs::symlink_metadata(root)
        .map_err(|e| format!("inspect cleanup target {}: {e}", root.display()))?;
    reject_link(&metadata, root)?;
    if !metadata.is_dir() {
        return Err(format!(
            "cleanup target is not a directory: {}",
            root.display()
        ));
    }
    let manifest_path = root.join("driver.vrdrivermanifest");
    let manifest: serde_json::Value = serde_json::from_slice(
        &fs::read(&manifest_path)
            .map_err(|e| format!("read cleanup manifest {}: {e}", manifest_path.display()))?,
    )
    .map_err(|e| format!("parse cleanup manifest {}: {e}", manifest_path.display()))?;
    let name = manifest
        .get("name")
        .and_then(|value| value.as_str())
        .ok_or_else(|| format!("cleanup manifest has no name: {}", manifest_path.display()))?;
    if !matches!(
        name,
        ACTIVE_DRIVER_NAME | RESOURCE_DRIVER_NAME | "GalaxyXRNative"
    ) {
        return Err(format!(
            "cleanup manifest identity is not allowlisted: {name}"
        ));
    }
    if expected_name.is_some_and(|expected| expected != name) {
        return Err(format!(
            "cleanup path/manifest identity mismatch: expected {}, found {name}",
            expected_name.unwrap()
        ));
    }
    let resource_only = manifest
        .get("resourceOnly")
        .and_then(|value| value.as_bool())
        .unwrap_or(false);
    if (name == RESOURCE_DRIVER_NAME) != resource_only {
        return Err(format!("cleanup manifest resourceOnly mismatch for {name}"));
    }
    if resource_only {
        if root.join("bin").exists() {
            return Err(format!(
                "resource-only cleanup target contains binaries: {}",
                root.display()
            ));
        }
    } else {
        let win64 = root.join("bin").join("win64");
        let has_driver_dll = win64.is_dir()
            && fs::read_dir(&win64)
                .map_err(|e| format!("read cleanup DLL directory {}: {e}", win64.display()))?
                .filter_map(Result::ok)
                .any(|entry| {
                    let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
                    name.starts_with("driver_") && name.ends_with(".dll")
                });
        if !has_driver_dll {
            return Err(format!(
                "active cleanup target has no win64 driver DLL: {}",
                root.display()
            ));
        }
    }
    Ok(name.to_string())
}

fn cleanup_path_fingerprint(path: &Path) -> Result<String, String> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|e| format!("inspect cleanup path {}: {e}", path.display()))?;
    reject_link(&metadata, path)?;
    if metadata.is_dir() {
        Ok(tree_identity(&hash_tree(path)?))
    } else if metadata.is_file() {
        hash_file(path)
    } else {
        Err(format!("unsupported cleanup target: {}", path.display()))
    }
}

fn same_canonical_path(left: &Path, right: &Path) -> bool {
    match (fs::canonicalize(left), fs::canonicalize(right)) {
        (Ok(left), Ok(right)) => left == right,
        _ => false,
    }
}

fn push_cleanup_target(
    targets: &mut Vec<PathBuf>,
    actions: &mut Vec<String>,
    token_entries: &mut Vec<String>,
    path: PathBuf,
    description: String,
) -> Result<(), String> {
    if targets
        .iter()
        .any(|existing| same_canonical_path(existing, &path))
    {
        return Ok(());
    }
    let fingerprint = cleanup_path_fingerprint(&path)?;
    token_entries.push(format!("remove|{}|{fingerprint}", path.display()));
    actions.push(description);
    targets.push(path);
    Ok(())
}

fn build_cleanup_plan(steamvr_dir: &str) -> Result<CleanupPlan, String> {
    let steamvr_root =
        fs::canonicalize(steamvr_dir).map_err(|e| format!("resolve SteamVR directory: {e}"))?;
    let drivers_root = steamvr_root.join("drivers");
    if !drivers_root.is_dir() {
        return Err(format!(
            "SteamVR drivers directory is missing: {}",
            drivers_root.display()
        ));
    }
    let vrpathreg = steamvr_root.join("bin").join("win64").join("vrpathreg.exe");
    let mut actions = Vec::new();
    let mut preserved = vec![
        "SteamVR settings are preserved".to_string(),
        "Galaxy XR settings, pairing keys, and pairing manifests are preserved".to_string(),
        "driver_vrlink and unrelated SteamVR drivers are preserved".to_string(),
    ];
    let mut blockers = Vec::new();
    let mut targets = Vec::new();
    let mut registrations = Vec::new();
    let mut token_entries = Vec::new();

    for name in [ACTIVE_DRIVER_NAME, RESOURCE_DRIVER_NAME, "GalaxyXRNative"] {
        let target = drivers_root.join(name);
        if !target.exists() {
            continue;
        }
        match cleanup_manifest_identity(&target, Some(name)) {
            Ok(_) => {
                if let Err(error) = push_cleanup_target(
                    &mut targets,
                    &mut actions,
                    &mut token_entries,
                    target.clone(),
                    format!("Remove installed {name} package: {}", target.display()),
                ) {
                    blockers.push(error);
                }
            }
            Err(error) => blockers.push(error),
        }
    }

    let local_appdata = std::env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is unavailable")?;
    let local_appdata = PathBuf::from(local_appdata);
    let openvrpaths_path = local_appdata.join("openvr").join("openvrpaths.vrpath");
    if openvrpaths_path.is_file() {
        let value: serde_json::Value = serde_json::from_slice(
            &fs::read(&openvrpaths_path)
                .map_err(|e| format!("read {}: {e}", openvrpaths_path.display()))?,
        )
        .map_err(|e| format!("parse {}: {e}", openvrpaths_path.display()))?;
        for registered in value
            .get("external_drivers")
            .and_then(|value| value.as_array())
            .into_iter()
            .flatten()
            .filter_map(|value| value.as_str())
        {
            let path = PathBuf::from(registered);
            match cleanup_manifest_identity(&path, None) {
                Ok(name) => match fs::canonicalize(&path) {
                    Ok(canonical) => {
                        let fingerprint = cleanup_path_fingerprint(&canonical)?;
                        token_entries
                            .push(format!("unregister|{}|{fingerprint}", canonical.display()));
                        actions.push(format!(
                            "Unregister exact {name} path: {}",
                            canonical.display()
                        ));
                        registrations.push(canonical);
                    }
                    Err(error) => blockers.push(format!(
                        "resolve registered cleanup path {}: {error}",
                        path.display()
                    )),
                },
                Err(_) => preserved.push(format!(
                    "Unrelated external driver preserved: {}",
                    path.display()
                )),
            }
        }
    }
    if !registrations.is_empty() && !vrpathreg.is_file() {
        blockers.push(format!("vrpathreg is missing: {}", vrpathreg.display()));
    }

    let appdata = PathBuf::from(std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?);
    let expected_vrcft_module = appdata
        .join("VRCFaceTracking")
        .join("CustomLibs")
        .join("GalaxyXR.VRCFaceTracking.dll");
    let receipt = load_receipt()?;
    let receipt_file = receipt_path()?;
    if let Some(receipt) = receipt.as_ref() {
        for spec in [ACTIVE_SPEC, RESOURCE_SPEC] {
            let target = drivers_root.join(spec.name);
            let Some(package) = receipt.package(spec.name) else {
                blockers.push(format!(
                    "managed receipt does not own required package {}",
                    spec.name
                ));
                continue;
            };
            if !target.is_dir() {
                blockers.push(format!("managed package is missing: {}", target.display()));
                continue;
            }
            match cleanup_path_fingerprint(&target) {
                Ok(hash) if package.path == target.to_string_lossy() && package.sha256 == hash => {}
                Ok(_) => blockers.push(format!(
                    "managed package drifted; refusing cleanup: {}",
                    target.display()
                )),
                Err(error) => blockers.push(error),
            }
        }
        if let Some(module_path) = receipt.vrcft_module_path.as_deref() {
            let module = PathBuf::from(module_path);
            let expected = receipt.vrcft_module_sha256.as_deref();
            match (
                module.is_file(),
                expected,
                cleanup_path_fingerprint(&module),
            ) {
                (true, Some(expected), Ok(actual))
                    if expected == actual
                        && same_canonical_path(&module, &expected_vrcft_module) =>
                {
                    if let Err(error) = push_cleanup_target(
                        &mut targets,
                        &mut actions,
                        &mut token_entries,
                        module.clone(),
                        format!("Remove receipt-owned VRCFT module: {}", module.display()),
                    ) {
                        blockers.push(error);
                    }
                }
                _ => blockers.push(format!(
                    "receipt-owned VRCFT module is missing or drifted: {}",
                    module.display()
                )),
            }
        }
        if receipt_file.is_file() {
            if let Err(error) = push_cleanup_target(
                &mut targets,
                &mut actions,
                &mut token_entries,
                receipt_file.clone(),
                format!("Remove managed install receipt: {}", receipt_file.display()),
            ) {
                blockers.push(error);
            }
        }
    } else {
        if expected_vrcft_module.is_file() {
            preserved.push(format!(
                "Unowned VRCFT module preserved: {}",
                expected_vrcft_module.display()
            ));
        }
    }

    let legacy_root = local_appdata.join("CustomHeadsetOpenVR");
    let active_path = legacy_root.join("driver").join("active.json");
    if active_path.is_file() {
        let active: serde_json::Value = serde_json::from_slice(
            &fs::read(&active_path).map_err(|e| format!("read {}: {e}", active_path.display()))?,
        )
        .map_err(|e| format!("parse {}: {e}", active_path.display()))?;
        let driver_path = active.get("driverPath").and_then(|value| value.as_str());
        let expected_hash = active
            .get("driverDllSha256")
            .and_then(|value| value.as_str());
        let journal_path = legacy_root.join("GalaxyXR").join("install-state.json");
        let journal: Option<serde_json::Value> = if journal_path.is_file() {
            Some(
                serde_json::from_slice(
                    &fs::read(&journal_path)
                        .map_err(|e| format!("read {}: {e}", journal_path.display()))?,
                )
                .map_err(|e| format!("parse {}: {e}", journal_path.display()))?,
            )
        } else {
            None
        };
        let proven = driver_path
            .zip(expected_hash)
            .and_then(|(driver_path, expected_hash)| {
                let driver = PathBuf::from(driver_path);
                let versions = legacy_root.join("driver").join("versions");
                let leaf = driver.file_name()?.to_string_lossy();
                let token_ok =
                    leaf.len() == 64 && leaf.bytes().all(|byte| byte.is_ascii_hexdigit());
                let contained = driver
                    .parent()
                    .is_some_and(|parent| same_canonical_path(parent, &versions));
                let dll = driver
                    .join("bin")
                    .join("win64")
                    .join("driver_CustomHeadsetOpenVR.dll");
                let hash_ok = hash_file(&dll).ok().is_some_and(|hash| {
                    hash.eq_ignore_ascii_case(expected_hash) && hash.eq_ignore_ascii_case(&leaf)
                });
                let journal_ok = journal.as_ref().is_some_and(|journal| {
                    journal.get("status").and_then(|value| value.as_str()) == Some("Successful")
                        && journal
                            .get("driverRegistrationPath")
                            .and_then(|value| value.as_str())
                            .is_some_and(|path| same_canonical_path(Path::new(path), &driver))
                });
                (token_ok && contained && hash_ok && journal_ok).then_some(driver)
            });
        if let Some(driver) = proven {
            if let Err(error) = cleanup_manifest_identity(&driver, Some(ACTIVE_DRIVER_NAME)) {
                blockers.push(error);
            } else {
                if let Err(error) = push_cleanup_target(
                    &mut targets,
                    &mut actions,
                    &mut token_entries,
                    driver.clone(),
                    format!("Remove proven legacy staged driver: {}", driver.display()),
                ) {
                    blockers.push(error);
                }
                if let Err(error) = push_cleanup_target(
                    &mut targets,
                    &mut actions,
                    &mut token_entries,
                    active_path.clone(),
                    format!("Remove legacy active pointer: {}", active_path.display()),
                ) {
                    blockers.push(error);
                }
                preserved.push(format!(
                    "Legacy recovery ledger/backups preserved: {}",
                    journal_path.display()
                ));
            }
        } else {
            blockers.push(format!(
                "legacy active driver could not be proven safe to clean: {}",
                active_path.display()
            ));
        }
    }

    actions.sort();
    preserved.sort();
    blockers.sort();
    token_entries.sort();
    let mut hasher = Sha256::new();
    for entry in token_entries {
        hasher.update(entry.as_bytes());
        hasher.update([b'\n']);
    }
    let plan_token = format!("{:x}", hasher.finalize());
    Ok(CleanupPlan {
        preview: CleanupPreview {
            plan_token,
            actions,
            preserved,
            blockers,
            steam_vr_running: steamvr_process_running(),
        },
        targets,
        registrations,
        vrpathreg,
    })
}

#[tauri::command]
pub fn preview_driver_cleanup(steamvr_dir: String) -> Result<CleanupPreview, String> {
    Ok(build_cleanup_plan(&steamvr_dir)?.preview)
}

fn rollback_cleanup(
    vrpathreg: &Path,
    detached: &[(PathBuf, PathBuf)],
    unregistered: &[PathBuf],
) -> Vec<String> {
    let mut errors = Vec::new();
    for (original, detached_path) in detached.iter().rev() {
        if let Err(error) = fs::rename(detached_path, original) {
            errors.push(format!(
                "restore {} from {}: {error}",
                original.display(),
                detached_path.display()
            ));
        }
    }
    for removed in unregistered.iter().rev() {
        match Command::new(vrpathreg)
            .arg("adddriver")
            .arg(removed)
            .status()
        {
            Ok(status) if status.success() => {}
            Ok(status) => errors.push(format!(
                "restore registration {}: vrpathreg exited with {status}",
                removed.display()
            )),
            Err(error) => errors.push(format!(
                "restore registration {}: {error}",
                removed.display()
            )),
        }
    }
    errors
}

#[tauri::command]
pub fn execute_driver_cleanup(
    steamvr_dir: String,
    plan_token: String,
) -> Result<CleanupReport, String> {
    let plan = build_cleanup_plan(&steamvr_dir)?;
    if plan.preview.steam_vr_running {
        return Err("SteamVR is running. Close SteamVR before cleaning installations.".into());
    }
    if !plan.preview.blockers.is_empty() {
        return Err(format!(
            "cleanup is blocked:\n{}",
            plan.preview.blockers.join("\n")
        ));
    }
    if plan.preview.plan_token != plan_token {
        return Err("cleanup plan changed after preview; preview again before continuing".into());
    }

    let mut unregistered = Vec::new();
    for registration in &plan.registrations {
        let status = Command::new(&plan.vrpathreg)
            .arg("removedriver")
            .arg(registration)
            .status();
        let status = match status {
            Ok(status) => status,
            Err(error) => {
                let rollback_errors = rollback_cleanup(&plan.vrpathreg, &[], &unregistered);
                return Err(format!(
                    "run vrpathreg for {}: {error}; rollback errors: {:?}",
                    registration.display(),
                    rollback_errors
                ));
            }
        };
        if !status.success() {
            let rollback_errors = rollback_cleanup(&plan.vrpathreg, &[], &unregistered);
            return Err(format!(
                "failed to unregister exact driver path: {}; rollback errors: {:?}",
                registration.display(),
                rollback_errors
            ));
        }
        unregistered.push(registration.clone());
    }

    let mut detached: Vec<(PathBuf, PathBuf)> = Vec::new();
    for target in &plan.targets {
        let file_name = target
            .file_name()
            .ok_or_else(|| format!("cleanup target has no file name: {}", target.display()))?
            .to_string_lossy();
        let tombstone = target
            .parent()
            .ok_or_else(|| format!("cleanup target has no parent: {}", target.display()))?
            .join(format!(".{file_name}.{}.cleanup", unique_suffix()));
        if let Err(error) = fs::rename(target, &tombstone) {
            let rollback_errors = rollback_cleanup(&plan.vrpathreg, &detached, &unregistered);
            return Err(format!(
                "detach cleanup target {}: {error}; rollback errors: {:?}",
                target.display(),
                rollback_errors
            ));
        }
        detached.push((target.clone(), tombstone));
    }

    let mut warnings = Vec::new();
    for (_, tombstone) in &detached {
        let result = if tombstone.is_dir() {
            fs::remove_dir_all(tombstone)
        } else {
            fs::remove_file(tombstone)
        };
        if let Err(error) = result {
            warnings.push(format!(
                "cleanup debt remains at {}: {error}",
                tombstone.display()
            ));
        }
    }
    Ok(CleanupReport {
        removed: detached
            .into_iter()
            .map(|(original, _)| original.to_string_lossy().into_owned())
            .collect(),
        unregistered: unregistered
            .into_iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect(),
        preserved: plan.preview.preserved,
        warnings,
    })
}
