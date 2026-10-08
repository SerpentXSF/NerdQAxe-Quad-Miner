import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit, TemplateRef, ViewChild } from '@angular/core';
import { FormArray, FormBuilder, FormGroup, Validators } from '@angular/forms';
import { switchMap, startWith, tap, catchError, of } from 'rxjs';
import { LoadingService } from '../../services/loading.service';
import { SystemService } from '../../services/system.service';
import { eASICModel } from '../../models/enum/eASICModel';
import { NbToastrService, NbDialogService, NbDialogRef, NbSelectComponent } from '@nebular/theme';
import { LocalStorageService } from 'src/app/services/local-storage.service';
import { OtpAuthService, EnsureOtpResult, EnsureOtpOptions } from '../../services/otp-auth.service';
import { TranslateService } from '@ngx-translate/core';
import { ISettingsV2, ISettingsV2Fan } from '../../models/ISettingsV2';
import {
  MULTI_POOL_MIN_ASICS,
  PoolPreset,
  availablePresets,
  configFor,
  droppedPoolNames,
  presetFor,
  presetLabelKey,
} from './pool-preset';

enum SupportLevel { Safe = 0, Advanced = 1, Pro = 2 }

@Component({
  selector: 'app-edit',
  templateUrl: './edit.component.html',
  styleUrls: ['./edit.component.scss']
})
export class EditComponent implements OnInit {
  public supportLevel: SupportLevel = SupportLevel.Safe;

  public form!: FormGroup;

  public dialogRef!: NbDialogRef<any>; // Store reference

  public frequencyOptions: { name: string; value: number }[] = [];
  public voltageOptions: { name: string; value: number }[] = [];

  public firmwareUpdateProgress: number | null = null;
  public websiteUpdateProgress: number | null = null;

  public dontShowWarning: boolean = false;

  public eASICModel = eASICModel;
  public asicModel!: eASICModel;

  public defaultFrequency: number = 0;
  public defaultCoreVoltage: number = 0;
  public defaultVrFrequency: number = 0;
  public fanCount: number = 1;

  // Per-pool remembered coinbase-verify mode (index = pool) for the on/off toggle
  public lastCoinbaseVerifyMode: number[] = [];

  // Multi-pool support
  public maxPools: number = 4;
  /**
   * ASICs on this board, used to gate the Triple and Quad presets. Defaults to
   * the threshold so an older firmware that does not report it keeps offering
   * every preset - hiding one a board can use is worse than showing one it
   * cannot.
   */
  public asicCount: number = MULTI_POOL_MIN_ASICS;
  public showPoolPassword: boolean[] = [];

  // WiFi scan
  public apActive = false;
  public wifiScanning = false;
  public wifiScanResults: { ssid: string; rssi: number; authmode: number }[] = [];

  toggleCoinbaseVerify(enabled: boolean, poolIndex: number) {
    const ctrl = this.poolsArray.at(poolIndex).get('coinbaseVerifyMode');
    if (!ctrl) return;
    if (enabled) {
      ctrl.setValue(this.lastCoinbaseVerifyMode[poolIndex] || 1);
    } else {
      this.lastCoinbaseVerifyMode[poolIndex] = ctrl.value;
      ctrl.setValue(0);
    }
  }
  public fanLabels: string[] = ['Fan 1', 'Fan 2'];

  public ecoFrequency: number = 0;
  public ecoCoreVoltage: number = 0;

  private originalSettings!: any;
  // Number of pools the device actually holds. Seeded from the settings load and
  // refreshed after each successful save (originalSettings is not), so the
  // erase prompt in onPoolPresetChange() never under-counts what a shrink drops.
  // Deliberately separate from originalSettings.pools, which requiresReboot reads.
  private storedPoolCount?: number;

  public otpEnabled = false;
  public hasCanExtension = false;
  private pendingTotp: string | undefined;

  private asicFrequencyValues: number[] = [];
  private asicVoltageValues: number[] = [];

  private rebootRequiredFields = new Set<string>([
    'flipScreen',
    'invertScreen',
    'hostname',
    'ssid',
    'wifiPass',
    'invertFanPolarity',
    'stratumDifficulty',
    'stratumKeep',
    'canMaster',
    'poolMode',
  ]);

