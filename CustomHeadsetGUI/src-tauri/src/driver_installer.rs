use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};
use sysinfo::{ProcessesToUpdate, System};

const ACTIVE_DRIVER_NAME: &str = "CustomHeadsetOpenVR";
const RESOURCE_DRIVER_NAME: &str = "galaxyxrresources";
static JSON_WRITE_LOCK: Mutex<()> = Mutex::new(());
const ACTIVE_REQUIRED_FILES: &[&str] = &[
    "driver.vrdrivermanifest",
    "bin/win64/driver_CustomHeadsetOpenVR.dll",
    "resources/driver.vrresources",
    "resources/settings/default.vrsettings",
    "resources/shaders/d3d11/vrlink_layer_ps.hlsl",
    "resources/shaders/d3d11/vrlink_fxaa_ps.hlsl",
    "resources/rendermodels/vst_controller_left/vst_controller_left.obj",
    "resources/rendermodels/vst_controller_right/vst_controller_right.obj",
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
    #[serde(default = "legacy_receipt_schema_version")]
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
    #[serde(default)]
    vrcft_module_installed: bool,
    vrcft_module_path: Option<String>,
    vrcft_module_sha256: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallPreflight {
    packages: Vec<PackageReceipt>,
    managed_root: String,
    receipt_migration: Option<String>,
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
    4
}

fn legacy_receipt_schema_version() -> u32 {
    1
}

struct LoadedReceipt {
    receipt: InstallReceipt,
    source_path: PathBuf,
    legacy: bool,
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

fn receipt_path_from(appdata: &Path) -> PathBuf {
    appdata
        .join("GalaxyXR")
        .join("CustomHeadset")
        .join("install-state.json")
}

fn legacy_receipt_path_from(appdata: &Path) -> PathBuf {
    appdata.join("CustomHeadset").join("install-state.json")
}

fn receipt_path() -> Result<PathBuf, String> {
    let appdata = std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?;
    Ok(receipt_path_from(&PathBuf::from(appdata)))
}

fn legacy_receipt_path() -> Result<PathBuf, String> {
    let appdata = std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?;
    Ok(legacy_receipt_path_from(&PathBuf::from(appdata)))
}

fn expected_vrcft_module_path_from(appdata: &Path) -> PathBuf {
    appdata
        .join("VRCFaceTracking")
        .join("CustomLibs")
        .join("GalaxyXR.VRCFaceTracking.dll")
}

fn expected_vrcft_module_path() -> Result<PathBuf, String> {
    let appdata = std::env::var_os("APPDATA").ok_or("APPDATA is unavailable")?;
    Ok(expected_vrcft_module_path_from(&PathBuf::from(appdata)))
}

fn managed_drivers_root() -> Result<PathBuf, String> {
    let local_appdata = std::env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is unavailable")?;
    Ok(PathBuf::from(local_appdata)
        .join("GalaxyXR")
        .join("CustomHeadset")
        .join("DriverPackages"))
}

fn vrpathreg_path(steamvr_dir: &str) -> Result<PathBuf, String> {
    let path = Path::new(steamvr_dir)
        .join("bin")
        .join("win64")
        .join("vrpathreg.exe");
    if !path.is_file() {
        return Err(format!("vrpathreg is missing: {}", path.display()));
    }
    Ok(path)
}

fn openvrpaths_path() -> Result<PathBuf, String> {
    let local_appdata = std::env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is unavailable")?;
    Ok(PathBuf::from(local_appdata)
        .join("openvr")
        .join("openvrpaths.vrpath"))
}

fn registered_external_drivers_from(openvrpaths: &Path) -> Result<Vec<PathBuf>, String> {
    if !openvrpaths.is_file() {
        return Ok(Vec::new());
    }
    let value: serde_json::Value = serde_json::from_slice(
        &fs::read(&openvrpaths).map_err(|e| format!("read {}: {e}", openvrpaths.display()))?,
    )
    .map_err(|e| format!("parse {}: {e}", openvrpaths.display()))?;
    let Some(external_drivers) = value.get("external_drivers") else {
        return Ok(Vec::new());
    };
    let external_drivers = external_drivers.as_array().ok_or_else(|| {
        format!(
            "{} has a non-array external_drivers value",
            openvrpaths.display()
        )
    })?;
    external_drivers
        .iter()
        .map(|value| {
            value.as_str().map(PathBuf::from).ok_or_else(|| {
                format!(
                    "{} contains a non-string driver path",
                    openvrpaths.display()
                )
            })
        })
        .collect()
}

fn registered_external_drivers() -> Result<Vec<PathBuf>, String> {
    registered_external_drivers_from(&openvrpaths_path()?)
}

fn is_exact_driver_registered(target: &Path) -> Result<bool, String> {
    Ok(registered_external_drivers()?
        .iter()
        .any(|registered| same_canonical_path(registered, target)))
}

fn run_vrpathreg(vrpathreg: &Path, action: &str, target: &Path) -> Result<(), String> {
    let output = Command::new(vrpathreg)
        .arg(action)
        .arg(target)
        .output()
        .map_err(|e| format!("run vrpathreg {action} {}: {e}", target.display()))?;
    if !output.status.success() {
        return Err(format!(
            "vrpathreg {action} failed for {} ({}): {}{}",
            target.display(),
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(())
}

fn set_exact_driver_registration(
    vrpathreg: &Path,
    target: &Path,
    registered: bool,
) -> Result<bool, String> {
    let was_registered = is_exact_driver_registered(target)?;
    if was_registered == registered {
        return Ok(false);
    }
    run_vrpathreg(
        vrpathreg,
        if registered {
            "adddriver"
        } else {
            "removedriver"
        },
        target,
    )?;
    if is_exact_driver_registered(target)? != registered {
        let rollback = run_vrpathreg(
            vrpathreg,
            if was_registered {
                "adddriver"
            } else {
                "removedriver"
            },
            target,
        )
        .err();
        return Err(format!(
            "vrpathreg did not {} exact path {}; immediate rollback={rollback:?}",
            if registered { "register" } else { "unregister" },
            target.display()
        ));
    }
    Ok(true)
}

fn read_receipt(path: &Path) -> Result<InstallReceipt, String> {
    serde_json::from_slice(&fs::read(path).map_err(|e| format!("read {}: {e}", path.display()))?)
        .map_err(|e| format!("parse {}: {e}", path.display()))
}

fn validate_receipt_shape(receipt: &InstallReceipt, legacy_source: bool) -> Result<(), String> {
    let vrcft_fields_complete = receipt.vrcft_module_path.is_some()
        && receipt.vrcft_module_sha256.is_some()
        && receipt.vrcft_module_installed;
    let vrcft_fields_empty = receipt.vrcft_module_path.is_none()
        && receipt.vrcft_module_sha256.is_none()
        && !receipt.vrcft_module_installed;
    if !vrcft_fields_complete && !vrcft_fields_empty {
        return Err("install receipt has inconsistent VRCFT ownership fields".into());
    }
    if receipt.schema_version == 1 {
        if !legacy_source
            || !receipt.packages.is_empty()
            || receipt.driver_path.is_none()
            || receipt.package_sha256.is_none()
            || receipt.file_count.is_none()
        {
            return Err(
                "v1 receipt is accepted only as an explicit generic legacy active-package receipt"
                    .into(),
            );
        }
        return Ok(());
    }
    if receipt.driver_path.is_some()
        || receipt.package_sha256.is_some()
        || receipt.file_count.is_some()
    {
        return Err("managed receipt contains legacy package fields".into());
    }
    let mut names: Vec<_> = receipt
        .packages
        .iter()
        .map(|package| package.name.as_str())
        .collect();
    names.sort_unstable();
    let mut expected = match receipt.schema_version {
        2 => vec![ACTIVE_DRIVER_NAME, RESOURCE_DRIVER_NAME],
        3 if vrcft_fields_empty => vec![RESOURCE_DRIVER_NAME],
        3 => return Err("v3 resource-only receipt cannot own a VRCFT module".into()),
        4 if vrcft_fields_empty => vec![ACTIVE_DRIVER_NAME, RESOURCE_DRIVER_NAME],
        4 => return Err("v4 receipt cannot own a VRCFT module".into()),
        version => return Err(format!("unsupported install receipt schema {version}")),
    };
    expected.sort_unstable();
    if names != expected {
        return Err(format!(
            "v{} receipt package identities are incomplete or duplicated",
            receipt.schema_version
        ));
    }
    Ok(())
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
                Some("json" | "vrsettings" | "vrresources")
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

fn validate_receipt_vrcft_target_at(
    receipt: &InstallReceipt,
    expected_target: &Path,
) -> Result<Option<PathBuf>, String> {
    validate_receipt_shape(receipt, receipt.schema_version == 1)?;
    let Some(path) = receipt.vrcft_module_path.as_deref() else {
        return Ok(None);
    };
    let expected_hash = receipt
        .vrcft_module_sha256
        .as_deref()
        .ok_or("VRCFT receipt hash is missing")?;
    let target = PathBuf::from(path);
    if !target.is_file()
        || !expected_target.is_file()
        || !same_canonical_path(&target, &expected_target)
    {
        return Err(format!(
            "receipt VRCFT path is not the exact managed module target: {}",
            target.display()
        ));
    }
    if hash_file(&target)? != expected_hash {
        return Err("receipt-owned VRCFT module is drifted".into());
    }
    Ok(Some(target))
}

fn validate_receipt_vrcft_target(receipt: &InstallReceipt) -> Result<Option<PathBuf>, String> {
    validate_receipt_vrcft_target_at(receipt, &expected_vrcft_module_path()?)
}

fn validate_receipt_package_target(
    receipt: &InstallReceipt,
    spec: PackageSpec,
    steamvr_dir: &str,
    legacy_source: bool,
) -> Result<Option<PathBuf>, String> {
    let (target, expected_hash, expected_count) = if receipt.schema_version == 1 {
        if spec.name != ACTIVE_DRIVER_NAME {
            return Ok(None);
        }
        (
            PathBuf::from(
                receipt
                    .driver_path
                    .as_deref()
                    .ok_or("v1 driverPath is missing")?,
            ),
            receipt
                .package_sha256
                .as_deref()
                .ok_or("v1 packageSha256 is missing")?,
            receipt.file_count.ok_or("v1 fileCount is missing")?,
        )
    } else {
        let package = receipt
            .package(spec.name)
            .ok_or_else(|| format!("receipt does not own required package {}", spec.name))?;
        (
            PathBuf::from(&package.path),
            package.sha256.as_str(),
            package.file_count,
        )
    };
    if !target.is_dir() {
        return Err(format!(
            "receipt-owned package is missing for {}: {}",
            spec.name,
            target.display()
        ));
    }
    let legacy_target = Path::new(steamvr_dir).join("drivers").join(spec.name);
    let location_ok = if legacy_source {
        same_canonical_path(&target, &legacy_target)
    } else {
        let managed_target = managed_drivers_root()?.join(spec.name);
        same_canonical_path(&target, &managed_target)
    };
    if !location_ok {
        return Err(format!(
            "receipt path is outside the expected {} package location for {}: {}",
            if legacy_source {
                "legacy"
            } else {
                "Galaxy XR managed"
            },
            spec.name,
            target.display()
        ));
    }
    validate_package(&target, spec)?;
    let tree = hash_tree(&target)?;
    if tree_identity(&tree) != expected_hash || tree.len() != expected_count {
        return Err(format!(
            "receipt-owned package drifted for {}: {}",
            spec.name,
            target.display()
        ));
    }
    Ok(Some(target))
}

fn receipt_package_specs(schema_version: u32) -> &'static [PackageSpec] {
    match schema_version {
        1 => &[ACTIVE_SPEC],
        2 => &[ACTIVE_SPEC, RESOURCE_SPEC],
        3 => &[RESOURCE_SPEC],
        4 => &[ACTIVE_SPEC, RESOURCE_SPEC],
        _ => unreachable!("receipt schema must be validated before selecting packages"),
    }
}

fn receipt_owned_package_targets(
    receipt: &InstallReceipt,
    steamvr_dir: &str,
    legacy_source: bool,
) -> Result<Vec<(PackageSpec, PathBuf)>, String> {
    validate_receipt_shape(receipt, legacy_source)?;
    let mut targets = Vec::new();
    for &spec in receipt_package_specs(receipt.schema_version) {
        if let Some(path) =
            validate_receipt_package_target(receipt, spec, steamvr_dir, legacy_source)?
        {
            targets.push((spec, path));
        }
    }
    validate_receipt_vrcft_target(receipt)?;
    Ok(targets)
}

fn load_receipt_for_context(steamvr_dir: &str) -> Result<Option<LoadedReceipt>, String> {
    let current_path = receipt_path()?;
    if current_path.is_file() {
        let receipt = read_receipt(&current_path)?;
        receipt_owned_package_targets(&receipt, steamvr_dir, false)?;
        return Ok(Some(LoadedReceipt {
            receipt,
            source_path: current_path,
            legacy: false,
        }));
    }
    let legacy_path = legacy_receipt_path()?;
    if !legacy_path.is_file() {
        return Ok(None);
    }
    let receipt = read_receipt(&legacy_path)?;
    receipt_owned_package_targets(&receipt, steamvr_dir, true).map_err(|error| {
        format!(
            "generic CustomHeadset receipt is not explicit Galaxy XR ownership and will not be migrated: {error}"
        )
    })?;
    Ok(Some(LoadedReceipt {
        receipt,
        source_path: legacy_path,
        legacy: true,
    }))
}

fn rollback_registration_changes(
    vrpathreg: &Path,
    added: &[PathBuf],
    removed: &[PathBuf],
) -> Vec<String> {
    let mut errors = Vec::new();
    for target in removed.iter().rev() {
        if let Err(error) = set_exact_driver_registration(vrpathreg, target, true) {
            errors.push(error);
        }
    }
    for target in added.iter().rev() {
        if let Err(error) = set_exact_driver_registration(vrpathreg, target, false) {
            errors.push(error);
        }
    }
    errors
}

fn restore_detached_packages(detached: &[(PathBuf, PathBuf)]) -> Vec<String> {
    detached
        .iter()
        .rev()
        .filter_map(|(target, tombstone)| {
            fs::rename(tombstone, target)
                .err()
                .map(|error| format!("restore {}: {error}", target.display()))
        })
        .collect()
}

fn reject_unowned_package_conflicts_with_state(
    steamvr_dir: &str,
    managed_root: &Path,
    previous_packages: &[(PackageSpec, PathBuf)],
    registered_drivers: &[PathBuf],
) -> Result<(), String> {
    let legacy_root = Path::new(steamvr_dir).join("drivers");
    for spec in [ACTIVE_SPEC, RESOURCE_SPEC] {
        let managed_target = managed_root.join(spec.name);
        if managed_target.exists()
            && !previous_packages
                .iter()
                .any(|(_, owned)| same_canonical_path(owned, &managed_target))
        {
            return Err(format!(
                "unowned managed {} package blocks install; move this folder out of the managed DriverPackages directory, then retry: {}",
                spec.name,
                managed_target.display()
            ));
        }
        let legacy_target = legacy_root.join(spec.name);
        if legacy_target.exists()
            && !previous_packages
                .iter()
                .any(|(_, owned)| same_canonical_path(owned, &legacy_target))
        {
            return Err(format!(
                "unowned legacy {} package blocks managed install; use Clean Existing Driver Installations first: {}",
                spec.name,
                legacy_target.display()
            ));
        }
    }
    for registered in registered_drivers {
        let Ok(name) = cleanup_manifest_identity(&registered, None) else {
            continue;
        };
        if !matches!(name.as_str(), ACTIVE_DRIVER_NAME | RESOURCE_DRIVER_NAME) {
            continue;
        }
        let expected_managed = managed_root.join(&name);
        let owned = previous_packages
            .iter()
            .any(|(_, path)| same_canonical_path(path, &registered));
        if !owned && !same_canonical_path(&registered, &expected_managed) {
            return Err(format!(
                "unowned external {name} registration blocks managed install; use Clean Existing Driver Installations first: {}",
                registered.display()
            ));
        }
    }
    Ok(())
}

fn reject_unowned_package_conflicts(
    steamvr_dir: &str,
    previous_packages: &[(PackageSpec, PathBuf)],
) -> Result<(), String> {
    reject_unowned_package_conflicts_with_state(
        steamvr_dir,
        &managed_drivers_root()?,
        previous_packages,
        &registered_external_drivers()?,
    )
}

fn inspect_source_package(source_dir: &str, spec: PackageSpec) -> Result<PackageReceipt, String> {
    let source = fs::canonicalize(source_dir)
        .map_err(|e| format!("resolve {} source package: {e}", spec.name))?;
    validate_package(&source, spec)?;
    let tree = hash_tree(&source)?;
    Ok(PackageReceipt {
        name: spec.name.into(),
        path: source.to_string_lossy().into_owned(),
        sha256: tree_identity(&tree),
        file_count: tree.len(),
    })
}

#[tauri::command]
pub fn preflight_driver_install(
    active_source_dir: String,
    resource_source_dir: String,
    steamvr_dir: String,
) -> Result<InstallPreflight, String> {
    if steamvr_process_running() {
        return Err("SteamVR is running. Close SteamVR before driver preflight.".into());
    }
    vrpathreg_path(&steamvr_dir)?;
    let openvrpaths = openvrpaths_path()?;
    if !openvrpaths.is_file() {
        return Err(format!(
            "OpenVR path registry is missing; start SteamVR once, close it, and retry: {}",
            openvrpaths.display()
        ));
    }
    let registered = registered_external_drivers_from(&openvrpaths)?;
    let loaded = load_receipt_for_context(&steamvr_dir)?;
    let previous_packages = loaded
        .as_ref()
        .map(|loaded| receipt_owned_package_targets(&loaded.receipt, &steamvr_dir, loaded.legacy))
        .transpose()?
        .unwrap_or_default();
    let managed_root = managed_drivers_root()?;
    reject_unowned_package_conflicts_with_state(
        &steamvr_dir,
        &managed_root,
        &previous_packages,
        &registered,
    )?;
    let packages = vec![
        inspect_source_package(&active_source_dir, ACTIVE_SPEC)?,
        inspect_source_package(&resource_source_dir, RESOURCE_SPEC)?,
    ];
    Ok(InstallPreflight {
        packages,
        managed_root: managed_root.to_string_lossy().into_owned(),
        receipt_migration: loaded
            .filter(|loaded| loaded.legacy)
            .map(|loaded| loaded.source_path.to_string_lossy().into_owned()),
    })
}

#[tauri::command]
pub fn verify_driver_install(steamvr_dir: String) -> Result<bool, String> {
    let Some(loaded) = load_receipt_for_context(&steamvr_dir)? else {
        return Ok(false);
    };
    if loaded.legacy {
        return Ok(false);
    }
    let receipt = loaded.receipt;
    let managed_root = managed_drivers_root()?;
    vrpathreg_path(&steamvr_dir)?;
    for spec in [ACTIVE_SPEC, RESOURCE_SPEC] {
        let target = managed_root.join(spec.name);
        let Some(package) = receipt.package(spec.name) else {
            return Ok(false);
        };
        if !target.is_dir()
            || package.path != target.to_string_lossy()
            || validate_package(&target, spec).is_err()
            || tree_identity(&hash_tree(&target)?) != package.sha256
            || !is_exact_driver_registered(&target)?
        {
            return Ok(false);
        }
    }
    validate_receipt_vrcft_target(&receipt)?;
    Ok(true)
}

#[tauri::command]
pub fn write_json_file_transactional(path: String, contents: String) -> Result<(), String> {
    write_json_file_transactional_checked(path, contents, None)
}

pub(crate) fn write_json_file_transactional_checked(
    path: String,
    contents: String,
    expected: Option<Option<&[u8]>>,
) -> Result<(), String> {
    let _guard = JSON_WRITE_LOCK
        .lock()
        .map_err(|_| "settings writer lock is poisoned")?;
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
        if let Err(error) = fs::rename(&target, &backup) {
            let _ = fs::remove_file(&stage);
            return Err(format!("backup SteamVR settings: {error}"));
        }
    }
    if let Some(expected_state) = expected {
        // Verify the exact directory entry displaced by this transaction,
        // closing the check/rename race with other processes.
        let actual = if had_target {
            match fs::read(&backup) {
                Ok(bytes) => Some(bytes),
                Err(error) => {
                    let restore = fs::rename(&backup, &target).err();
                    let _ = fs::remove_file(&stage);
                    return Err(format!(
                        "verify displaced settings: {error}; restore={restore:?}"
                    ));
                }
            }
        } else {
            None
        };
        let matches = match (expected_state, actual.as_deref()) {
            (Some(expected_bytes), Some(actual_bytes)) => expected_bytes == actual_bytes,
            (None, None) => true,
            _ => false,
        };
        if !matches {
            let restore = if had_target {
                fs::rename(&backup, &target).err()
            } else {
                None
            };
            let _ = fs::remove_file(&stage);
            return Err(format!(
                "settings changed before the transactional swap; retry the operation; restore={restore:?}"
            ));
        }
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
    active_source_dir: String,
    resource_source_dir: String,
    steamvr_dir: String,
) -> Result<InstallReceipt, String> {
    preflight_driver_install(
        active_source_dir.clone(),
        resource_source_dir.clone(),
        steamvr_dir.clone(),
    )?;
    let loaded_receipt = load_receipt_for_context(&steamvr_dir)?;
    let previous_receipt = loaded_receipt.as_ref().map(|loaded| loaded.receipt.clone());
    let previous_module = previous_receipt
        .as_ref()
        .map(validate_receipt_vrcft_target)
        .transpose()?
        .flatten();
    let vrpathreg = vrpathreg_path(&steamvr_dir)?;
    let drivers_root = managed_drivers_root()?;
    fs::create_dir_all(&drivers_root)
        .map_err(|e| format!("create managed driver package directory: {e}"))?;
    let previous_packages = loaded_receipt
        .as_ref()
        .map(|loaded| receipt_owned_package_targets(&loaded.receipt, &steamvr_dir, loaded.legacy))
        .transpose()?
        .unwrap_or_default();
    reject_unowned_package_conflicts(&steamvr_dir, &previous_packages)?;

    let active_package = prepare_package(
        &active_source_dir,
        &drivers_root,
        ACTIVE_SPEC,
        previous_receipt.as_ref(),
    )?;
    let resource_package = match prepare_package(
        &resource_source_dir,
        &drivers_root,
        RESOURCE_SPEC,
        previous_receipt.as_ref(),
    ) {
        Ok(package) => package,
        Err(error) => {
            let rollback = rollback_packages(std::slice::from_ref(&active_package));
            return Err(format!(
                "prepare resource package failed: {error}; rollback={rollback:?}"
            ));
        }
    };
    let mut packages = vec![active_package, resource_package];
    for index in 0..packages.len() {
        if let Err(error) = activate_package(&mut packages[index]) {
            let failed_name = packages[index].spec.name;
            let rollback = rollback_packages(&packages);
            return Err(format!(
                "activate {failed_name} package failed: {error}; rollback={rollback:?}"
            ));
        }
    }

    let mut registrations_added = Vec::new();
    for package in &packages {
        match set_exact_driver_registration(&vrpathreg, &package.target, true) {
            Ok(true) => registrations_added.push(package.target.clone()),
            Ok(false) => {}
            Err(error) => {
                let registration_rollback =
                    rollback_registration_changes(&vrpathreg, &registrations_added, &[]);
                let package_rollback = rollback_packages(&packages);
                return Err(format!(
                    "register {} package failed: {error}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}",
                    package.spec.name
                ));
            }
        }
    }

    let mut registrations_removed = Vec::new();
    for (_, previous_target) in &previous_packages {
        if packages
            .iter()
            .any(|package| same_canonical_path(&package.target, previous_target))
        {
            continue;
        }
        match set_exact_driver_registration(&vrpathreg, previous_target, false) {
            Ok(true) => registrations_removed.push(previous_target.clone()),
            Ok(false) => {}
            Err(error) => {
                let registration_rollback = rollback_registration_changes(
                    &vrpathreg,
                    &registrations_added,
                    &registrations_removed,
                );
                let package_rollback = rollback_packages(&packages);
                return Err(format!(
                    "unregister previous exact package path failed: {error}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}"
                ));
            }
        }
    }

    let mut detached_previous = Vec::new();
    for (_, previous_target) in &previous_packages {
        if packages
            .iter()
            .any(|package| same_canonical_path(&package.target, previous_target))
        {
            continue;
        }
        let file_name = previous_target.file_name().ok_or_else(|| {
            format!(
                "previous package has no file name: {}",
                previous_target.display()
            )
        })?;
        let tombstone = previous_target
            .parent()
            .ok_or_else(|| {
                format!(
                    "previous package has no parent: {}",
                    previous_target.display()
                )
            })?
            .join(format!(
                ".{}.{}.migrated",
                file_name.to_string_lossy(),
                unique_suffix()
            ));
        if let Err(error) = fs::rename(previous_target, &tombstone) {
            let detach_rollback = restore_detached_packages(&detached_previous);
            let registration_rollback = rollback_registration_changes(
                &vrpathreg,
                &registrations_added,
                &registrations_removed,
            );
            let package_rollback = rollback_packages(&packages);
            return Err(format!(
                "detach previous package {}: {error}; detach rollback={detach_rollback:?}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}",
                previous_target.display()
            ));
        }
        detached_previous.push((previous_target.clone(), tombstone));
    }

    let detached_module = if let Some(module) = previous_module {
        let tombstone = module
            .parent()
            .ok_or_else(|| format!("owned VRCFT module has no parent: {}", module.display()))?
            .join(format!(
                ".GalaxyXR.VRCFaceTracking.{}.migrated",
                unique_suffix()
            ));
        if let Err(error) = fs::rename(&module, &tombstone) {
            let detach_rollback = restore_detached_packages(&detached_previous);
            let registration_rollback = rollback_registration_changes(
                &vrpathreg,
                &registrations_added,
                &registrations_removed,
            );
            let package_rollback = rollback_packages(&packages);
            return Err(format!(
                "detach obsolete owned GXRP VRCFT module: {error}; detach rollback={detach_rollback:?}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}"
            ));
        }
        Some((module, tombstone))
    } else {
        None
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
        vrcft_module_installed: false,
        vrcft_module_path: None,
        vrcft_module_sha256: None,
    };

    let legacy_receipt_tombstone = if loaded_receipt.as_ref().is_some_and(|loaded| loaded.legacy) {
        let source = &loaded_receipt.as_ref().unwrap().source_path;
        let tombstone = source
            .parent()
            .ok_or("legacy receipt has no parent")?
            .join(format!(".install-state.{}.migrated", unique_suffix()));
        if let Err(error) = fs::rename(source, &tombstone) {
            if let Some((module, detached)) = &detached_module {
                let _ = fs::rename(detached, module);
            }
            let detach_rollback = restore_detached_packages(&detached_previous);
            let registration_rollback = rollback_registration_changes(
                &vrpathreg,
                &registrations_added,
                &registrations_removed,
            );
            let package_rollback = rollback_packages(&packages);
            return Err(format!(
                "detach validated legacy receipt: {error}; detach rollback={detach_rollback:?}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}"
            ));
        }
        Some((source.clone(), tombstone))
    } else {
        None
    };

    if let Err(error) = write_receipt(&receipt) {
        let legacy_receipt_restore = legacy_receipt_tombstone
            .as_ref()
            .and_then(|(source, tombstone)| fs::rename(tombstone, source).err());
        let module_restore = detached_module
            .as_ref()
            .and_then(|(module, detached)| fs::rename(detached, module).err());
        let detach_rollback = restore_detached_packages(&detached_previous);
        let registration_rollback =
            rollback_registration_changes(&vrpathreg, &registrations_added, &registrations_removed);
        let package_rollback = rollback_packages(&packages);
        return Err(format!(
            "commit install receipt: {error}; legacy receipt restore={legacy_receipt_restore:?}; module restore={module_restore:?}; detach rollback={detach_rollback:?}; registration rollback={registration_rollback:?}; package rollback={package_rollback:?}"
        ));
    }

    if let Some((_, tombstone)) = legacy_receipt_tombstone {
        let _ = fs::remove_file(tombstone);
    }
    for transaction in &packages {
        if transaction.had_target {
            let _ = fs::remove_dir_all(&transaction.backup);
        }
    }
    for (_, tombstone) in detached_previous {
        let _ = fs::remove_dir_all(tombstone);
    }
    if let Some((_, tombstone)) = detached_module {
        let _ = fs::remove_file(tombstone);
    }
    Ok(receipt)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_root(label: &str) -> PathBuf {
        std::env::temp_dir().join(format!("galaxyxr-{label}-{}", unique_suffix()))
    }

    fn create_test_package(root: &Path, spec: PackageSpec) {
        fs::create_dir_all(root).unwrap();
        let manifest = if spec.resource_only {
            format!(
                r#"{{"name":"{}","resourceOnly":true,"alwaysActivate":true,"hmd_presence":[]}}"#,
                spec.name
            )
        } else {
            format!(r#"{{"name":"{}","resourceOnly":false}}"#, spec.name)
        };
        fs::write(root.join("driver.vrdrivermanifest"), manifest).unwrap();
        for relative in spec.required_files {
            let path = root.join(relative);
            if path.exists() {
                continue;
            }
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            let contents: &[u8] = match path.extension().and_then(|value| value.to_str()) {
                Some("json" | "vrsettings" | "vrresources") => b"{}",
                Some("obj" | "mtl" | "hlsl") => b"",
                _ => b"test",
            };
            fs::write(path, contents).unwrap();
        }
    }

    fn v2_receipt(active: &Path, resources: &Path) -> InstallReceipt {
        InstallReceipt {
            schema_version: 2,
            packages: vec![
                PackageReceipt {
                    name: ACTIVE_DRIVER_NAME.into(),
                    path: active.to_string_lossy().into_owned(),
                    sha256: String::new(),
                    file_count: 0,
                },
                PackageReceipt {
                    name: RESOURCE_DRIVER_NAME.into(),
                    path: resources.to_string_lossy().into_owned(),
                    sha256: String::new(),
                    file_count: 0,
                },
            ],
            driver_path: None,
            package_sha256: None,
            file_count: None,
            vrcft_module_installed: false,
            vrcft_module_path: None,
            vrcft_module_sha256: None,
        }
    }

    fn v3_receipt(resources: &Path) -> InstallReceipt {
        InstallReceipt {
            schema_version: 3,
            packages: vec![PackageReceipt {
                name: RESOURCE_DRIVER_NAME.into(),
                path: resources.to_string_lossy().into_owned(),
                sha256: String::new(),
                file_count: 0,
            }],
            driver_path: None,
            package_sha256: None,
            file_count: None,
            vrcft_module_installed: false,
            vrcft_module_path: None,
            vrcft_module_sha256: None,
        }
    }

    fn v4_receipt(active: &Path, resources: &Path) -> InstallReceipt {
        let mut receipt = v2_receipt(active, resources);
        receipt.schema_version = 4;
        receipt
    }

    #[test]
    fn v4_receipt_owns_both_driver_packages_without_vrcft() {
        let mut receipt = v4_receipt(Path::new("active"), Path::new("resources"));
        validate_receipt_shape(&receipt, false).unwrap();
        receipt.packages.pop();
        assert!(validate_receipt_shape(&receipt, false).is_err());
        receipt = v4_receipt(Path::new("active"), Path::new("resources"));
        receipt.vrcft_module_installed = true;
        receipt.vrcft_module_path = Some("module".into());
        receipt.vrcft_module_sha256 = Some("hash".into());
        assert!(validate_receipt_shape(&receipt, false).is_err());
    }

    #[test]
    fn v3_resource_only_receipt_remains_valid_for_v4_upgrade() {
        let receipt = v3_receipt(Path::new("resources"));
        validate_receipt_shape(&receipt, false).unwrap();
        let specs = receipt_package_specs(receipt.schema_version);
        assert_eq!(specs.len(), 1);
        assert_eq!(specs[0].name, RESOURCE_DRIVER_NAME);

        let upgraded = v4_receipt(Path::new("active"), Path::new("resources"));
        validate_receipt_shape(&upgraded, false).unwrap();
    }

    #[test]
    fn v4_receipt_selects_both_driver_packages() {
        let receipt = v4_receipt(Path::new("active"), Path::new("resources"));
        let specs = receipt_package_specs(receipt.schema_version);
        assert_eq!(specs.len(), 2);
        assert_eq!(specs[0].name, ACTIVE_DRIVER_NAME);
        assert_eq!(specs[1].name, RESOURCE_DRIVER_NAME);
    }

    #[test]
    fn galaxy_receipt_is_isolated_from_generic_custom_headset_state() {
        let appdata = Path::new(r"C:\Users\Test\AppData\Roaming");
        assert_eq!(
            receipt_path_from(appdata),
            appdata
                .join("GalaxyXR")
                .join("CustomHeadset")
                .join("install-state.json")
        );
        assert_ne!(
            receipt_path_from(appdata),
            legacy_receipt_path_from(appdata)
        );
    }

    #[test]
    fn validated_v1_receipt_owns_only_the_active_legacy_package() {
        let root = test_root("v1-receipt");
        let steamvr = root.join("SteamVR");
        let active = steamvr.join("drivers").join(ACTIVE_DRIVER_NAME);
        create_test_package(&active, ACTIVE_SPEC);
        let tree = hash_tree(&active).unwrap();
        let receipt = InstallReceipt {
            schema_version: 1,
            packages: Vec::new(),
            driver_path: Some(active.to_string_lossy().into_owned()),
            package_sha256: Some(tree_identity(&tree)),
            file_count: Some(tree.len()),
            vrcft_module_installed: false,
            vrcft_module_path: None,
            vrcft_module_sha256: None,
        };
        let owned =
            receipt_owned_package_targets(&receipt, steamvr.to_str().unwrap(), true).unwrap();
        assert_eq!(owned.len(), 1);
        assert_eq!(owned[0].0.name, ACTIVE_DRIVER_NAME);
        assert!(validate_receipt_shape(&receipt, false).is_err());

        fs::remove_file(active.join("resources/shaders/d3d11/vrlink_layer_ps.hlsl")).unwrap();
        assert!(receipt_owned_package_targets(&receipt, steamvr.to_str().unwrap(), true).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn generic_v2_migration_requires_both_exact_legacy_packages() {
        let root = test_root("v2-receipt");
        let steamvr = root.join("SteamVR");
        let active = steamvr.join("drivers").join(ACTIVE_DRIVER_NAME);
        let resources = steamvr.join("drivers").join(RESOURCE_DRIVER_NAME);
        create_test_package(&active, ACTIVE_SPEC);
        create_test_package(&resources, RESOURCE_SPEC);
        let active_tree = hash_tree(&active).unwrap();
        let resource_tree = hash_tree(&resources).unwrap();
        let mut receipt = v2_receipt(&active, &resources);
        receipt.packages[0].sha256 = tree_identity(&active_tree);
        receipt.packages[0].file_count = active_tree.len();
        receipt.packages[1].sha256 = tree_identity(&resource_tree);
        receipt.packages[1].file_count = resource_tree.len();
        assert_eq!(
            receipt_owned_package_targets(&receipt, steamvr.to_str().unwrap(), true)
                .unwrap()
                .len(),
            2
        );
        receipt.packages.pop();
        assert!(receipt_owned_package_targets(&receipt, steamvr.to_str().unwrap(), true).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn conflict_preflight_rejects_unowned_matching_external_driver() {
        let root = test_root("conflict");
        let steamvr = root.join("SteamVR");
        fs::create_dir_all(steamvr.join("drivers")).unwrap();
        let managed = root.join("managed");
        let external = root.join("external").join(ACTIVE_DRIVER_NAME);
        create_test_package(&external, ACTIVE_SPEC);
        assert!(reject_unowned_package_conflicts_with_state(
            steamvr.to_str().unwrap(),
            &managed,
            &[],
            std::slice::from_ref(&external)
        )
        .is_err());
        assert!(reject_unowned_package_conflicts_with_state(
            steamvr.to_str().unwrap(),
            &managed,
            &[(ACTIVE_SPEC, external.clone())],
            std::slice::from_ref(&external)
        )
        .is_ok());
        let managed_active = managed.join(ACTIVE_DRIVER_NAME);
        create_test_package(&managed_active, ACTIVE_SPEC);
        assert!(reject_unowned_package_conflicts_with_state(
            steamvr.to_str().unwrap(),
            &managed,
            &[],
            &[]
        )
        .is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn openvr_registry_rejects_unusable_external_driver_data() {
        let root = test_root("openvrpaths");
        fs::create_dir_all(&root).unwrap();
        let paths = root.join("openvrpaths.vrpath");
        fs::write(&paths, r#"{"external_drivers":"not-an-array"}"#).unwrap();
        assert!(registered_external_drivers_from(&paths).is_err());
        fs::write(&paths, r#"{"external_drivers":[42]}"#).unwrap();
        assert!(registered_external_drivers_from(&paths).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn vrresources_are_parsed_and_dangling_driver_paths_are_rejected() {
        let root = test_root("vrresources");
        let resources = root.join("resources");
        fs::create_dir_all(&resources).unwrap();
        let document = resources.join("driver.vrresources");
        fs::write(&document, b"{").unwrap();
        assert!(validate_json_tree(&root, "testdriver").is_err());
        fs::write(
            &document,
            r#"{"icons":{"ready":"{testdriver}/icons/missing.png"}}"#,
        )
        .unwrap();
        assert!(validate_json_tree(&root, "testdriver").is_err());
        fs::create_dir_all(resources.join("icons")).unwrap();
        fs::write(resources.join("icons/missing.png"), b"png").unwrap();
        validate_json_tree(&root, "testdriver").unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn vrcft_receipt_cannot_own_same_hash_at_a_different_path() {
        let root = test_root("vrcft-path");
        let expected = root.join("VRCFaceTracking/CustomLibs/GalaxyXR.VRCFaceTracking.dll");
        let wrong = root.join("elsewhere/GalaxyXR.VRCFaceTracking.dll");
        fs::create_dir_all(expected.parent().unwrap()).unwrap();
        fs::create_dir_all(wrong.parent().unwrap()).unwrap();
        fs::write(&expected, b"same module").unwrap();
        fs::write(&wrong, b"same module").unwrap();
        let mut receipt = v2_receipt(Path::new("active"), Path::new("resources"));
        receipt.vrcft_module_installed = true;
        receipt.vrcft_module_path = Some(wrong.to_string_lossy().into_owned());
        receipt.vrcft_module_sha256 = Some(hash_file(&wrong).unwrap());
        assert!(validate_receipt_vrcft_target_at(&receipt, &expected).is_err());
        receipt.vrcft_module_path = Some(expected.to_string_lossy().into_owned());
        assert_eq!(
            validate_receipt_vrcft_target_at(&receipt, &expected).unwrap(),
            Some(expected.clone())
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn built_release_package_has_a_closed_resource_graph() {
        let output = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("..")
            .join("output");
        let resources = output.join(RESOURCE_DRIVER_NAME);
        assert!(resources.is_dir(), "build the resource package first");
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
    if steamvr_process_running() {
        return Err("SteamVR is running. Close SteamVR before uninstalling drivers.".into());
    }
    let loaded = load_receipt_for_context(&steamvr_dir)?
        .ok_or("managed install receipt is missing; refusing uninstall")?;
    let LoadedReceipt {
        receipt,
        source_path,
        legacy,
    } = loaded;
    let vrpathreg = vrpathreg_path(&steamvr_dir)?;
    let targets = receipt_owned_package_targets(&receipt, &steamvr_dir, legacy)?;
    let module = validate_receipt_vrcft_target(&receipt)?;
    let mut registrations_removed = Vec::new();
    for (_, target) in &targets {
        match set_exact_driver_registration(&vrpathreg, target, false) {
            Ok(true) => registrations_removed.push(target.clone()),
            Ok(false) => {}
            Err(error) => {
                let rollback =
                    rollback_registration_changes(&vrpathreg, &[], &registrations_removed);
                return Err(format!(
                    "unregister exact managed package path failed: {error}; rollback={rollback:?}"
                ));
            }
        }
    }
    let mut tombstones = Vec::new();
    for (spec, target) in &targets {
        let tombstone = target
            .parent()
            .ok_or_else(|| format!("managed target has no parent: {}", target.display()))?
            .join(format!(".{}.{}.remove", spec.name, unique_suffix()));
        if let Err(error) = fs::rename(target, &tombstone) {
            for (restore_target, detached) in tombstones.iter().rev() {
                let _ = fs::rename(detached, restore_target);
            }
            let registration_rollback =
                rollback_registration_changes(&vrpathreg, &[], &registrations_removed);
            return Err(format!(
                "detach installed {}: {error}; registration rollback={registration_rollback:?}",
                spec.name
            ));
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
            let registration_rollback =
                rollback_registration_changes(&vrpathreg, &[], &registrations_removed);
            return Err(format!(
                "detach VRCFT module: {error}; package restore={restore:?}; registration rollback={registration_rollback:?}"
            ));
        }
    }
    if let Err(error) = fs::remove_file(&source_path) {
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
        let registration_rollback =
            rollback_registration_changes(&vrpathreg, &[], &registrations_removed);
        return Err(format!(
            "remove install receipt: {error}; package restore={package_restore:?}; module restore={module_restore:?}; registration rollback={registration_rollback:?}"
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

fn is_proven_legacy_galaxy_package(path: &Path, name: &str) -> bool {
    match name {
        ACTIVE_DRIVER_NAME | "GalaxyXRNative" => {
            path.join("resources/shaders/d3d11/vrlink_layer_ps.hlsl")
                .is_file()
                && path
                    .join("resources/shaders/d3d11/vrlink_fxaa_ps.hlsl")
                    .is_file()
        }
        RESOURCE_DRIVER_NAME => validate_package(path, RESOURCE_SPEC).is_ok(),
        _ => false,
    }
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
    let loaded_receipt = load_receipt_for_context(steamvr_dir)?;
    let receipt_owned_packages = loaded_receipt
        .as_ref()
        .map(|loaded| receipt_owned_package_targets(&loaded.receipt, steamvr_dir, loaded.legacy))
        .transpose()?
        .unwrap_or_default();

    for name in [ACTIVE_DRIVER_NAME, RESOURCE_DRIVER_NAME, "GalaxyXRNative"] {
        let target = drivers_root.join(name);
        if !target.exists() {
            continue;
        }
        let receipt_owned = receipt_owned_packages
            .iter()
            .any(|(spec, path)| spec.name == name && same_canonical_path(path, &target));
        match cleanup_manifest_identity(&target, Some(name)) {
            Ok(_) if receipt_owned => {
                // The receipt-owned target is validated and added below.
            }
            Ok(_) if is_proven_legacy_galaxy_package(&target, name) => {
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
            Ok(_) => preserved.push(format!(
                "Unowned similarly named SteamVR package preserved: {}",
                target.display()
            )),
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
                Ok(name) => {
                    let receipt_owned = receipt_owned_packages.iter().any(|(spec, owned)| {
                        spec.name == name && same_canonical_path(owned, &path)
                    });
                    if !receipt_owned && !is_proven_legacy_galaxy_package(&path, &name) {
                        preserved.push(format!(
                            "Unowned similarly named external driver preserved: {}",
                            path.display()
                        ));
                        continue;
                    }
                    match fs::canonicalize(&path) {
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
                    }
                }
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

    let expected_vrcft_module = expected_vrcft_module_path()?;
    if let Some(loaded) = loaded_receipt.as_ref() {
        for (spec, target) in &receipt_owned_packages {
            if let Err(error) = push_cleanup_target(
                &mut targets,
                &mut actions,
                &mut token_entries,
                target.clone(),
                format!(
                    "Remove receipt-owned {} package: {}",
                    spec.name,
                    target.display()
                ),
            ) {
                blockers.push(error);
            }
        }
        if let Some(module) = validate_receipt_vrcft_target(&loaded.receipt)? {
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
        if loaded.source_path.is_file() {
            if let Err(error) = push_cleanup_target(
                &mut targets,
                &mut actions,
                &mut token_entries,
                loaded.source_path.clone(),
                format!(
                    "Remove managed install receipt: {}",
                    loaded.source_path.display()
                ),
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
        match set_exact_driver_registration(vrpathreg, removed, true) {
            Ok(true) | Ok(false) => {}
            Err(error) => errors.push(format!(
                "restore exact registration {}: {error}",
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
        match set_exact_driver_registration(&plan.vrpathreg, registration, false) {
            Ok(true) => unregistered.push(registration.clone()),
            Ok(false) => {
                let rollback_errors = rollback_cleanup(&plan.vrpathreg, &[], &unregistered);
                return Err(format!(
                    "exact registration disappeared after preview: {}; rollback errors: {:?}",
                    registration.display(),
                    rollback_errors
                ));
            }
            Err(error) => {
                let rollback_errors = rollback_cleanup(&plan.vrpathreg, &[], &unregistered);
                return Err(format!(
                    "unregister exact driver path {}: {error}; rollback errors: {:?}",
                    registration.display(),
                    rollback_errors
                ));
            }
        }
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
