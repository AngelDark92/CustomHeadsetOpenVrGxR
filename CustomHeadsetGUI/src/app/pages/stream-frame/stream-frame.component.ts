import { Component, effect, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { MatSliderModule } from '@angular/material/slider';
import { MatSelectModule } from '@angular/material/select';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { DriverSettingService } from '../../services/driver-setting.service';
import { DriverInfoService } from '../../services/driver-info.service';
import { Settings, StreamFrameConfig, ControllersConfig } from '../../services/JsonFileDefines';
import { FieldTipComponent } from '../../utilities/field-tip/field-tip.component';
import { ResetButtonComponent } from '../../utilities/reset-button/reset-button.component';
import { StreamFrameCurveComponent } from '../../utilities/stream-frame-curve/stream-frame-curve.component';

function defaultStreamFrame(): StreamFrameConfig {
  return {
    enable: false,
    saturation: 50,
    contrast: 50,
    contrastMidpoint: 50,
    contrastLinear: false,
    gamma: 2.2,
    colorMultiplier: { r: 1, g: 1, b: 1 },
    srgbMatrix: [],
    cas: { enable: false, strength: 0.5 },
    dither: false,
    stationaryDimming: { enable: false, movementThreshold: 0.4, movementTime: 15, dimSeconds: 10, brightenSeconds: 1 },
    k1: 0,
    k2: 0,
    distortion: {
      gain: 1, mode: 'k1k2',
      points: [],
      perEye: false,
      perAxis: false,
      curves: {},
      annulus: { enable: false, rMin: 0, rMax: 0.75, feather: 0.05 },
      tune: { enable: false, rate: 0.08, bands: [0.15, 0.22, 0.3, 0.38, 0.46, 0.55, 0.65], stepSize: 0, ringOpacity: 0.55, forceGrid: true },
      centerTune: { enable: false, breatheAmp: 0.05 }
    },
    centerOffsetXLeft: 0,
    centerOffsetXRight: 0,
    centerOffsetY: 0,
    skipColorWhileDashboardOpen: false,
    processAtSubmitLayer: false,
    syncTimeoutMs: 5,
    velocityFix: false,
    velocityFixMode: 'off',
    eyeGaze: { debugRing: false, tanHalfFovX: 1.19, tanHalfFovY: 1.19, predictionMs: 30, debugGrid: false, gridMode: 'uv', gridAngularDeg: 2.5, calibDot: false, swimProbe: false, overlayWarped: false, probeCapture: false, gridWorldLocked: false },
    pupilSwim: { centerStrengthX: 0, centerStrengthY: 0 },
    poseLogging: false
  };
}

// fill missing fields without touching set ones, so older settings files and
// files written before this page existed load into a complete object
function defaultControllers(): ControllersConfig {
  return {
    rotationOffsetDeg: { x: 0, y: 0, z: 0 },
    positionOffsetCm: { x: 0, y: 0, z: 0 },
    aligner: { enable: false },
  };
}

function fillDefaults(target: any, defaults: any): any {
  if (target === undefined || target === null) {
    return JSON.parse(JSON.stringify(defaults));
  }
  if (typeof defaults === 'object' && defaults !== null && !Array.isArray(defaults) && typeof target === 'object') {
    for (const key of Object.keys(defaults)) {
      target[key] = fillDefaults(target[key], defaults[key]);
    }
  }
  return target;
}

@Component({
  selector: 'app-stream-frame',
  imports: [
    CommonModule,
    FormsModule,
    MatSlideToggleModule,
    MatSliderModule,
    MatSelectModule,
    MatButtonModule,
    MatIconModule,
    MatInputModule,
    FieldTipComponent,
    ResetButtonComponent,
    StreamFrameCurveComponent
  ],
  templateUrl: './stream-frame.component.html',
  styleUrl: './stream-frame.component.scss'
})
export class StreamFrameComponent {
  dss = inject(DriverSettingService);
  dis = inject(DriverInfoService);

  rootSetting?: Settings;
  controllerSettings?: ControllersConfig;
  controllerDefaults: ControllersConfig = defaultControllers();
  settings?: StreamFrameConfig;
  defaults: StreamFrameConfig = defaultStreamFrame();
  // bumped on every edit so the curve component redraws immediately
  revision = signal(0);
  // friendly band layout inputs; the driver consumes the raw bands array,
  // these three regenerate it evenly spaced on change
  tuneBandCount = 7;
  tuneBandFirst = 0.15;
  tuneBandLast = 0.65;
  matrixText = signal('');
  matrixError = signal('');

  constructor() {
    effect(() => {
      this.rootSetting = this.dss.values();
      if (this.rootSetting) {
        this.rootSetting.streamFrame = fillDefaults(this.rootSetting.streamFrame, defaultStreamFrame());
        this.rootSetting.controllers = fillDefaults(this.rootSetting.controllers, defaultControllers());
        this.controllerSettings = this.rootSetting.controllers;
        this.settings = this.rootSetting.streamFrame;
        this.matrixText.set((this.settings?.srgbMatrix ?? []).join(', '));
        const bands = this.settings?.distortion?.tune?.bands;
        if (bands && bands.length > 0) {
          this.tuneBandCount = bands.length;
          this.tuneBandFirst = bands[0];
          this.tuneBandLast = bands[bands.length - 1];
        }
      }
      const infoDefaults = (this.dis.values()?.defaultSettings as any)?.streamFrame;
      this.defaults = fillDefaults(infoDefaults ? JSON.parse(JSON.stringify(infoDefaults)) : undefined, defaultStreamFrame());
      this.revision.update(x => x + 1);
    });
  }

  // custom shader also applying color to streamed frames while the dashboard is
  // open causes double application and the "works only with dashboard" confusion
  customShaderConflict(): boolean {
    const cs = this.rootSetting?.customShader;
    const sf = this.settings;
    if (!cs || !sf || !sf.enable) return false;
    if (!cs.enable || !cs.enableForOther) return false;
    if (sf.skipColorWhileDashboardOpen) return false;
    return (cs as any).saturation !== 50 || cs.contrast !== 50;
  }

  resetControllers(group: keyof ControllersConfig) {
    if (this.controllerSettings) {
      this.controllerSettings[group] = JSON.parse(JSON.stringify(this.controllerDefaults[group]));
      this.save();
    }
  }

  // regenerate the tuner band array evenly spaced from the three layout inputs
  updateBands() {
    if (!this.settings) return;
    let count = Math.round(this.tuneBandCount);
    if (!(count >= 2)) count = 2;
    if (count > 12) count = 12;
    let first = this.tuneBandFirst;
    let last = this.tuneBandLast;
    if (!(first > 0.02)) first = 0.02;
    if (!(last > first)) last = first + 0.05;
    if (last > 1.2) last = 1.2;
    const bands: number[] = [];
    for (let i = 0; i < count; i++) {
      bands.push(Math.round((first + (last - first) * i / (count - 1)) * 1000) / 1000);
    }
    this.tuneBandCount = count;
    this.tuneBandFirst = first;
    this.tuneBandLast = last;
    this.settings.distortion.tune.bands = bands;
    this.save();
  }

  save() {
    if (this.rootSetting) {
      this.dss.save(this.rootSetting);
    }
    this.revision.update(x => x + 1);
  }

  reset(key: keyof StreamFrameConfig) {
    if (!this.settings) return;
    (this.settings as any)[key] = JSON.parse(JSON.stringify((this.defaults as any)[key]));
    if (key === 'srgbMatrix') {
      this.matrixText.set(this.settings.srgbMatrix.join(', '));
      this.matrixError.set('');
    }
    this.save();
  }

  // ---- distortion profile sharing ----
  shareText = signal('');
  shareStatus = signal('');
  // collapsible section state; debug starts closed, everything else open.
  // concrete shape (no index signature) so strict templates allow dot access
  sections = { color: true, enhance: true, distortion: true, share: true, advanced: true, debug: false };
  // any calibration overlay/mode that would be visible or disruptive in a
  // normal play session — drives the warning banner at the top of the page
  calibrationActive(): boolean {
    const s = this.settings;
    const c = this.controllerSettings;
    if (!s) return false;
    return !!(s.distortion?.tune?.enable || s.distortion?.centerTune?.enable
      || c?.aligner?.enable || s.eyeGaze?.probeCapture || s.eyeGaze?.debugGrid
      || s.eyeGaze?.calibDot || s.eyeGaze?.debugRing || s.eyeGaze?.overlayWarped
      || s.eyeGaze?.swimProbe);
  }
  private buildProfile(): any {
    const s = this.settings!;
    const profile: any = {
      type: 'streamFrameDistortionProfile',
      version: 1,
      name: 'My Galaxy XR profile',
      distortion: JSON.parse(JSON.stringify(s.distortion)),
      k1: s.k1,
      k2: s.k2,
      centerOffsetXLeft: s.centerOffsetXLeft,
      centerOffsetXRight: s.centerOffsetXRight,
      centerOffsetY: s.centerOffsetY
    };
    // the annulus and tuners are tuning diagnostics, not part of a shareable profile
    delete profile.distortion.annulus;
    delete profile.distortion.tune;
    delete profile.distortion.centerTune;
    return profile;
  }
  exportJsonFile() {
    if (!this.settings) return;
    const text = JSON.stringify(this.buildProfile(), null, 2);
    const blob = new Blob([text], { type: 'application/json' });
    const anchor = document.createElement('a');
    anchor.href = URL.createObjectURL(blob);
    const stamp = new Date().toISOString().slice(0, 10);
    anchor.download = 'gxr-distortion-profile-' + stamp + '.json';
    anchor.click();
    URL.revokeObjectURL(anchor.href);
    this.shareStatus.set('Profile downloaded as ' + anchor.download);
  }
  importJsonFile(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    file.text().then(text => {
      this.shareText.set(text);
      this.importProfile();
      input.value = '';
    });
  }
  exportProfile() {
    if (!this.settings) return;
    const s = this.settings;
    const profile = {
      type: 'streamFrameDistortionProfile',
      version: 1,
      name: 'My Galaxy XR profile',
      distortion: JSON.parse(JSON.stringify(s.distortion)),
      k1: s.k1,
      k2: s.k2,
      centerOffsetXLeft: s.centerOffsetXLeft,
      centerOffsetXRight: s.centerOffsetXRight,
      centerOffsetY: s.centerOffsetY
    };
    // the annulus and tuner are tuning diagnostics, not part of a shareable profile
    delete profile.distortion.annulus;
    delete profile.distortion.tune;
    delete profile.distortion.centerTune;
    const text = JSON.stringify(profile, null, 2);
    this.shareText.set(text);
    this.shareStatus.set('Profile exported below. Copy it anywhere.');
    navigator.clipboard?.writeText(text).then(
      () => this.shareStatus.set('Profile copied to clipboard.'),
      () => {}
    );
  }
  importProfile() {
    if (!this.settings) return;
    let parsed: any;
    try {
      parsed = JSON.parse(this.shareText());
    } catch (e: any) {
      this.shareStatus.set('Not valid JSON: ' + e.message);
      return;
    }
    // controller-aligner save files ({"controllers": {...}}) import here too,
    // applying straight into the offset fields below
    const controllers = parsed?.controllers;
    if (controllers && (controllers.rotationOffsetDeg || controllers.positionOffsetCm) && this.controllerSettings) {
      const applyAxes = (target: { x: number, y: number, z: number }, source: any) => {
        for (const axis of ['x', 'y', 'z'] as const) {
          if (Number.isFinite(source?.[axis])) target[axis] = source[axis];
        }
      };
      applyAxes(this.controllerSettings.rotationOffsetDeg, controllers.rotationOffsetDeg);
      applyAxes(this.controllerSettings.positionOffsetCm, controllers.positionOffsetCm);
      this.save();
      this.shareStatus.set('Controller offsets imported and applied.');
      return;
    }
    if (parsed?.type !== 'streamFrameDistortionProfile' || typeof parsed.distortion !== 'object') {
      this.shareStatus.set('Not a stream frame distortion profile.');
      return;
    }
    const num = (v: any, fallback: number) => (Number.isFinite(v) ? v : fallback);
    const s = this.settings;
    // center-tuner saves apply ONLY the center offsets: importing a stale
    // centers file must never roll the curves back to its embedded snapshot
    if (parsed.centersOnly) {
      s.centerOffsetXLeft = num(parsed.centerOffsetXLeft, s.centerOffsetXLeft);
      s.centerOffsetXRight = num(parsed.centerOffsetXRight, s.centerOffsetXRight);
      s.centerOffsetY = num(parsed.centerOffsetY, s.centerOffsetY);
      this.save();
      this.revision.update(v => v + 1);
      this.shareStatus.set('Center offsets imported and applied (curves untouched).');
      return;
    }
    const d = parsed.distortion;
    s.distortion.mode = d.mode === 'spline' ? 'spline' : 'k1k2';
    s.distortion.perEye = !!d.perEye;
    s.distortion.perAxis = !!d.perAxis;
    const parsePoints = (arr: any) => Array.isArray(arr)
      ? arr.filter((p: any) => Number.isFinite(p?.r) && Number.isFinite(p?.scale)).map((p: any) => ({ r: p.r, scale: p.scale }))
      : [];
    s.distortion.points = parsePoints(d.points);
    s.distortion.curves = {};
    if (d.curves && typeof d.curves === 'object') {
      for (const key of Object.keys(d.curves)) {
        const c = d.curves[key];
        s.distortion.curves[key] = { k1: num(c?.k1, 0), k2: num(c?.k2, 0), points: parsePoints(c?.points) };
      }
    }
    s.k1 = num(parsed.k1, 0);
    s.k2 = num(parsed.k2, 0);
    s.centerOffsetXLeft = num(parsed.centerOffsetXLeft, 0);
    s.centerOffsetXRight = num(parsed.centerOffsetXRight, 0);
    s.centerOffsetY = num(parsed.centerOffsetY, 0);
    this.save();
    this.shareStatus.set('Profile applied' + (parsed.name ? ': ' + parsed.name : '.'));
  }

  onMatrixTextChanged(text: string) {
    this.matrixText.set(text);
    if (!this.settings) return;
    const trimmed = text.trim();
    if (trimmed === '') {
      this.settings.srgbMatrix = [];
      this.matrixError.set('');
      this.save();
      return;
    }
    const values = trimmed.split(/[\s,;]+/).map(Number);
    if (values.length === 9 && values.every(x => Number.isFinite(x))) {
      this.settings.srgbMatrix = values;
      this.matrixError.set('');
      this.save();
    } else {
      this.matrixError.set('Needs exactly 9 numbers (row major 3x3), or empty to disable');
    }
  }
}