  @ViewChild('poolPresetSelect') poolPresetSelect?: NbSelectComponent;

  @Input() uri = '';

  constructor(
    private fb: FormBuilder,
    private systemService: SystemService,
    private toastrService: NbToastrService,
    private loadingService: LoadingService,
    private localStorageService: LocalStorageService,
    private dialogService: NbDialogService,
    private otpAuth: OtpAuthService,
    private translate: TranslateService,
  ) { }

  // ---- Multi-pool helpers ----

  get poolsArray(): FormArray {
    return this.form.get('pools') as FormArray;
  }

  /**
   * The preset matching the current form state. Derived, never stored - so the
   * Add/Remove Pool buttons update the dropdown for free and there is no
   * two-way sync to fall out of step.
   */
  get currentPoolPreset(): PoolPreset {
    const mode = this.form?.get('poolMode')?.value ?? 0;
    const count = this.poolsArray?.length ?? 1;
    return presetFor(mode, count);
  }

  /**
   * Presets this board can offer. Triple and Quad are held back on single-ASIC
   * hardware - see availablePresets().
   *
   * asicCount falls back to MULTI_POOL_MIN_ASICS when the device does not report
   * it, so an older firmware paired with this UI keeps showing every preset
   * rather than silently hiding ones its board can actually use.
   */
  get poolPresetOptions(): PoolPreset[] {
    return availablePresets(this.asicCount, this.currentPoolPreset);
  }

  /** Translation key for a preset's dropdown label. */
  public poolPresetLabel(preset: PoolPreset): string {
    return presetLabelKey(preset);
  }

  /**
   * Apply a preset: set poolMode and grow or shrink the visible pool slots.
   *
   * Shrinking below what the device has stored clears those slots on save
   * (Config::applyPoolsJson clears every slot the submitted array does not
   * cover), and their passwords cannot be recovered because the settings API
   * never returns them - so confirm first, naming the pools.
   *
   * On cancel nothing in the form is touched, but the control's display must
   * be pushed back explicitly: Nebular moves its own selection the moment an
   * option is clicked, and `[selected]` only re-pushes when the bound value
   * changes. The derived getter is unchanged on cancel, so Angular sees no
   * change and would leave the dropdown showing a preset the form does not
   * have.
   */
  public onPoolPresetChange(preset: PoolPreset): void {
    const cfg = configFor(preset);
    // The dropdown always offers all five presets, but maxPools is supplied by
    // the device. addPool() silently refuses to grow past maxPools, so a target
    // above it would make the grow loop below spin forever (and freeze the tab).
    // Clamp the target - do not remove this as "redundant" - and use it for the
    // prompt too, so it describes what will actually be erased.
    const targetCount = Math.min(cfg.poolCount, this.maxPools);
    const storedCount = this.storedPoolCount ?? this.poolsArray.length;

    const dropped = droppedPoolNames(targetCount, storedCount);
    if (dropped.length > 0) {
      const names = dropped.length === 1
        ? dropped[0]
        : `${dropped.slice(0, -1).join(', ')} and ${dropped[dropped.length - 1]}`;
      const ok = confirm(
        `This will erase ${names}, including ${dropped.length === 1 ? 'its password' : 'their passwords'}, ` +
        `which cannot be recovered.\n\nContinue?`
      );
      if (!ok) {
        this.revertPoolPresetDisplay();
        return;
      }
    }

    this.form.get('poolMode')?.setValue(cfg.poolMode);
    while (this.poolsArray.length > targetCount) {
      this.removePool(this.poolsArray.length - 1);
    }
    while (this.poolsArray.length < targetCount) {
      this.addPool();
    }
  }

  /**
   * Push the derived preset back into the Nebular control.
   *
   * Needed only on cancel. Nebular has already moved its own displayed
   * selection to the clicked option, and since the derived getter did not
   * change, Angular will not re-evaluate the [selected] binding. Without this
   * the dropdown would claim a preset the form does not hold.
   */
  private revertPoolPresetDisplay(): void {
    this.poolPresetSelect?.writeValue(this.currentPoolPreset);
  }

