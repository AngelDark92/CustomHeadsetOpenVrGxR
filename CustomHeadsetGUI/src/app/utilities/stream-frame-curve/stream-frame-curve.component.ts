import { Component, ElementRef, effect, inject, input, output, signal, viewChild, AfterViewInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { MatButtonModule } from '@angular/material/button';
import { MatSelectModule } from '@angular/material/select';
import { StreamFrameConfig, StreamFrameDistortionPoint } from '../../services/JsonFileDefines';
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

  private dragIndex = -1;
  private viewReady = false;

  constructor() {
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

  // ---- shared curve math (annulus + exaggeration applied for the preview only) ----
  private scaleAt(r: number, exaggeration: number): number {
    const cfg = this.settings();
    let s: number;
    if (cfg.distortion.mode === 'spline') {
      const points = [...cfg.distortion.points].sort((a, b) => a.r - b.r);
      s = evaluateCurve(points, r);
    } else {
      s = 1 + (cfg.k1 || 0) * r * r + (cfg.k2 || 0) * r * r * r * r;
    }
    const an = cfg.distortion.annulus;
    if (an?.enable) {
      const f = an.feather ?? 0.05;
      const w = smoothstep(an.rMin - f, an.rMin + f, r) * (1 - smoothstep(an.rMax - f, an.rMax + f, r));
      s = 1 + (s - 1) * w;
    }
    return 1 + (s - 1) * exaggeration;
  }
  // a source point at radius rs appears at radius r solving r*s(r)=rs
  private invertRadial(rs: number, exaggeration: number): number {
    if (rs <= 0) return 0;
    let lo = 0, hi = 2.0;
    for (let i = 0; i < 40; i++) {
      const mid = (lo + hi) / 2;
      if (mid * this.scaleAt(mid, exaggeration) < rs) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2;
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
    const style = getComputedStyle(canvas);
    const fg = style.getPropertyValue('color') || '#ccc';

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
    // curve
    ctx.strokeStyle = '#5f9fdf'; ctx.lineWidth = 2; ctx.beginPath();
    for (let i = 0; i <= 300; i++) {
      const r = i / 300;
      const y = this.toY(this.scaleAt(r, 1), h);
      if (i === 0) ctx.moveTo(this.toX(r, w), y); else ctx.lineTo(this.toX(r, w), y);
    }
    ctx.stroke(); ctx.lineWidth = 1;
    // control points in spline mode
    if (cfg.distortion.mode === 'spline') {
      for (let i = 0; i < cfg.distortion.points.length; i++) {
        const pt = cfg.distortion.points[i];
        ctx.fillStyle = i === this.dragIndex ? '#ffe9a0' : '#e8b64c';
        ctx.beginPath();
        ctx.arc(this.toX(pt.r, w), this.toY(pt.scale, h), 5, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    void fg;
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
      const r = this.invertRadial(rs, ex);
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
    for (let i = 0; i < cfg.distortion.points.length; i++) {
      const pt = cfg.distortion.points[i];
      const dx = this.toX(pt.r, canvas.width) - x, dy = this.toY(pt.scale, canvas.height) - y;
      if (dx * dx + dy * dy < 100) return i;
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
    const cfg = this.settings();
    const canvas = this.curveCanvas().nativeElement;
    const [x, y] = this.canvasPos(event);
    const pt = cfg.distortion.points[this.dragIndex];
    if (!pt) { this.dragIndex = -1; return; }
    pt.r = Math.round(this.fromX(x, canvas.width) * 1000) / 1000;
    pt.scale = Math.round(this.fromY(y, canvas.height) * 100000) / 100000;
    this.changed.emit();
    this.draw();
  }
  onPointerUp(event: PointerEvent) {
    if (this.dragIndex < 0) return;
    this.dragIndex = -1;
    const cfg = this.settings();
    cfg.distortion.points.sort((a, b) => a.r - b.r);
    this.changed.emit();
    this.draw();
    void event;
  }
  onDoubleClick(event: MouseEvent) {
    const cfg = this.settings();
    if (cfg.distortion.mode !== 'spline') return;
    const canvas = this.curveCanvas().nativeElement;
    const [x, y] = this.canvasPos(event);
    cfg.distortion.points.push({
      r: Math.round(this.fromX(x, canvas.width) * 1000) / 1000,
      scale: Math.round(this.fromY(y, canvas.height) * 100000) / 100000
    });
    cfg.distortion.points.sort((a, b) => a.r - b.r);
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
      cfg.distortion.points.splice(index, 1);
      this.changed.emit();
      this.draw();
    }
  }

  convertK1K2ToSpline() {
    const cfg = this.settings();
    const radii = [0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9];
    cfg.distortion.points = radii.map(r => ({
      r,
      scale: Math.round((1 + cfg.k1 * r * r + cfg.k2 * r * r * r * r) * 100000) / 100000
    }));
    cfg.distortion.mode = 'spline';
    this.changed.emit();
    this.draw();
  }
  resetCurve() {
    const cfg = this.settings();
    if (cfg.distortion.mode === 'spline') {
      cfg.distortion.points = [{ r: 0, scale: 1 }, { r: 0.4, scale: 1 }, { r: 0.8, scale: 1 }];
    } else {
      cfg.k1 = 0;
      cfg.k2 = 0;
    }
    this.changed.emit();
    this.draw();
  }
  onUiChanged() {
    this.changed.emit();
    this.draw();
  }
}
