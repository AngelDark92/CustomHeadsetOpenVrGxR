import { computed, Injectable } from '@angular/core';
import { readDir, readTextFile, exists, DirEntry, size, copyFile, remove, watchImmediate } from '@tauri-apps/plugin-fs';
import { basename, join, } from '@tauri-apps/api/path';
import { Settings } from './JsonFileDefines';
import { debounceTime, Subject } from 'rxjs';
import { signal } from '@angular/core';
import { PathsService } from './paths.service';;
import { JsonSettingServiceBase } from './JsonSettingServiceBase';
import { cleanJsonComments } from '../helpers';
import { DriverInfoService } from './driver-info.service';
export type DistortionProfileEntry = {
  name: string;
  displayName?: string;
  isDefault: boolean;
  file?: DirEntry;
};
export type DistortionProfileFile = {
  name: string;
  device?: string;
  entry: DirEntry;
};
@Injectable({
  providedIn: 'root'
})
export class DriverSettingService extends JsonSettingServiceBase<Settings> {
  private readonly _distortionProfileList = signal<DistortionProfileFile[]>([]);
  public readonly distortionProfileList = this._distortionProfileList.asReadonly();
  constructor(private pathService: PathsService, driverInfoService: DriverInfoService) {
    super(pathService.settingPath, pathService.appDataDirPath, computed(() => driverInfoService.values()?.defaultSettings ?? ({} as Settings)), false, true);
  }
  // migration: keys the driver has retired are deleted on load so the
  // next natural save writes a clean settings.json. Prune-on-save
  // incident 2026-08-11: the round-trip writer preserved a legacy
  // "velocityFix" bool for months; the default-diff serializer then
  // pruned the explicit velocityFixMode the moment it matched the new
  // published default, and the fossil took over mode selection.
  private static readonly retiredStreamFrameKeys = ['velocityFix', 'kalmanDupSkip', 'kalmanAdaptiveBoost'];
  protected override migrateLoadedValues(values: Settings): Settings {
    const sf = (values as any)?.streamFrame;
    if (sf) {
      for (const key of DriverSettingService.retiredStreamFrameKeys) {
        if (key in sf) {
          delete sf[key];
        }
      }
    }
    return values;
  }
  protected override async init() {
    await super.init();
    await this.listDistortionProfiles();
    const distrotionProfileSbj = new Subject<void>();
    distrotionProfileSbj.pipe(debounceTime(20)).subscribe(() => {
      this.listDistortionProfiles();
    })
    watchImmediate(this.pathService.distortionDirPath, (ev) => {
      if (ev.paths.some(x => x.match(/\.json$/))) {
        distrotionProfileSbj.next();
      }
    });
  }
  private async listDistortionProfiles() {
    const files = (await readDir(this.pathService.distortionDirPath))
      .filter(x => x.isFile && x.name.endsWith('.json'))
    const profiles: DistortionProfileFile[] = [];
    for (const file of files) {
      const profile: DistortionProfileFile = {
        name: file.name,
        entry: file
      };
      try {
        const fullPath = await join(this.pathService.distortionDirPath, file.name);
        const content = await readTextFile(fullPath);
        const parsed = JSON.parse(cleanJsonComments(content));
        if (parsed.device) {
          profile.device = parsed.device;
        }
      } catch (e) {
        // ignore parse errors
      }
      profiles.push(profile);
    }
    this._distortionProfileList.set(profiles);
  }

  async delete(name: string) {
    const path = await this.pathService.getProfileFullPath(name);
    if (await exists(path)) {
      await remove(path);
    }
  }

  async importFile(paths: string[]) {
    const message: { [path: string]: { success: boolean, message?: string } } = {}
    for (let path of paths) {
      try {
        const l = await size(path);
        if (l > 5000) {
          message[path] = { success: false, message: $localize`file too large` }
          continue;
        }
        const json = await readTextFile(path);
        try {
          const dpObj = JSON.parse(cleanJsonComments(json));
          if (!('type' in dpObj) || !('distortions' in dpObj)) {
            message[path] = { success: false, message: $localize`import file not valid, missing "type" or "distortions"?` }
            continue;
          }
        } catch (jex) {
          message[path] = { success: false, message: $localize`import file not valid ${jex}` }
        }
        await copyFile(path, await join(this.pathService.distortionDirPath, await basename(path)));
        message[path] = { success: true }
      } catch (ex) {
        message[path] = { success: false, message: $localize`import file error ${ex}` }
      }
    }
    return message;
  }

}
