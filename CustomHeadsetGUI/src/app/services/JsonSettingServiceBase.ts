
import { effect, inject, Signal, signal } from '@angular/core';
import { exists, mkdir, readTextFile, watchImmediate, writeTextFile } from '@tauri-apps/plugin-fs';
import { cleanJsonComments, DebouncedFileWriter, debouncedFileWriter, deepCopy, deepMerge } from '../helpers';
import { debounceTime, delay, filter, Subject } from 'rxjs';
import { AppSettingAccessor } from './AppSettingAccessor';
export enum FileReadErrorReason {
    NotExists = 'File not exists',
    ParsingFailed = 'Parsing failed'
}
export type FileReadError = {
    reason: FileReadErrorReason,
    message?: string
}

function applyJsonChanges(baseline: any, current: any, desired: any): any {
    if (JSON.stringify(baseline) === JSON.stringify(desired)) return current;
    const baselineObject = baseline !== null && typeof baseline === 'object' && !Array.isArray(baseline);
    const currentObject = current !== null && typeof current === 'object' && !Array.isArray(current);
    const desiredObject = desired !== null && typeof desired === 'object' && !Array.isArray(desired);
    if (!baselineObject || !currentObject || !desiredObject) return desired;

    const result: Record<string, any> = { ...current };
    const keys = new Set([...Object.keys(baseline), ...Object.keys(desired)]);
    for (const key of keys) {
        const baselineHas = Object.prototype.hasOwnProperty.call(baseline, key);
        const desiredHas = Object.prototype.hasOwnProperty.call(desired, key);
        if (!desiredHas && baselineHas) {
            delete result[key];
        } else if (desiredHas && !baselineHas) {
            result[key] = desired[key];
        } else if (desiredHas) {
            result[key] = applyJsonChanges(baseline[key], current[key], desired[key]);
        }
    }
    return result;
}

export abstract class JsonSettingServiceBase<T> {
    protected _values = signal<T | undefined>(undefined, {});
    // migration hook: subclasses may strip retired keys from freshly
    // loaded values so the next natural save writes a clean file
    protected migrateLoadedValues(values: T): T { return values; }
    public values = this._values.asReadonly();
    protected readonly debouncedFileWriter: DebouncedFileWriter;
    protected _initTask: Promise<void>;
    public get initTask() {
        return this._initTask;
    }
    protected _readFileError = signal<FileReadError | undefined>(undefined);
    public readFileError = this._readFileError.asReadonly();
    protected defaults?: T;
    public get filePath() {
        return this._filePath;
    }
    constructor(
        private _filePath: string,
        private _fileDir: string,
        private defaultValue: Signal<T | undefined>,
        private autoCreate: boolean,
        private watchFileforAutoReload: boolean) {
        const appSettings = inject(AppSettingAccessor)
        this.debouncedFileWriter = debouncedFileWriter(_filePath, _fileDir, () => appSettings.settings.updateMode == 'rewrite');
        this._initTask = this.init();

        effect(() => {
            this.defaults = (this.defaultValue() ?? {} as any)
            this.loadSetting()
        });
    }
    protected async init() {
        if (!await exists(this._fileDir)) {
            await mkdir(this._fileDir);
        }
        if (!await exists(this._filePath) && this.autoCreate) {
            await writeTextFile(this._filePath, JSON.stringify("{}"));
        }
        if (this.watchFileforAutoReload) {
            const watchSubject = new Subject<void>();
            watchSubject.pipe(filter(() => !this.debouncedFileWriter.isSavingFile()), debounceTime(50), delay(50)).subscribe(() => {
                this.loadSetting()
            });
            watchImmediate(this._fileDir, ev => {
                if (ev.paths.some(p => p == this._filePath)) {
                    watchSubject.next();
                }
            });
            watchSubject.next();
        } else {
            await this.loadSetting();
        }

    }
    async loadSetting(): Promise<boolean> {
        return await navigator.locks.request(`loadfile_${this._filePath}`, async () => {
            try {
                if (await exists(this._filePath)) {
                    try {
                        const copiedDefaults = deepCopy(this.defaults);
                        this._values.set(this.migrateLoadedValues(deepMerge(copiedDefaults as any, JSON.parse(cleanJsonComments(await readTextFile(this._filePath))))));
                        this._readFileError.set(undefined);
                        return true;
                    } catch (e) {
                        console.error('parsing setting error', this._filePath, e)
                        this._readFileError.set({
                            reason: FileReadErrorReason.ParsingFailed,
                            message: `${e}`
                        });
                        this._values.set(undefined);
                    }
                } else {
                    this._readFileError.set({ reason: FileReadErrorReason.NotExists });
                    this._values.set(undefined)
                }
            } catch (e) {
                console.error('load setting error', this._filePath, e);
            }
            return false
        });
    }

    async save(values: T) {
        await this._initTask;
        this.debouncedFileWriter.save(JSON.stringify(deepCopy(values, this.defaults ?? {}), undefined, 4));
        this._values.set(Object.assign({}, values))
    }

    async runExclusiveFileMutation<TResult>(operation: () => Promise<TResult>, protectedTopLevelKeys: string[] = []): Promise<TResult> {
        await this._initTask;
        return await this.debouncedFileWriter.runExclusive(
            operation,
            (baselineContent, currentContent, bufferedContent) => {
                const baseline = JSON.parse(cleanJsonComments(baselineContent));
                const current = JSON.parse(cleanJsonComments(currentContent));
                const buffered = JSON.parse(cleanJsonComments(bufferedContent));
                const rebased = applyJsonChanges(baseline, current, buffered);
                for (const key of protectedTopLevelKeys) {
                    if (Object.prototype.hasOwnProperty.call(current, key)) rebased[key] = current[key];
                    else delete rebased[key];
                }
                return JSON.stringify(rebased, undefined, 4);
            },
            async () => {
                // Refresh signals before releasing the writer lock so a later
                // save starts from the authoritative, rebased state.
                await this.loadSetting();
            }
        );
    }
}
