import { computed, effect, Injectable, OnDestroy, signal } from '@angular/core';
import { copyFile, exists, mkdir, readDir, readTextFile, remove, watchImmediate, writeTextFile } from '@tauri-apps/plugin-fs';
import { appLocalDataDir, basename, join, resourceDir } from '@tauri-apps/api/path';
import { DriverSettingService } from './driver-setting.service';
import { DriverInfoService } from './driver-info.service';
import { debounceTime, Subject } from 'rxjs';
import { DriverCleanupPreview, DriverCleanupReport, execute_driver_cleanup, get_executable_path, install_driver_transactional, preflight_driver_install, preview_driver_cleanup, restart_vrcompositor, uninstall_driver_transactional, verify_driver_install, write_json_file_transactional, run_process_sync } from '../tauri_wrapper';
import { customHeadsetDriverName, driverCopyInstallationMethod, vendor, vendorUi } from '../../environment';
import { open } from '@tauri-apps/plugin-dialog';
import { DialogService } from './dialog.service';
import { PullingService } from './PullingService';
import { cleanJsonComments } from '../helpers';
import { launch_process } from '../tauri_wrapper';
import { openUrl } from '@tauri-apps/plugin-opener';

@Injectable({providedIn: "root", })
export class SystemDiagnosticService implements OnDestroy {
  private _installingDriver = signal(false)
  public readonly installingDriver = this._installingDriver.asReadonly()
  private _cleaningDriver = signal(false)
  public readonly cleaningDriver = this._cleaningDriver.asReadonly()
  private _steamVRinstalled = signal<string | undefined>(undefined);
  public readonly steamVRinstalled = this._steamVRinstalled.asReadonly();
  private _driverInstalled = signal<string | undefined>(undefined);
  public readonly driverInstalled = this._driverInstalled.asReadonly();
  private _driverRepairReason = signal<string | undefined>(undefined);
  public readonly driverRepairReason = this._driverRepairReason.asReadonly();
  public readonly settingFileInited = computed(() => this.dss.values() && this.dis.values());
  public readonly systemReady = computed(() => this.steamVRinstalled() && this.driverInstalled() && this.settingFileInited())
  public readonly galaxyXrIntegration = vendorUi === 'galaxyxr';
  public readonly driverVersionMismatch = computed(() => {
    const installed = this.driverInstalled();
    const lastRun = this.dis.values()?.driverVersion;
    return !!installed && !!lastRun && installed !== lastRun;
  });
  private _steamVrConfig = signal<any>(undefined);
  public readonly steamVrConfig = this._steamVrConfig.asReadonly();
  private cleanUp: (() => void)[] = []
  private _initTask: Promise<any>;
  public get initTask() {
    return this._initTask;
  }
  public readonly pullingSteamVRinstall = new PullingService(() => this.checkSteamVrInstalled(), 'pullingSteamVRinstallk');
  public readonly PullingDriverinstall = new PullingService(() => this.checkDriverInstalled(), 'PullingDriverinstall');
  constructor(
    public dss: DriverSettingService,
    public dis: DriverInfoService,
    private dialog: DialogService,
  ) {
    let readySetup = false
    effect(() => {
      this.watchSteamVRSettings();
      const ready = this.systemReady()
      if (ready && !readySetup) {
        readySetup = true;
        this.readySetup();
      }
    })
    this._initTask = (async () => {
      await dss.initTask;
      await dis.initTask;
      await this.checkDriverInstalled()
    })();
  }
  async watchSteamVRSettings(){
    const subject = new Subject<void>();
    const usub = subject.pipe(debounceTime(50)).subscribe(async () => {
      const value = await this.getSteamVRSettings();
      this._steamVrConfig.set(value)
    })
    this.cleanUp.push(() => {
      usub.unsubscribe();
      subject.complete();
    })
    const path = await this.getSteamVRConfigFilePath();
    if (path) {
      watchImmediate(path, (e) => {
        subject.next()
      });
      subject.next()
    }
  }
  async readySetup() {
    
  }
  ngOnDestroy(): void {
    for (const cfn of this.cleanUp) {
      cfn()
    }
  }

