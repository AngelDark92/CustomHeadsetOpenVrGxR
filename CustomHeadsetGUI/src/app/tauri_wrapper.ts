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
    driverPath: string;
    packageSha256: string;
    fileCount: number;
    vrcftModuleInstalled: boolean;
    vrcftModulePath?: string;
    vrcftModuleSha256?: string;
}

export async function install_driver_transactional(
    sourceDir: string,
    steamvrDir: string,
    vrcftModulePath?: string,
): Promise<DriverInstallReceipt> {
    return await invoke('install_driver_transactional', {
        sourceDir,
        steamvrDir,
        vrcftModulePath: vrcftModulePath ?? null,
    }) as DriverInstallReceipt;
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
