import { Component, ElementRef, effect, inject, input, output, signal, untracked, viewChild, AfterViewInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { StreamFrameConfig, StreamFrameCurveData, StreamFrameDistortionPoint } from '../../services/JsonFileDefines';
import { DriverSettingService } from '../../services/driver-setting.service';

// math mirrored from FrameProcessor.cpp / vrlink_layer_ps.hlsl / streamframe-visualizer.html
function evaluateCurve(points: StreamFrameDistortionPoint[], r: number): number {
  if (points.length === 0) return 1.0;
  if (points.length === 1 || r <= points[0].r) return r <= points[0].r ? points[0].scale : points[points.length - 1].scale;
  if (r >= points[points.length - 1].r) return points[points.length - 1].scale;
  let i = 0;
  while (i + 2 < points.length && r > points[i + 1].r) i++;
  const r0 = points[i].r, r1 = points[i + 1].r, v0 = points[i].scale, v1 = points[i + 1].scale;
  const h = r1 - r0;
  if (h <= 0) return v0;
  let m0: number, m1: number;
  if (i === 0) { m0 = (v1 - v0) / h; }
  else { const hr = points[i + 1].r - points[i - 1].r; m0 = hr > 0 ? (points[i + 1].scale - points[i - 1].scale) / hr : 0; }
  if (i + 2 >= points.length) { m1 = (v1 - v0) / h; }
  else { const hr = points[i + 2].r - points[i].r; m1 = hr > 0 ? (points[i + 2].scale - points[i].scale) / hr : 0; }
  const t = (r - r0) / h, t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * h * m0 + (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * h * m1;
}
function smoothstep(a: number, b: number, x: number): number {
  const t = Math.min(1, Math.max(0, (x - a) / Math.max(1e-9, b - a)));
  return t * t * (3 - 2 * t);
}

@Component({
  selector: 'app-stream-frame-curve',
  imports: [CommonModule, FormsModule, MatButtonModule, MatSelectModule],
  templateUrl: './stream-frame-curve.component.html',
  styleUrl: './stream-frame-curve.component.scss'
})
export class StreamFrameCurveComponent implements AfterViewInit {
  settings = input.required<StreamFrameConfig>();
  // parent increments on any external change so the plots redraw promptly
  revision = input<number>(0);
  changed = output<void>();

  curveCanvas = viewChild.required<ElementRef<HTMLCanvasElement>>('curveCanvas');
  previewCanvas = viewChild.required<ElementRef<HTMLCanvasElement>>('previewCanvas');

  private dss = inject(DriverSettingService);

  // vertical range of the plot around 1.0, fixed so dragging feels stable
  scaleRange = signal(0.05);
  scaleRangeOptions = [0.01, 0.02, 0.05, 0.1, 0.2];
  exaggeration = signal(8);

  // which curve is being edited: 'base' or a named per eye / per axis curve
  curveKey = signal('base');
  curveLabels: { [key: string]: string | undefined } = {
    base: 'Curve', left: 'Left eye', right: 'Right eye',
    horizontal: 'Horizontal', vertical: 'Vertical',
    leftHorizontal: 'Left / horizontal', leftVertical: 'Left / vertical',
    rightHorizontal: 'Right / horizontal', rightVertical: 'Right / vertical'
  };

  private dragIndex = -1;
  private viewReady = false;

  constructor() {
    // keep the stored selection valid when per-eye/per-axis toggles or an
    // imported profile change the available keys (write moved here from
    // activeCurve: signal writes are allowed in effects, not in render)
    effect(() => {
      // settings are mutated in place; the parent signals edits via the
      // revision input, so track it to catch per-eye/per-axis toggles
      this.revision();
      this.dss.values();
      const keys = this.curveKeys();
      if (!keys.includes(untracked(() => this.curveKey()))) {
        this.curveKey.set(keys[0]);
      }
    });
    effect(() => {
      // redraw on external settings reloads, parent edits, and local ui state
      this.dss.values();
      this.revision();
      this.scaleRange();
      this.exaggeration();
      if (this.viewReady) {
        requestAnimationFrame(() => this.draw());
      }
    });
  }
  ngAfterViewInit() {
    this.viewReady = true;
    this.draw();
  }

  // ---- curve selection ----
  curveKeys(): string[] {
    const d = this.settings().distortion;
    if (d.perEye && d.perAxis) return ['leftHorizontal', 'leftVertical', 'rightHorizontal', 'rightVertical'];
    if (d.perEye) return ['left', 'right'];
    if (d.perAxis) return ['horizontal', 'vertical'];
    return ['base'];
  }
  // the storage behind a curve key. 'base' lives at the legacy locations,
  // named curves are seeded from the base curve on first access.
  curveData(key: string, seed: boolean): StreamFrameCurveData {
    const cfg = this.settings();
    if (key === 'base') {
      // facade over the legacy storage so all code paths look the same
      const self = this;
      return {
        get k1() { return cfg.k1; }, set k1(v: number) { cfg.k1 = v; void self; },
        get k2() { return cfg.k2; }, set k2(v: number) { cfg.k2 = v; },
        get points() { return cfg.distortion.points; }, set points(v) { cfg.distortion.points = v; }
      } as StreamFrameCurveData;
    }
    if (!cfg.distortion.curves) cfg.distortion.curves = {};
    let curve = cfg.distortion.curves[key];
    if (!curve && seed) {
      curve = {
        k1: cfg.k1, k2: cfg.k2,
        points: JSON.parse(JSON.stringify(cfg.distortion.points))
      };
      cfg.distortion.curves[key] = curve;
    }
    return curve ?? { k1: cfg.k1, k2: cfg.k2, points: cfg.distortion.points };
  }
  // the key to actually use this render: falls back to the first valid key
  // when the stored selection doesn't exist in the current mode (e.g. a
  // per-eye profile was just imported while 'base' was selected). READ-ONLY:
  // writing curveKey here threw NG0600 (template expressions run in a
  // reactive context in Angular 19) and aborted the whole page's first
  // render — the stored signal is normalized by the constructor effect.
  private effectiveKey(): string {
    const keys = this.curveKeys();
    const key = this.curveKey();
    return keys.includes(key) ? key : keys[0];
  }
  activeCurve(seed = true): StreamFrameCurveData {
    return this.curveData(this.effectiveKey(), seed);
  }

  // ---- shared curve math (annulus + exaggeration applied for the preview only) ----
  private scaleAtCurve(curve: StreamFrameCurveData, r: number, exaggeration: number): number {
    const cfg = this.settings();
    let s: number;
    if (cfg.distortion.mode === 'spline') {
      const points = [...curve.points].sort((a, b) => a.r - b.r);
      s = evaluateCurve(points, r);
    } else {
      s = 1 + (curve.k1 || 0) * r * r + (curve.k2 || 0) * r * r * r * r;
    }
    const an = cfg.distortion.annulus;
    if (an?.enable) {
      const f = an.feather ?? 0.05;
      const w = smoothstep(an.rMin - f, an.rMin + f, r) * (1 - smoothstep(an.rMax - f, an.rMax + f, r));
      s = 1 + (s - 1) * w;
    }
    return 1 + (s - 1) * exaggeration;
  }
  private rawScale(curve: StreamFrameCurveData, r: number): number {
    const cfg = this.settings();
    if (cfg.distortion.mode === 'spline') {
      const points = [...curve.points].sort((a, b) => a.r - b.r);
      return evaluateCurve(points, r);
    }
    return 1 + (curve.k1 || 0) * r * r + (curve.k2 || 0) * r * r * r * r;
  }
  // the full directional field for the preview: per axis blends the horizontal
  // and vertical curves of the previewed eye by the squared direction cosine,
  // exactly like the shader, then applies annulus and exaggeration
  private directionalScale(r: number, wH: number, exaggeration: number): number {
    const cfg = this.settings();
    const d = cfg.distortion;
    let s: number;
    if (d.perAxis) {
      const eyePrefix = d.perEye ? (this.curveKey().startsWith('right') ? 'right' : 'left') : '';
      const hKey = eyePrefix ? eyePrefix + 'Horizontal' : 'horizontal';
      const vKey = eyePrefix ? eyePrefix + 'Vertical' : 'vertical';
      s = this.rawScale(this.curveData(hKey, false), r) * wH + this.rawScale(this.curveData(vKey, false), r) * (1 - wH);
    } else {
      s = this.rawScale(this.activeCurve(false), r);
    }
    const an = d.annulus;
    if (an?.enable) {
      const f = an.feather ?? 0.05;
      const w = smoothstep(an.rMin - f, an.rMin + f, r) * (1 - smoothstep(an.rMax - f, an.rMax + f, r));
      s = 1 + (s - 1) * w;
    }
    return 1 + (s - 1) * exaggeration;
  }

  // ---- coordinate mapping for the curve plot ----
  private toX(r: number, w: number) { return r * w; }
  private toY(s: number, h: number) { return h / 2 - (s - 1) / this.scaleRange() * (h / 2 - 12); }
  private fromX(x: number, w: number) { return Math.min(1, Math.max(0, x / w)); }
  private fromY(y: number, h: number) { return 1 + (h / 2 - y) / (h / 2 - 12) * this.scaleRange(); }

  draw() {
    this.drawCurve();
    this.drawPreview();
  }

  private drawCurve() {
    const canvas = this.curveCanvas().nativeElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    const cfg = this.settings();

    // annulus band
    const an = cfg.distortion.annulus;
    if (an?.enable) {
      ctx.fillStyle = 'rgba(200, 150, 150, 0.15)';
      ctx.fillRect(this.toX(an.rMin, w), 0, this.toX(an.rMax, w) - this.toX(an.rMin, w), h);
    }
    // identity line + labels
    ctx.strokeStyle = 'rgba(128,128,128,0.8)';
    ctx.beginPath(); ctx.moveTo(0, this.toY(1, h)); ctx.lineTo(w, this.toY(1, h)); ctx.stroke();
    ctx.fillStyle = 'rgba(128,128,128,1)'; ctx.font = '11px monospace';
    ctx.fillText('s=1', 4, this.toY(1, h) - 4);
    ctx.fillText('s=' + (1 + this.scaleRange()).toFixed(3), 4, 12);
    ctx.fillText('s=' + (1 - this.scaleRange()).toFixed(3), 4, h - 4);
    ctx.fillText('r=0', 2, this.toY(1, h) + 14);
    ctx.fillText('r=1', w - 28, this.toY(1, h) + 14);
    // sibling curves dimmed for comparison, active curve bright on top
    const activeKey = this.curveKey();
    for (const key of this.curveKeys()) {
      if (key === activeKey) continue;
      const curve = this.curveData(key, false);
      ctx.strokeStyle = 'rgba(95, 159, 223, 0.3)'; ctx.lineWidth = 1; ctx.beginPath();
      for (let i = 0; i <= 300; i++) {
        const r = i / 300;
        const y = this.toY(this.scaleAtCurve(curve, r, 1), h);
        if (i === 0) ctx.moveTo(this.toX(r, w), y); else ctx.lineTo(this.toX(r, w), y);
      }
      ctx.stroke();
    }
    const active = this.activeCurve(false);
    ctx.strokeStyle = '#5f9fdf'; ctx.lineWidth = 2; ctx.beginPath();
    for (let i = 0; i <= 300; i++) {
      const r = i / 300;
      const y = this.toY(this.scaleAtCurve(active, r, 1), h);
      if (i === 0) ctx.moveTo(this.toX(r, w), y); else ctx.lineTo(this.toX(r, w), y);
    }
    ctx.stroke(); ctx.lineWidth = 1;
    // control points in spline mode
    if (cfg.distortion.mode === 'spline') {
      for (let i = 0; i < active.points.length; i++) {
        const pt = active.points[i];
        ctx.fillStyle = i === this.dragIndex ? '#ffe9a0' : '#e8b64c';
        ctx.beginPath();
        ctx.arc(this.toX(pt.r, w), this.toY(pt.scale, h), 5, 0, Math.PI * 2);
        ctx.fill();
      }
    }
  }

  private drawPreview() {
    const canvas = this.previewCanvas().nativeElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    const cfg = this.settings();
    const ex = this.exaggeration();
    const cx = 0.5 + (cfg.centerOffsetXLeft || 0), cy = 0.5 + (cfg.centerOffsetY || 0);
    const appear = (qx: number, qy: number): [number, number] => {
      const px = qx - cx, py = qy - cy;
      const rs = Math.hypot(px, py);
      if (rs < 1e-6) return [qx, qy];
      // blend weight is constant along a ray, so the inversion stays 1d
      const wH = (px * px) / Math.max(px * px + py * py, 1e-12);
      let lo = 0, hi = 2.0;
      for (let i = 0; i < 40; i++) {
        const mid = (lo + hi) / 2;
        if (mid * this.directionalScale(mid, wH, ex) < rs) lo = mid; else hi = mid;
      }
      const r = (lo + hi) / 2;
      const f = r / rs;
      return [cx + px * f, cy + py * f];
    };
    ctx.strokeStyle = '#5faf5f';
    const lines = 12, steps = 48;
    for (let axis = 0; axis < 2; axis++) {
      for (let li = 0; li <= lines; li++) {
        ctx.beginPath();
        for (let si = 0; si <= steps; si++) {
          const a = li / lines, b = si / steps;
          const [nx, ny] = axis === 0 ? appear(a, b) : appear(b, a);
          if (si === 0) ctx.moveTo(nx * w, ny * h); else ctx.lineTo(nx * w, ny * h);
        }
        ctx.stroke();
      }
    }
    ctx.fillStyle = '#dd5';
    ctx.beginPath(); ctx.arc(cx * w, cy * h, 3, 0, Math.PI * 2); ctx.fill();
  }

  // ---- interaction ----
  private canvasPos(event: PointerEvent | MouseEvent): [number, number] {
    const canvas = this.curveCanvas().nativeElement;
    const rect = canvas.getBoundingClientRect();
    return [(event.clientX - rect.left) * canvas.width / rect.width, (event.clientY - rect.top) * canvas.height / rect.height];
  }
  private hitTest(x: number, y: number): number {
    const cfg = this.settings();
    if (cfg.distortion.mode !== 'spline') return -1;
    const canvas = this.curveCanvas().nativeElement;
    // grab radius of 10 css pixels, converted to canvas units so hit testing
    // feels the same when the window scales the canvas down or up
    const rect = canvas.getBoundingClientRect();
    const cssToCanvas = rect.width > 0 ? canvas.width / rect.width : 1;
    const radius = 10 * cssToCanvas;
    const points = this.activeCurve(false).points;
    for (let i = 0; i < points.length; i++) {
      const pt = points[i];
      const dx = this.toX(pt.r, canvas.width) - x, dy = this.toY(pt.scale, canvas.height) - y;
      if (dx * dx + dy * dy < radius * radius) return i;
    }
    return -1;
  }
  onPointerDown(event: PointerEvent) {
    const [x, y] = this.canvasPos(event);
    this.dragIndex = this.hitTest(x, y);
    if (this.dragIndex >= 0) {
      (event.target as HTMLElement).setPointerCapture(event.pointerId);
      event.preventDefault();
    }
  }
  onPointerMove(event: PointerEvent) {
    if (this.dragIndex < 0) return;
    const canvas = this.curveCanvas().nativeElement;
    const [x, y] = this.canvasPos(event);
    const pt = this.activeCurve().points[this.dragIndex];
    if (!pt) { this.dragIndex = -1; return; }
    pt.r = Math.round(this.fromX(x, canvas.width) * 1000) / 1000;
    pt.scale = Math.round(this.fromY(y, canvas.height) * 100000) / 100000;
    this.changed.emit();
    this.draw();
  }
  onPointerUp(event: PointerEvent) {
    if (this.dragIndex < 0) return;
    this.dragIndex = -1;
    this.activeCurve().points.sort((a, b) => a.r - b.r);
    this.changed.emit();
    this.draw();
    void event;
  }
  onDoubleClick(event: MouseEvent) {
    const cfg = this.settings();
    if (cfg.distortion.mode !== 'spline') return;
    const canvas = this.curveCanvas().nativeElement;
    const [x, y] = this.canvasPos(event);
    const points = this.activeCurve().points;
    points.push({
      r: Math.round(this.fromX(x, canvas.width) * 1000) / 1000,
      scale: Math.round(this.fromY(y, canvas.height) * 100000) / 100000
    });
    points.sort((a, b) => a.r - b.r);
    this.changed.emit();
    this.draw();
  }
  onContextMenu(event: MouseEvent) {
    event.preventDefault();
    const cfg = this.settings();
    if (cfg.distortion.mode !== 'spline') return;
    const [x, y] = this.canvasPos(event);
    const index = this.hitTest(x, y);
    if (index >= 0) {
      this.activeCurve().points.splice(index, 1);
      this.changed.emit();
      this.draw();
    }
  }

  convertK1K2ToSpline() {
    const cfg = this.settings();
    const curve = this.activeCurve();
    const radii = [0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9];
    curve.points = radii.map(r => ({
      r,
      scale: Math.round((1 + curve.k1 * r * r + curve.k2 * r * r * r * r) * 100000) / 100000
    }));
    cfg.distortion.mode = 'spline';
    this.changed.emit();
    this.draw();
  }
  resetCurve() {
    const cfg = this.settings();
    const curve = this.activeCurve();
    if (cfg.distortion.mode === 'spline') {
      curve.points = [{ r: 0, scale: 1 }, { r: 0.4, scale: 1 }, { r: 0.8, scale: 1 }];
    } else {
      curve.k1 = 0;
      curve.k2 = 0;
    }
    this.changed.emit();
    this.draw();
  }
  onUiChanged() {
    this.changed.emit();
    this.draw();
  }
}
