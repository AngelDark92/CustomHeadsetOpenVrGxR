import { Component, effect, inject, signal } from '@angular/core';
import { open } from '@tauri-apps/plugin-shell';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { AppUpdateInfoSuccess, AppUpdateService } from '../../services/app-update.service';
import { delay, isNewVersion } from '../../helpers';
import { DriverInfoService } from '../../services/driver-info.service';
import { SystemDiagnosticService } from '../../services/system-diagnostic.service';
import { DialogService } from '../../services/dialog.service';
import { DriverSettingService } from '../../services/driver-setting.service'
import {FieldTipComponent} from '../../utilities/field-tip/field-tip.component'
@Component({
  selector: 'app-about',
  imports: [MatButtonModule, MatIconModule, FieldTipComponent,],
  providers: [],
  templateUrl: './about.component.html',
  styleUrl: './about.component.scss'
})
export class AboutComponent {

  public isNewVersion = isNewVersion;
  public checking = signal<boolean>(false)
  public updateInfo = signal<AppUpdateInfoSuccess | undefined>(undefined)
  public dss = inject(DriverSettingService)
  private oldMeganeXEdidVendor: number | undefined = undefined
  private oldDreamAirEidVendor: number | undefined = undefined
  public driverVersionMismatch = signal<boolean>(false);
  constructor(public aus: AppUpdateService, public dis: DriverInfoService, public sds: SystemDiagnosticService, private dialog: DialogService) {
    effect(() => {
      const installedVersion = this.sds.driverInstalled();
      const lastRunVersion = this.dis.values()?.driverVersion;
      this.driverVersionMismatch.set(!!installedVersion && !!lastRunVersion && installedVersion !== lastRunVersion);
    });
    effect(() => {
        let newSettings = this.dss.values()
        
        if(newSettings?.meganeX8K?.edidVendorIdOverride != undefined && this.oldMeganeXEdidVendor != undefined && newSettings.meganeX8K.edidVendorIdOverride != this.oldMeganeXEdidVendor) {
          sds.restartCompositor()
        }
        this.oldMeganeXEdidVendor = newSettings?.meganeX8K?.edidVendorIdOverride
        if(newSettings?.dreamAir?.edidVendorIdOverride != undefined && this.oldDreamAirEidVendor != undefined && newSettings.dreamAir.edidVendorIdOverride != this.oldDreamAirEidVendor) {
          sds.restartCompositor()
        }
        this.oldDreamAirEidVendor = newSettings?.dreamAir?.edidVendorIdOverride
    });
  }
  async openExternal(event: Event, url: string) {
    event.preventDefault()
    await open(url)
  }

  async checkUpdate() {
    this.checking.set(true)
    try {
      const start = performance.now();
      await this.aus.checkUpdate();

      const wait = 2000 - (performance.now() - start);
      if (wait > 0) {
        await delay(wait)
      }
    } finally {
      this.checking.set(false)
    }
  }
  async installDriver() {
    if (await this.sds.installDriver()) {
      this.dialog.message($localize`Install success`, $localize`please launch SteamVR to finish the installation`)
    }
  }
  async uninstallDriver(){
    if (await this.sds.uninstallDriver()) {
      this.dialog.message($localize`Uninstall success`, $localize`Successfully uninstalled the driver`)
    }
  }
  async cleanExistingInstallations() {
    try {
      const preview = await this.sds.previewExistingInstallationsCleanup();
      if (preview.steamVrRunning) {
        await this.dialog.message(
          $localize`Cleanup Blocked`,
          $localize`SteamVR is running. Close SteamVR completely, then preview the cleanup again.`,
        );
        return;
      }
      if (preview.blockers.length) {
        await this.dialog.message(
          $localize`Cleanup Blocked`,
          $localize`The cleanup cannot continue safely:` + `\n\n${preview.blockers.join('\n')}`,
        );
        return;
      }
      if (!preview.actions.length) {
        await this.dialog.message(
          $localize`Nothing to Clean`,
          $localize`No existing CustomHeadsetOpenVR or Galaxy XR driver installations were found.`,
        );
        return;
      }
      const message = [
        $localize`The following exact installations and files will be removed:`,
        '',
        ...preview.actions.map(action => `• ${action}`),
        '',
        $localize`The following data will be preserved:`,
        ...preview.preserved.map(item => `• ${item}`),
        '',
        $localize`This cannot be undone. Continue only if SteamVR is closed.`,
      ].join('\n');
      const confirmed = await this.dialog.confirm(
        $localize`Clean Existing Driver Installations`,
        message,
        $localize`Clean Installations`,
        'warn',
      );
      if (!confirmed) return;

      const report = await this.sds.cleanExistingInstallations(preview.planToken);
      const result = [
        $localize`Cleanup completed.`,
        '',
        $localize`Removed:`,
        ...(report.removed.length ? report.removed.map(path => `• ${path}`) : [$localize`• None`]),
        '',
        $localize`Unregistered:`,
        ...(report.unregistered.length ? report.unregistered.map(path => `• ${path}`) : [$localize`• None`]),
      ];
      if (report.warnings.length) {
        result.push('', $localize`Warnings:`, ...report.warnings.map(warning => `• ${warning}`));
      }
      await this.dialog.message($localize`Cleanup Complete`, result.join('\n'));
    } catch (error) {
      await this.dialog.message($localize`Cleanup Failed`, `${error}`);
    }
  }
}