  private createPoolGroup(p: any = {}, index: number = 0): FormGroup {
    const primary = index === 0;
    const urlValidators = [
      Validators.pattern(/^(?!.*stratum\+tcp:\/\/).*$/),
      Validators.pattern(/^[^:]*$/),
    ];
    const portValidators = [
      Validators.pattern(/^[^:]*$/),
      Validators.min(0),
      Validators.max(65353),
    ];
    return this.fb.group({
      url: [p.url ?? '', primary ? [Validators.required, ...urlValidators] : urlValidators],
      port: [p.port ?? 3333, primary ? [Validators.required, ...portValidators] : portValidators],
      user: [p.user ?? '', primary ? [Validators.required] : []],
      password: ['*****'],
      enonceSubscribe: [(p.enonceSubscribe ?? 0) == 1],
      tls: [(p.tls ?? 0) == 1],
      protocol: [p.protocol ?? 0],                       // 0 = V1, 1 = V2
      sv2AuthorityPubkey: [p.sv2AuthorityPubkey ?? ''],
      sv2ChannelType: [p.sv2ChannelType ?? 0],           // 0 = Extended, 1 = Standard
      coinbaseVerifyMode: [p.coinbaseVerifyMode ?? 0],
      coinbaseMaxFee: [p.coinbaseMaxFee ?? 3.0],
      coinbaseVerifyForce: [p.coinbaseVerifyForce ?? false],
      weight: [p.weight ?? 25, [Validators.min(0), Validators.max(100)]],
    });
  }

  public addPool(): void {
    if (this.poolsArray.length >= this.maxPools) return;
    this.poolsArray.push(this.createPoolGroup({}, this.poolsArray.length));
    this.showPoolPassword.push(false);
    this.lastCoinbaseVerifyMode.push(1);
  }

  /**
   * Trash-button entry point: confirm before removing a pool.
   *
   * Separate from removePool() because onPoolPresetChange() already prompts once
   * for the whole downshift and then calls removePool() in a loop - confirming
   * inside removePool() would ask again for every slot it drops.
   *
   * Two things are lost on save and both are worth spelling out. The pool's own
   * settings and password go, and the settings API never returns a password, so
   * that is final. And because a saved password stays with its NVS slot while
   * everything else shifts up, the pools below this one would otherwise point at
   * the previous occupant's password until they are re-entered.
   */
  public removePoolWithConfirm(i: number): void {
    if (this.poolsArray.length <= 1) return;

    const below = this.poolsArray.length - (i + 1);
    let msg = `Remove Pool ${i + 1}? Its settings and password are erased when you save, `
            + `and the password cannot be recovered.`;
    if (below > 0) {
      const names = Array.from({ length: below }, (_, k) => `Pool ${i + 2 + k}`);
      const joined = names.length === 1
        ? names[0]
        : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
      msg += `\n\n${joined} will move up, but a saved password stays with its slot, `
           + `so you will need to re-enter the password for ${names.length === 1 ? 'it' : 'each of them'}.`;
    }
    msg += `\n\nContinue?`;

    if (!confirm(msg)) return;
    this.removePool(i);
  }

  public removePool(i: number): void {
    // Allow down to a single pool (mine to just one). Failover mode still needs
    // a fallback, but the firmware keeps a harmless empty second slot for that.
    if (this.poolsArray.length <= 1) return;
    this.poolsArray.removeAt(i);
    this.showPoolPassword.splice(i, 1);
    this.lastCoinbaseVerifyMode.splice(i, 1);
  }

  public movePool(i: number, dir: -1 | 1): void {
    const j = i + dir;
    if (j < 0 || j >= this.poolsArray.length) return;
    const group = this.poolsArray.at(i);
    this.poolsArray.removeAt(i);
    this.poolsArray.insert(j, group);
    const swap = (arr: any[]) => { const t = arr[i]; arr[i] = arr[j]; arr[j] = t; };
    swap(this.showPoolPassword);
    swap(this.lastCoinbaseVerifyMode);
  }

  public canAddPool(): boolean {
    return this.poolsArray && this.poolsArray.length < this.maxPools;
  }

  public togglePoolPassword(i: number): void {
    this.showPoolPassword[i] = !this.showPoolPassword[i];
  }

