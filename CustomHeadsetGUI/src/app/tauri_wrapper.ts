import { invoke } from '@tauri-apps/api/core';

export async function get_executable_path() {
    return await invoke('get_executable_path') as string;

}
export async function is_vrmonitor_running() {
    return await invoke('is_vrmonitor_running') as boolean;
}
export async function restart_vrcompositor() {
    return await invoke('restart_vrcompositor') as boolean;
}
export async function kill_process(process_name: string): Promise<boolean> {
    return await invoke('kill_process', { processName: process_name }) as boolean;
}
export async function launch_process(path: string, args: string[]): Promise<boolean> {
    return await invoke('launch_process', { path, args }) as boolean;
}

export interface DriverInstallReceipt {
    schemaVersion: number;
    packages: Array<{ name: string; path: string; sha256: string; fileCount: number }>;
    vrcftModuleInstalled: boolean;
    vrcftModulePath?: string;
    vrcftModuleSha256?: string;
}

export interface GalaxyXrApkPreview {
    apkPath: string;
    fileName: string;
    packageName: string;
    versionCode: number;
    host: string;
    controlPort: number;
    trackingPort: number;
    apkSha256: string;
    bridgeSha256: string;
    pairingTokenFingerprint: string;
    signaturePresent: boolean;
}

export async function inspect_galaxyxr_apk(apkPath: string): Promise<GalaxyXrApkPreview> {
    return await invoke('inspect_galaxyxr_apk', { apkPath }) as GalaxyXrApkPreview;
}

export async function enroll_galaxyxr_apk(
    apkPath: string,
    expectedSha256: string,
    settingsPath: string,
    expectedSettingsContents?: string,
): Promise<GalaxyXrApkPreview> {
    return await invoke('enroll_galaxyxr_apk', { apkPath, expectedSha256, settingsPath, expectedSettingsContents }) as GalaxyXrApkPreview;
}

export async function install_driver_transactional(
    sourceDir: string,
    resourceSourceDir: string,
    steamvrDir: string,
    vrcftModulePath?: string,
): Promise<DriverInstallReceipt> {
    return await invoke('install_driver_transactional', {
        sourceDir,
        resourceSourceDir,
        steamvrDir,
        vrcftModulePath: vrcftModulePath ?? null,
    }) as DriverInstallReceipt;
}

export async function preflight_driver_install(
    sourceDir: string,
    resourceSourceDir: string,
    steamvrDir: string,
): Promise<void> {
    await invoke('preflight_driver_install', { sourceDir, resourceSourceDir, steamvrDir });
}

export async function uninstall_driver_transactional(steamvrDir: string): Promise<boolean> {
    return await invoke('uninstall_driver_transactional', { steamvrDir }) as boolean;
}

export async function verify_driver_install(steamvrDir: string): Promise<boolean> {
    return await invoke('verify_driver_install', { steamvrDir }) as boolean;
}

export async function write_json_file_transactional(path: string, contents: string): Promise<void> {
    await invoke('write_json_file_transactional', { path, contents });
}

export interface DriverCleanupPreview {
    planToken: string;
    actions: string[];
    preserved: string[];
    blockers: string[];
    steamVrRunning: boolean;
}

export interface DriverCleanupReport {
    removed: string[];
    unregistered: string[];
    preserved: string[];
    warnings: string[];
}

export async function preview_driver_cleanup(steamvrDir: string): Promise<DriverCleanupPreview> {
    return await invoke('preview_driver_cleanup', { steamvrDir }) as DriverCleanupPreview;
}

export async function execute_driver_cleanup(
    steamvrDir: string,
    planToken: string,
): Promise<DriverCleanupReport> {
    return await invoke('execute_driver_cleanup', { steamvrDir, planToken }) as DriverCleanupReport;
}


export async function run_process_sync(path: string, args: string[]): Promise<number> {
    return await invoke('run_process_sync', { path, args }) as number;
}