  public async checkDriverInstalled() {
    const steamVrPath = await this.checkSteamVrInstalled();
    if (steamVrPath) {
      if (!vendor) {
        try {
          if (!await verify_driver_install(steamVrPath)) {
            this._driverInstalled.set(undefined);
            this._driverRepairReason.set('The managed Galaxy XR driver receipt or installed files do not match. Close SteamVR and run Install to repair them.');
            return false;
          }
          this._driverRepairReason.set(undefined);
        } catch (error) {
          this._driverInstalled.set(undefined);
          this._driverRepairReason.set(`Driver verification failed: ${error}`);
          console.warn('Managed driver verification failed:', error);
          return false;
        }
      }
      // Check for copied driver installation
      let driverPath = await join(steamVrPath, 'drivers', customHeadsetDriverName, 'driver.vrdrivermanifest')
      if (!await exists(driverPath)) {
        // Check for in-place registered installation
        const registeredPath = await this.checkDriverRegisteredPath();
        driverPath = await join(registeredPath, 'driver.vrdrivermanifest')
      }
      if (await exists(driverPath)) {
        let version = '0.0.0'
        try {
          const obj = JSON.parse(cleanJsonComments(await readTextFile(driverPath)))
          if (obj['version']) {
            version = obj['version']
          }
        } catch (ex) {
          console.warn('read version failed')
        }
        let infoDriverVersion = this.dis.values()?.driverVersion
        if (version === '0.0.0' && infoDriverVersion) {
          version = infoDriverVersion
        }
        this._driverInstalled.set(version);
        this.PullingDriverinstall.stop()
        return true;
      }
    }
    this._driverInstalled.set(undefined);
    return false;
  }
  public async getOpenvrpaths() {
    const openVrConfigPath = await join(await appLocalDataDir(), '../openvr/openvrpaths.vrpath');
    const vrPathExists = await exists(openVrConfigPath);
    if (vrPathExists) {
      try {
        return JSON.parse(await readTextFile(openVrConfigPath));
      } catch (e) {
        console.error(e);
      }
    }
    return undefined;
  }
  public async disableSteamVRDriver(driverName: string) {
    await this.updateSteamVRSettings(settings => {
      const name = this.getDriverFieldName(driverName);
      if (!settings[name]) {
        settings[name] = {}
      }
      settings[name]['enable'] = false
      return true;
    })
  }
  public async enableSteamVRDriver(driverName: string) {
    await this.updateSteamVRSettings(settings => {
      const name = this.getDriverFieldName(driverName);
      if (!settings[name]) {
        settings[name] = {}
      }
      settings[name]['enable'] = true
      delete settings[name]['blocked_by_safe_mode']
      return true;
    })
  }
  public async unblockAllDrivers() {
    await this.updateSteamVRSettings(settings => {
      let changed = false;
      for (const key in settings) {
        if (key.startsWith('driver_') && settings[key]) {
          if (settings[key]['blocked_by_safe_mode']) {
            delete settings[key]['blocked_by_safe_mode'];
            changed = true;
          }
        }
      }
      return changed;
    })
  }
  public getSteamVRDriverEnableState(settings: any, driverName: string) {
    if (settings) {
      const driverSetting = settings[this.getDriverFieldName(driverName)]
      if (driverSetting) {
        return (driverSetting['enable'] ?? true) && !(driverSetting['blocked_by_safe_mode'] ?? false);
      }
    }
    return true;
  }
  public isDriverBlocked(settings: any, driverName: string) {
    if (settings) {
      const driverSetting = settings[this.getDriverFieldName(driverName)]
      if (driverSetting) {
        for (const key in settings) {
          if (key.startsWith('driver_') && settings[key]) {
            if (settings[key]['blocked_by_safe_mode']) {
              return true;
            }
          }
        }
      }
    }
    return false;
  }
  private getDriverFieldName(driverName: string) {
    return `driver_${driverName}`;
  }
  /**
   * For vendor-specific drivers: whether the vendor-neutral CustomHeadsetOpenVR
   * driver is enabled (which locks this vendor driver out).
   */
  public getNeutralDriverEnabled(settings: any): boolean {
    if (!settings) {
      return false;
    }
    const neutralDriverKey = this.getDriverFieldName('CustomHeadsetOpenVR');
    const driverSetting = settings[neutralDriverKey];
    if (!driverSetting) {
      // Driver not present in settings at all - treat as disabled
      return false;
    }
    if (driverSetting['blocked_by_safe_mode']) {
      return false;
    }
    return driverSetting['enable'] ?? true;
  }
  /**
   * For vendor-specific drivers: disable the neutral driver and enable the vendor driver.
   * This implements the driver lockout swap behavior.
   */
  public async enableVendorDriverAndDisableNeutral() {
    await this.updateSteamVRSettings(settings => {
      let changed = false;
      // Disable the vendor-neutral driver
      const neutralKey = this.getDriverFieldName('CustomHeadsetOpenVR');
      if (!settings[neutralKey]) {
        settings[neutralKey] = {};
      }
      if (settings[neutralKey]['enable'] !== false) {
        settings[neutralKey]['enable'] = false;
        changed = true;
      }
      // Enable the vendor-specific driver
      const vendorKey = this.getDriverFieldName(customHeadsetDriverName);
      if (!settings[vendorKey]) {
        settings[vendorKey] = {};
      }
      if (settings[vendorKey]['enable'] !== true) {
        settings[vendorKey]['enable'] = true;
        changed = true;
      }
      delete settings[vendorKey]['blocked_by_safe_mode'];
      return changed;
    });
  }
  private installing = false
  private driverMutationInProgress = false
  async installDriver() {
    if (this.installing || this.driverMutationInProgress) return false;
    const steamVrPath = this.steamVRinstalled();
    if (!steamVrPath) return false;
    this._installingDriver.set(true);
    this.installing = true
    this.driverMutationInProgress = true
    let installSucceeded = false;
    try {
      const executablePath = await get_executable_path();
      if (vendor) {
        let driverDir = await join(executablePath, `../${customHeadsetDriverName}`);
        if (!await exists(driverDir)) {
          if (await this.dialog.confirm($localize`Driver folder not found`, $localize`The driver folder is not in the default location. Unpack the entire zip and retry, or manually locate the new CustomHeadsetOpenVR folder to be installed.`, $localize`Locate`, 'primary')) {
            const path = await open({ directory: true, multiple: false })
            if (path) {
              driverDir = path;
            } else {
              return false;
            }
          } else {
            return false;
          }
        }
        if (!await exists(await join(driverDir, 'driver.vrdrivermanifest'))) {
          await this.dialog.message($localize`Driver files not valid`, $localize`the folder seems not include driver file, please check again`)
          return false;
        }
        // Vendor replacement retains the upstream copy/register flow. Refuse
        // to continue when the old vendor install could not be removed.
        if (!await this.uninstallDriverInternal()) {
          return false;
        }
        await this.cleanupLegacyForkInstall(steamVrPath);
        if (driverCopyInstallationMethod) {
          const steamVrDriverDir = await join(steamVrPath, 'drivers');
          if (!await exists(steamVrDriverDir)) {
            await mkdir(steamVrDriverDir)
          }
          const driverPath = await join(steamVrDriverDir, customHeadsetDriverName);
          try {
            await this.copyRec(driverPath, driverDir)
          } catch (e) {
            await this.dialog.message($localize`Install Failed, Make sure SteamVR is closed`, `${e}`)
            return false
          }
        } else {
          // Register driver in place using vrpathreg (vendor build behavior)
          try {
            const success = await this.registerDriver(steamVrPath, driverDir);
            if (!success) {
              await this.dialog.message($localize`Install Failed`, $localize`Failed to register driver using vrpathreg. Make sure SteamVR is installed and closed.`)
              return false;
            }
          } catch (e) {
            await this.dialog.message($localize`Install Failed, Make sure SteamVR is closed`, `${e}`)
            return false
          }
        }
      } else {
        const bundledResources = await resourceDir();
        const candidates = [
          {
            active: await join(bundledResources, 'CustomHeadsetOpenVR'),
            resource: await join(bundledResources, 'galaxyxrresources'),
          },
          {
            active: await join(executablePath, '../CustomHeadsetOpenVR'),
            resource: await join(executablePath, '../galaxyxrresources'),
          },
          {
            active: await join(executablePath, '../../CustomHeadsetOpenVR'),
            resource: await join(executablePath, '../../galaxyxrresources'),
          },
        ];
        let activeDriverDir: string | undefined;
        let resourceDriverDir: string | undefined;
        for (const candidate of candidates) {
          if (await exists(await join(candidate.active, 'driver.vrdrivermanifest')) &&
              await exists(await join(candidate.resource, 'driver.vrdrivermanifest'))) {
            activeDriverDir = candidate.active;
            resourceDriverDir = candidate.resource;
            break;
          }
        }
        if (!activeDriverDir || !resourceDriverDir) {
          const locate = await this.dialog.confirm(
            $localize`Galaxy XR driver package not found`,
            $localize`Unpack the complete release and retry, or locate the CustomHeadsetOpenVR folder beside galaxyxrresources.`,
            $localize`Locate`,
            'primary',
          );
          if (!locate) return false;
          const selected = await open({ directory: true, multiple: false });
          if (typeof selected !== 'string') return false;
          activeDriverDir = selected;
          resourceDriverDir = await join(selected, '..', 'galaxyxrresources');
        }
        if (!await exists(await join(activeDriverDir, 'driver.vrdrivermanifest')) ||
            !await exists(await join(resourceDriverDir, 'driver.vrdrivermanifest'))) {
          await this.dialog.message(
            $localize`Driver files not valid`,
            $localize`CustomHeadsetOpenVR and galaxyxrresources must both contain driver.vrdrivermanifest.`,
          );
          return false;
        }
        try {
          await preflight_driver_install(activeDriverDir, resourceDriverDir, steamVrPath);
          await install_driver_transactional(activeDriverDir, resourceDriverDir, steamVrPath);
        } catch (error) {
          await this.dialog.message($localize`Install failed; make sure SteamVR is closed`, `${error}`);
          return false;
        }
      }
      installSucceeded = true;
      try {
        await this.checkDriverInstalled()
      } catch (error) {
        console.warn('Installed driver status refresh failed:', error);
      }
      return true;
    } finally {
      if(installSucceeded){
        try {
          if (vendor) {
            await this.enableVendorDriverAndDisableNeutral();
          } else {
            await this.updateSteamVRSettings(settings => {
              if (!this.dss.values() || this.dss.values()?.meganeX8K?.enable) {
                for (const name of ['MeganeXSuperlight', 'MeganeX8KMark2', 'MeganeXsuperlight8K_Native', 'MeganeX8KMark2_Native']) {
                  const field = this.getDriverFieldName(name);
                  settings[field] ??= {};
                  settings[field]['enable'] = false;
                }
              }
              const active = this.getDriverFieldName('CustomHeadsetOpenVR');
              settings[active] ??= {};
              settings[active]['enable'] = true;
              delete settings[active]['blocked_by_safe_mode'];
              const resource = this.getDriverFieldName('galaxyxrresources');
              settings[resource] ??= {};
              settings[resource]['enable'] = true;
              delete settings[resource]['blocked_by_safe_mode'];
              return true;
            });
          }
        } catch (error) {
          console.error(error);
          try {
            await this.dialog.message($localize`Driver Installed`, $localize`The driver package was installed, but SteamVR settings could not be updated atomically. Enable the installed driver in SteamVR and retry.`);
          } catch (dialogError) {
            console.error('Failed to show the SteamVR settings warning:', dialogError);
          }
        }
      }
      this.installing = false
      this.driverMutationInProgress = false
      this._installingDriver.set(false)
    }
  }
  async uninstallDriver() {
    if (this.driverMutationInProgress) return false;
    this.driverMutationInProgress = true;
    try {
      return await this.uninstallDriverInternal();
    } finally {
      this.driverMutationInProgress = false;
    }
  }
  private async uninstallDriverInternal() {
    const steamVrPath = this.steamVRinstalled();
    if (!steamVrPath) return false;
    if (!vendor) {
      try {
        const success = await uninstall_driver_transactional(steamVrPath);
        await this.checkDriverInstalled()
        return success;
      } catch (e) {
        await this.dialog.message($localize`Uninstall Failed, Make sure SteamVR is closed`, `${e}`)
        return false;
      }
    }
    // Remove copied driver from SteamVR drivers folder (if exists)
    const driverPath = await join(steamVrPath, 'drivers', customHeadsetDriverName);
    if (await exists(driverPath)) {
      try{
        await remove(driverPath, {recursive: true});
      } catch(e){
        await this.dialog.message($localize`Uninstall Failed, Make sure SteamVR is closed`, `${e}`)
        return false;
      }
    }
    // Also unregister any in-place registered driver using vrpathreg
    try {
      await this.unregisterDriver(steamVrPath);
    } catch(e) {
      console.warn('Failed to unregister driver using vrpathreg:', e);
    }
    this.checkDriverInstalled()
    return true;
  }
  async previewExistingInstallationsCleanup(): Promise<DriverCleanupPreview> {
    const steamVrPath = this.steamVRinstalled();
    if (!steamVrPath) {
      throw new Error('SteamVR installation was not found');
    }
    return await preview_driver_cleanup(steamVrPath);
  }
  async cleanExistingInstallations(planToken: string): Promise<DriverCleanupReport> {
    if (this.driverMutationInProgress) {
      throw new Error('Another driver install, uninstall, or cleanup operation is already running');
    }
    const steamVrPath = this.steamVRinstalled();
    if (!steamVrPath) {
      throw new Error('SteamVR installation was not found');
    }
    this.driverMutationInProgress = true;
    this._cleaningDriver.set(true);
    try {
      const report = await execute_driver_cleanup(steamVrPath, planToken);
      await this.checkDriverInstalled();
      return report;
    } finally {
      this._cleaningDriver.set(false);
      this.driverMutationInProgress = false;
    }
  }
  /**
   * Check if the driver is registered in-place via openvrpaths external_drivers.
   * Returns the registered driver directory, or "" when not registered.
   */
  public async checkDriverRegisteredPath(): Promise<string> {
    try {
      const openvrpaths = await this.getOpenvrpaths();
      if (!openvrpaths || !openvrpaths.external_drivers) {
        return "";
      }
      const drivers = openvrpaths.external_drivers;
      if (Array.isArray(drivers)) {
        for (const driverPath of drivers) {
          const lastFolder = await basename(driverPath);
          if (lastFolder === customHeadsetDriverName) {
            let manifestPath = await join(driverPath, 'driver.vrdrivermanifest')
            if(await exists(manifestPath)){
              return driverPath;
            }
          }
        }
      }
      return "";
    } catch (e) {
      console.warn('Failed to check driver registration from openvrpaths:', e);
      return "";
    }
  }
  /**
   * Legacy installs of this fork were copied into SteamVR/drivers under the
   * name CustomHeadsetOpenVR, colliding with the upstream vendor-neutral
   * driver of the same name. Detect such an install by its fingerprint (the
   * vrlink shaders only this fork ships) and remove it. A genuine upstream
   * CustomHeadsetOpenVR install can never match the fingerprint and is never
   * touched. Runs only in vendor builds, during install.
   */
  public async cleanupLegacyForkInstall(steamVrPath: string): Promise<void> {
    const isForkInstall = async (dir: string): Promise<boolean> => {
      try {
        const manifestPath = await join(dir, 'driver.vrdrivermanifest');
        if (!await exists(manifestPath)) return false;
        const manifest = JSON.parse(cleanJsonComments(await readTextFile(manifestPath)));
        if (manifest['name'] !== 'CustomHeadsetOpenVR') return false;
        // fingerprint: both fork-only shaders must be present
        const fp1 = await join(dir, 'resources', 'shaders', 'd3d11', 'vrlink_layer_ps.hlsl');
        const fp2 = await join(dir, 'resources', 'shaders', 'd3d11', 'vrlink_fxaa_ps.hlsl');
        return await exists(fp1) && await exists(fp2);
      } catch (e) {
        console.warn('Fingerprint check failed for', dir, e);
        return false;
      }
    };
    // 1. Copied install in SteamVR/drivers/CustomHeadsetOpenVR
    try {
      const copiedPath = await join(steamVrPath, 'drivers', 'CustomHeadsetOpenVR');
      if (await exists(copiedPath) && await isForkInstall(copiedPath)) {
        console.log('Removing legacy fork install at', copiedPath);
        await remove(copiedPath, { recursive: true });
      }
    } catch (e) {
      console.warn('Failed to remove legacy copied fork install:', e);
    }
    // 2. Registered external driver dirs named CustomHeadsetOpenVR with the fingerprint:
    // surgically drop only those entries from openvrpaths.external_drivers
    // (removedriverswithname would also remove a genuine upstream registration)
    try {
      const openvrpaths = await this.getOpenvrpaths();
      const drivers = openvrpaths?.external_drivers;
      if (Array.isArray(drivers)) {
        const keep: string[] = [];
        let removedAny = false;
        for (const driverPath of drivers) {
          const lastFolder = await basename(driverPath);
          if (lastFolder === 'CustomHeadsetOpenVR' && await isForkInstall(driverPath)) {
            console.log('Unregistering legacy fork install at', driverPath);
            removedAny = true;
            continue;
          }
          keep.push(driverPath);
        }
        if (removedAny) {
          openvrpaths.external_drivers = keep;
          const openVrConfigPath = await join(await appLocalDataDir(), '../openvr/openvrpaths.vrpath');
          await writeTextFile(openVrConfigPath, JSON.stringify(openvrpaths, undefined, 1).replaceAll('  ', '\t'));
        }
      }
    } catch (e) {
      console.warn('Failed to unregister legacy fork install:', e);
    }
  }
  private async getVrpathregPath(steamVrPath: string): Promise<string | undefined> {
    // this fork's GUI targets Windows only
    const vrpathregPath = await join(steamVrPath, 'bin', 'win64', 'vrpathreg.exe');
    if (!await exists(vrpathregPath)) {
      return undefined;
    }
    return vrpathregPath;
  }
  /**
   * Register driver in place using vrpathreg adddriver.
   * Verifies the driver directory name matches the expected driver name before registering.
   */
  private async registerDriver(steamVrPath: string, driverDir: string): Promise<boolean> {
    const vrpathregPath = await this.getVrpathregPath(steamVrPath);
    if (!vrpathregPath) {
      return false;
    }
    const lastFolder = await basename(driverDir);
    if (lastFolder !== customHeadsetDriverName) {
      console.warn(`Driver directory name "${lastFolder}" does not match expected "${customHeadsetDriverName}". Refusing to register.`);
      return false;
    }
    const exitCode = await run_process_sync(vrpathregPath, ['adddriver', driverDir]);
    return exitCode === 0;
  }
  /** Unregister only the exact in-place path owned by this package. */
  private async unregisterDriver(steamVrPath: string): Promise<void> {
    const vrpathregPath = await this.getVrpathregPath(steamVrPath);
    if (!vrpathregPath) {
      return;
    }
    const driverPath = await this.checkDriverRegisteredPath();
    if (driverPath) {
      await run_process_sync(vrpathregPath, ['removedriver', driverPath]);
    }
  }
  private async copyRec(targetDir: string, sourceDir: string) {
    if (!await exists(targetDir)) {
      await mkdir(targetDir)
    }
    const content = await readDir(sourceDir);
    for (const e of content) {
      if (e.isFile) {
        await copyFile(await join(sourceDir, e.name), await join(targetDir, e.name));
      } else if (e.isDirectory) {
        await this.copyRec(await join(targetDir, e.name), await join(sourceDir, e.name));
      }
    }
  }
  /**
   * 
   * @param update return true to save
   */
  public async updateSteamVRSettings(update: (steamVrSettings: any) => boolean) {
    const settings = await this.getSteamVRSettings();
    if (settings) {
      const path = await this.getSteamVRConfigFilePath();
      if (update(settings) && path) {
        await write_json_file_transactional(path, JSON.stringify(settings, undefined, 4))
      }
    }
  }
  public async getSteamVRSettings() {
    const path = await this.getSteamVRConfigFilePath();
    if (path) {
      return JSON.parse(await readTextFile(path));
    }
  }
  public async getSteamVRConfigFilePath(): Promise<string | undefined> {
    const dirPath = await this.getSteamVRConfigDirPath()
    if (dirPath) {
      return await join(dirPath, 'steamvr.vrsettings')
    }
    return undefined;
  }
  public async getSteamVRConfigDirPath(): Promise<string | undefined> {
    const openvrpaths = await this.getOpenvrpaths();
    if (openvrpaths) {
      const path = openvrpaths?.['config'];
      if (path && typeof path == 'object' && Array.isArray(path)) {
        const configFolderPath = path[0];
        if (configFolderPath) {
          return configFolderPath;
        }
      }
    }
    return undefined;
  }
  public async checkSteamVrInstalled(): Promise<string | undefined> {
    const openvrpaths = await this.getOpenvrpaths();
    const runtime = openvrpaths?.['runtime'];
    if (runtime && typeof runtime == 'object' && Array.isArray(runtime)) {
      const steamVrPath = runtime.find(x => typeof x == 'string' && x.endsWith('SteamVR'));
      if (steamVrPath) {
        if (await exists(await join(steamVrPath, 'bin', 'version.txt'))) {
          this._steamVRinstalled.set(steamVrPath);
          this.pullingSteamVRinstall.stop()
          return steamVrPath;
        }
      }
    }
    this._steamVRinstalled.set(undefined);
    return undefined;
  }
  public async resetDriverSetting() {
    await writeTextFile(this.dss.filePath, "{}")
  }
  public async restartCompositor() {
    return await restart_vrcompositor()
  }
  public async launchSteamVR(){
    // Try direct SteamVR executable paths first
    const steamvrPaths = [
      'C:/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/win64/vrstartup.exe',
      'C:/Program Files/Steam/steamapps/common/SteamVR/bin/win64/vrstartup.exe',
    ];
    for (const path of steamvrPaths) {
      const success = await launch_process(path, []);
      if (success) {
        console.log('SteamVR launched successfully from', path);
        return;
      }
    }

    // Fallback to Steam protocol URL
    try {
      await openUrl('steam://rungameid/250820');
      console.log('SteamVR launch requested via Steam protocol');
    } catch (error) {
      console.log('Failed to launch SteamVR:', error);
    }
  }
}