  ngOnInit(): void {
    this.systemService.getSettingsV2(this.uri)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe((info: ISettingsV2) => {
        this.originalSettings = structuredClone(info);
        this.storedPoolCount = info.pools?.length;

        this.originalSettings["poolMode"] = info.poolMode ?? 0;
        this.originalSettings["canMaster"] = info.can?.enabled ? 1 : 0;

        this.maxPools = info.maxPools ?? 4;
        this.asicCount = info.asicCount ?? MULTI_POOL_MIN_ASICS;
        const poolsInfo = (info.pools && info.pools.length) ? info.pools : [{} as any, {} as any];
        this.showPoolPassword = poolsInfo.map(() => false);
        this.lastCoinbaseVerifyMode = poolsInfo.map(p => p.coinbaseVerifyMode || 1);

        this.otpEnabled = !!info.otp;
        this.apActive = !!info.apActive;
        this.hasCanExtension = !!info.can.hasExtension;

        this.asicModel = info.asicModel;

        this.defaultFrequency = info.defaultFrequency ?? 0;
        this.defaultCoreVoltage = info.defaultCoreVoltage ?? 0;

        this.ecoFrequency = info.ecoFrequency ?? undefined;
        this.ecoCoreVoltage = info.ecoCoreVoltage ?? undefined;

        this.asicFrequencyValues = info.frequencyOptions ?? [];
        this.asicVoltageValues = info.voltageOptions ?? [];

        this.defaultVrFrequency = info.defaultVrFrequency ?? undefined;

        this.fanCount = info.fans?.length ?? 1;
        this.fanLabels = info.fans?.map((f: ISettingsV2Fan, i: number) => f.label || `Fan ${i + 1}`) ?? ['Fan 1', 'Fan 2'];
        const fan1cfg = info.fans?.[1];

        const freqBase = this.asicFrequencyValues.map(v => {
          let suffix = '';
          if (v === this.defaultFrequency) suffix = ' (default)';
          if (this.ecoFrequency != null && v === this.ecoFrequency) suffix = ' (eco)';
          return { name: `${v}${suffix}`, value: v };
        });

        const voltBase = this.asicVoltageValues.map(v => {
          let suffix = '';
          if (v === this.defaultCoreVoltage) suffix = ' (default)';
          if (this.ecoCoreVoltage != null && v === this.ecoCoreVoltage) suffix = ' (eco)';
          return { name: `${v}${suffix}`, value: v };
        });

        // Build dropdowns and, if needed, append the current custom value
        this.frequencyOptions = this.assembleDropdownOptions(freqBase, info.frequency);
        this.voltageOptions = this.assembleDropdownOptions(voltBase, info.coreVoltage);

        // Build the form (Min/Max for volt/freq will be set dynamically right after)
        this.form = this.fb.group({
          stratumKeep: [info.stratumKeep == 1],
          canMaster: [info.can.enabled == true],
          flipScreen: [info.flipScreen == 1],
          invertScreen: [info.invertScreen == 1],
          autoScreenOff: [info.autoScreenOff == 1],
          customMempoolEnabled: [!!info.mempoolCustom],
          mempoolUrl: [info.mempoolUrl || 'https://mempool.space'],
          timeFormat: [this.localStorageService.getItem('timeFormat') || '24h'],
          pools: this.fb.array(poolsInfo.map((p, i) => this.createPoolGroup(p, i))),

          hostname: [info.hostname, [Validators.required]],
          ssid: [info.ssid, [Validators.required]],
          wifiPass: ['*****'],

          coreVoltage: [info.coreVoltage, [Validators.min(info.absMinCoreVoltage || 1005), Validators.max(info.absMaxCoreVoltage || 1400), Validators.required]],
          frequency: [info.frequency, [Validators.required]],
          jobInterval: [info.jobInterval, [Validators.required]],
          stratumDifficulty: [info.stratumDifficulty, [Validators.required, Validators.min(1)]],

          poolMode: [info.poolMode ?? 0, [Validators.required]],                   // 0 = Failover, 1 = Dual
          poolBalance: [info.poolBalance ?? 50, [                                   // Anteil PRIMARY in %
            Validators.required,
            Validators.min(0),
            Validators.max(100),
          ]],

          autofanspeed: [info.fans[0]?.mode ?? 0, [Validators.required]],
          pidTargetTemp: [info.fans[0]?.pid?.targetTemp ?? 55, [
            Validators.min(30),
            Validators.max(80),
            Validators.required
          ]],
          pidP: [info.fans[0]?.pid?.p ?? 6, [
            Validators.min(0),
            Validators.max(100),
            Validators.required
          ]],
          pidI: [info.fans[0]?.pid?.i ?? 0.1, [
            Validators.min(0),
            Validators.max(10),
            Validators.required
          ]],
          pidD: [info.fans[0]?.pid?.d ?? 10, [
            Validators.min(0),
            Validators.max(100),
            Validators.required
          ]],
          invertFanPolarity: [info.invertFanPolarity == 1, [Validators.required]],
          pidUseMax: [info.pidUseMax ?? true],
          manualFanSpeed: [info.fans[0]?.manualSpeed ?? 100, [Validators.required]],
          overheat_temp: [info.fans[0]?.overheatTemp ?? 70, [
            Validators.min(40),
            Validators.max(90),
            Validators.required
          ]],
          vrFrequency: [info.vrFrequency, [
            Validators.min(1000),
            Validators.max(100000),
            Validators.pattern(/^\d+$/),   // only ints
            Validators.required,
          ]],
          otpEnabled: [info.otp],

          fan1Mode: [fan1cfg?.mode ?? 3, [Validators.required]],
          fan1ManualSpeed: [fan1cfg?.manualSpeed ?? 100, [Validators.min(0), Validators.max(100), Validators.required]],
          fan1OverheatTemp: [fan1cfg?.overheatTemp ?? 70, [Validators.min(40), Validators.max(90), Validators.required]],
          fan1PidTargetTemp: [fan1cfg?.pid?.targetTemp ?? 65, [Validators.min(30), Validators.max(80), Validators.required]],
          fan1PidP: [fan1cfg?.pid?.p ?? 6, [Validators.min(0), Validators.max(100), Validators.required]],
          fan1PidI: [fan1cfg?.pid?.i ?? 0.1, [Validators.min(0), Validators.max(10), Validators.required]],
          fan1PidD: [fan1cfg?.pid?.d ?? 10, [Validators.min(0), Validators.max(100), Validators.required]],
        });

        this.form.controls['autofanspeed'].valueChanges
          .pipe(startWith(this.form.controls['autofanspeed'].value))
          .subscribe(() => this.updatePIDFieldStates());

        this.form.controls['fan1Mode'].valueChanges
          .pipe(startWith(this.form.controls['fan1Mode'].value))
          .subscribe(() => this.updateFan1FieldStates());

        this.updatePIDFieldStates();
        this.updateFan1FieldStates();

      });
  }

