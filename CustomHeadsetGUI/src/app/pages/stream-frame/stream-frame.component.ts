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
import { Settings, StreamFrameConfig } from '../../services/JsonFileDefines';
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
    k1: 0,
    k2: 0,
    distortion: {
      mode: 'k1k2',
      points: [],
      perEye: false,
      perAxis: false,
      curves: {},
      annulus: { enable: false, rMin: 0, rMax: 0.75, feather: 0.05 }
    },
    centerOffsetXLeft: 0,
    centerOffsetXRight: 0,
    centerOffsetY: 0,
    skipColorWhileDashboardOpen: false,
    processAtSubmitLayer: false
  };
}

// fill missing fields without touching set ones, so older settings files and
// files written before this page existed load into a complete object
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
  settings?: StreamFrameConfig;
  defaults: StreamFrameConfig = defaultStreamFrame();
  // bumped on every edit so the curve component redraws immediately
  revision = signal(0);
  matrixText = signal('');
  matrixError = signal('');

  constructor() {
    effect(() => {
      this.rootSetting = this.dss.values();
      if (this.rootSetting) {
        this.rootSetting.streamFrame = fillDefaults(this.rootSetting.streamFrame, defaultStreamFrame());
        this.settings = this.rootSetting.streamFrame;
        this.matrixText.set((this.settings?.srgbMatrix ?? []).join(', '));
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