  private updatePIDFieldStates(): void {
    const mode = this.form.controls['autofanspeed'].value;
    const enable = (ctrl: string) => this.form.controls[ctrl]?.enable({ emitEvent: false });
    const disable = (ctrl: string) => this.form.controls[ctrl]?.disable({ emitEvent: false });

    if (mode === 0) {
      enable('manualFanSpeed');
      disable('pidTargetTemp');
      disable('pidP');
      disable('pidI');
      disable('pidD');
    } else if (mode === 1) {
      disable('manualFanSpeed');
      disable('pidTargetTemp');
      disable('pidP');
      disable('pidI');
      disable('pidD');
    } else if (mode === 2) {
      disable('manualFanSpeed');
      enable('pidTargetTemp');
      if (this.supportLevel >= 1) {
        enable('pidP');
        enable('pidI');
        enable('pidD');
      } else {
        disable('pidP');
        disable('pidI');
        disable('pidD');
      }
    }
  }

  private updateFan1FieldStates(): void {
    const mode = this.form.controls['fan1Mode'].value;
    const enable = (ctrl: string) => this.form.controls[ctrl]?.enable({ emitEvent: false });
    const disable = (ctrl: string) => this.form.controls[ctrl]?.disable({ emitEvent: false });

    if (mode === 3) {
      // LINKED — disable fan1-controls; overheatTemp stays enabled (VReg shutdown threshold)
      enable('fan1OverheatTemp');
      disable('fan1ManualSpeed');
      disable('fan1PidTargetTemp');
      disable('fan1PidP');
      disable('fan1PidI');
      disable('fan1PidD');
    } else if (mode === 0) {
      // MANUAL
      enable('fan1ManualSpeed');
      enable('fan1OverheatTemp');
      disable('fan1PidTargetTemp');
      disable('fan1PidP');
      disable('fan1PidI');
      disable('fan1PidD');
    } else if (mode === 2) {
      // PID
      disable('fan1ManualSpeed');
      enable('fan1OverheatTemp');
      enable('fan1PidTargetTemp');
      if (this.supportLevel >= 1) {
        enable('fan1PidP');
        enable('fan1PidI');
        enable('fan1PidD');
      } else {
        disable('fan1PidP');
        disable('fan1PidI');
        disable('fan1PidD');
      }
    }
  }

  public updateSystem(totp?: string) {
    const f = this.form.getRawValue();

    // Client-only preference
    if (f.timeFormat) {
      this.localStorageService.setItem('timeFormat', f.timeFormat);
      window.dispatchEvent(new CustomEvent('timeFormatChanged', { detail: f.timeFormat }));
    }

    // Build pools[] array matching GET /api/v2/settings structure
    const pools: any[] = (f.pools ?? []).map((p: any) => {
      const pool: any = {
        url: p.url,
        port: p.port,
        user: p.user,
        enonceSubscribe: !!p.enonceSubscribe,
        tls: !!p.tls,
        protocol: p.protocol,
        sv2AuthorityPubkey: p.sv2AuthorityPubkey,
        sv2ChannelType: p.sv2ChannelType,
        coinbaseVerifyMode: p.coinbaseVerifyMode,
        coinbaseMaxFee: p.coinbaseMaxFee,
        coinbaseVerifyForce: !!p.coinbaseVerifyForce,
        weight: p.weight,
      };
      if (p.password !== '*****') pool.password = p.password;
      return pool;
    });

    // Build fans[] array
    const fans: any[] = [
      {
        mode: f.autofanspeed,
        manualSpeed: f.manualFanSpeed,
        overheatTemp: f.overheat_temp,
        pid: { targetTemp: f.pidTargetTemp, p: f.pidP, i: f.pidI, d: f.pidD }
      }
    ];
    if (this.fanCount > 1) {
      fans.push({
        mode: f.fan1Mode,
        manualSpeed: f.fan1ManualSpeed,
        overheatTemp: f.fan1OverheatTemp,
        pid: { targetTemp: f.fan1PidTargetTemp, p: f.fan1PidP, i: f.fan1PidI, d: f.fan1PidD }
      });
    }

    // Build v2 payload
    const payload: any = {
      // Network
      hostname: f.hostname,
      ssid: f.ssid,
      // ASIC
      frequency: f.frequency,
      coreVoltage: f.coreVoltage,
      vrFrequency: f.vrFrequency,
      jobInterval: f.jobInterval,
      stratumDifficulty: f.stratumDifficulty,
      // Stratum
      poolMode: f.poolMode,
      poolBalance: f.poolBalance,
      stratumKeep: f.stratumKeep ? 1 : 0,
      pools,
      // Fans
      fans,
      invertFanPolarity: !!f.invertFanPolarity,
      pidUseMax: !!f.pidUseMax,
      // Mempool
      mempoolCustom: !!f.customMempoolEnabled,
      mempoolUrl: f.customMempoolEnabled ? f.mempoolUrl : '',
      // Display
      flipScreen: !!f.flipScreen,
      invertScreen: !!f.invertScreen,
      autoScreenOff: !!f.autoScreenOff,
      // CAN
      canMaster: !!f.canMaster,
    };

    // WiFi password — allow empty, strip masked
    const wifiPass = f.wifiPass == null ? '' : f.wifiPass;
    if (wifiPass !== '*****') payload.wifiPass = wifiPass;

    return this.systemService.updateSettingsV2(this.uri, payload, totp).pipe(
      // The device now holds exactly the pools we submitted. No re-fetch: a save
      // is often followed by a reboot and the request could race it.
      tap(() => { this.storedPoolCount = pools.length; })
    );
  }

  get requiresReboot(): boolean {
    if (!this.form || !this.originalSettings) return false;

    // Adding/removing a pool changes how many stratum tasks the firmware spawns,
    // which is decided at boot -> needs a restart. A per-pool protocol (SV1/SV2)
    // change also needs one.
    const origPools = this.originalSettings.pools ?? [];
    if (this.poolsArray && this.poolsArray.length !== origPools.length) {
      return true;
    }
    if (this.poolsArray) {
      for (let i = 0; i < this.poolsArray.length; i++) {
        const cur = this.poolsArray.at(i).get('protocol')?.value ?? 0;
        const orig = origPools[i]?.protocol ?? 0;
        if (cur !== orig) return true;
      }
    }

    const current = this.form.getRawValue();

    for (const key of this.rebootRequiredFields) {
      if (!(key in current)) {
        continue;
      }

      const currentValue = this.normalizeValue(current[key]);
      const originalValue = this.normalizeValue(this.originalSettings[key]);

      // Masked password fields: unchanged if still '*****', changed otherwise
      if (typeof currentValue === 'string' && currentValue === '*****') {
        continue;
      }
      // Fields not present in original settings (e.g. wifiPass):
      // if we got past the '*****' check, the user has typed something new
      if (originalValue === undefined || originalValue === null) {
        return true;
      }

      if (currentValue !== originalValue) {
        //console.log(`Mismatch on key: ${key}`, currentValue, originalValue);
        return true;
      }
    }

    return false;
  }

  private normalizeValue(value: any): any {
    if (typeof value === 'boolean') {
      return value ? 1 : 0;
    }
    return value;
  }

  showWifiPassword: boolean = false;
  toggleWifiPasswordVisibility() {
    this.showWifiPassword = !this.showWifiPassword;
  }

  public setDevToolsOpen(supportLevel: number) {
    this.supportLevel = supportLevel;
    console.log('Advanced Mode:', supportLevel);

    const freqBase = this.asicFrequencyValues.map(v => {
      let suffix = '';
      if (v === this.defaultFrequency) suffix = ' (default)';
      if (this.ecoFrequency != null && v === this.ecoFrequency) suffix = ' (eco)';
      return { name: `${v}${suffix}`, value: v };
    });

    const voltBase = this.asicVoltageValues.map(v => {
      let suffix = '';
      if (v === this.defaultCoreVoltage) suffix = ' (default)';
      if (this.ecoCoreVoltage != null && v === this.ecoCoreVoltage) suffix = ' (eco)';
      return { name: `${v}${suffix}`, value: v };
    });

    this.frequencyOptions = this.assembleDropdownOptions(freqBase, this.form.controls['frequency'].value);
    this.voltageOptions = this.assembleDropdownOptions(voltBase, this.form.controls['coreVoltage'].value);

    this.updatePIDFieldStates();
    this.updateFan1FieldStates();
  }

  public isVoltageTooHigh(): boolean {
    if (!this.asicVoltageValues.length) return false;
    const maxVoltage = Math.max(...this.asicVoltageValues);
    return this.form?.controls['coreVoltage'].value > maxVoltage;
  }

  public isFrequencyTooHigh(): boolean {
    if (!this.asicFrequencyValues.length) return false;
    const maxFrequency = Math.max(...this.asicFrequencyValues);
    return this.form?.controls['frequency'].value > maxFrequency;
  }

  public checkVoltageLimit(): void {
    this.form.controls['coreVoltage'].updateValueAndValidity({ emitEvent: false });
  }

  public checkFrequencyLimit(): void {
    this.form.controls['frequency'].updateValueAndValidity({ emitEvent: false });
  }

  /**
   * Dynamically assemble dropdown options, including custom values.
   * @param predefined The predefined options.
   * @param currentValue The current value to include as a custom option if needed.
   */
  private assembleDropdownOptions(predefined: { name: string, value: number }[], currentValue: number): { name: string, value: number }[] {
    const options = [...predefined];
    if (!options.some(option => option.value === currentValue)) {
      options.push({
        name: `${currentValue} (custom)`,
        value: currentValue
      });
    }
    return options;
  }

  public restart() {
    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT'),
      { disableOtp: true },
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          this.systemService.restart("", totp).pipe(
            // drop session on reboot
            tap(() => this.otpAuth.clearSession()),
            this.loadingService.lockUIUntilComplete()
          )
        ),
        catchError((err: HttpErrorResponse) => {
          console.log(err);
          this.toastrService.danger(this.translate.instant('SYSTEM.RESTART_FAILED'), this.translate.instant('COMMON.ERROR'));
          return of(null);
        })
      )
      .subscribe(res => {
        if (res !== null) {
          this.toastrService.success(this.translate.instant('SYSTEM.RESTART_SUCCESS'), this.translate.instant('COMMON.SUCCESS'));
        }
      });
  }

  // Function to check if settings are unsafe
  public hasUnsafeSettings(): boolean {
    return this.isVoltageTooHigh() || this.isFrequencyTooHigh();
  }

  // Open warning modal unless user disabled it
  public confirmSave(dialog: TemplateRef<any>): void {
    if (!this.localStorageService.getBool('hideUnsafeSettingsWarning') && this.hasUnsafeSettings()) {
      this.dialogRef = this.dialogService.open(dialog, { closeOnBackdropClick: false });
    } else {
      this.runSaveWithOptionalOtp();
    }
  }

  // Save preference and close modal
  public saveAfterWarning(): void {
    if (this.dontShowWarning) {
      this.localStorageService.setBool('hideUnsafeSettingsWarning', true);
    }
    this.dialogRef.close();
    this.runSaveWithOptionalOtp();
  }

  get wrapAroundTime(): number {
    const freq = this.form.get('vrFrequency')?.value;
    if (!freq || freq <= 0) {
      return 0;
    }
    const wrap = 65536 / freq; // seconds
    return wrap;
  }

  private runSaveWithOptionalOtp(): void {
    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT')
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          this.updateSystem(totp).pipe(this.loadingService.lockUIUntilComplete())
        ),
      )
      .subscribe({
        next: () => {
          this.toastrService.success('Success!', 'Saved.');
        },
        error: (err: HttpErrorResponse) => {
          this.toastrService.danger('Error.', `Could not save. ${err.message}`);
        }
      });
  }

  public poolTabHeader(i: number) {
    const proto = this.poolsArray?.at(i)?.get('protocol')?.value;
    const protoLabel = proto === 1 ? ' (SV2)' : ' (SV1)';

    // In failover mode with two pools, label them primary / fallback.
    if (this.form?.get("poolMode")?.value == 0 && this.poolsArray?.length === 2) {
      if (i == 0) {
        return this.translate.instant('SETTINGS.PRIMARY_STRATUM_POOL') + protoLabel;
      }
      return this.translate.instant('SETTINGS.FALLBACK_STRATUM_POOL') + protoLabel;
    }
    return `Pool ${i + 1}` + protoLabel;
  }

  public scanWifi(dialog: TemplateRef<any>): void {
    if (this.wifiScanning) return;
    this.wifiScanning = true;
    this.systemService.scanWifi().subscribe({
      next: (response) => {
        const byStrength: { [ssid: string]: { ssid: string; rssi: number; authmode: number } } = {};
        for (const n of response.networks || []) {
          if (!n.ssid) continue;
          if (!byStrength[n.ssid] || n.rssi > byStrength[n.ssid].rssi) {
            byStrength[n.ssid] = n;
          }
        }
        this.wifiScanResults = Object.values(byStrength).sort((a, b) => b.rssi - a.rssi);
        this.wifiScanning = false;
        this.dialogRef = this.dialogService.open(dialog, { closeOnBackdropClick: true });
      },
      error: () => {
        this.wifiScanning = false;
        this.toastrService.danger(
          this.translate.instant('SETTINGS.WIFI_SCAN_FAILED'),
          this.translate.instant('COMMON.ERROR')
        );
      }
    });
  }

  public selectWifiNetwork(ssid: string): void {
    this.form.patchValue({ ssid });
    this.form.markAsDirty();
    if (this.dialogRef) {
      this.dialogRef.close();
    }
  }

  public wifiSignalStrength(rssi: number): string {
    if (rssi >= -50) return 'excellent';
    if (rssi >= -60) return 'good';
    if (rssi >= -70) return 'fair';
    return 'weak';
  }

}
