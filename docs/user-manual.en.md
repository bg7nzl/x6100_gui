# X6100 User Manual (BG7NZL Custom Firmware)

> This document covers day-to-day operation only — no technical deep dive. Follow the steps as written.
> It applies to BG7NZL sideload firmware built on gdyuldin upstream (rewritten FT8, plus browser Remote / Logbook / OTA, and more). If you are still on stock firmware, or you did not boot from this TF/SD card, most of the features below will not be present.
> Read the [Disclaimer](DISCLAIMER.md) before use.

Developer notes live in other docs in this folder; normal operation does not require them.

---

## How to flash this firmware (sideload)

This firmware runs from a TF/SD card sideload: stock firmware remains on the radio’s internal eMMC; this system lives on the external card. Remove the card and power on normally, and you generally return to stock — the stock image on the radio is not overwritten.

### Preparation

1. Confirm the baseband (BASE) version is 1.1.9 (System Info → BASE). If it is below 1.1.9, upgrade the baseband first, then write the SD card image.
2. Prepare an SD card (capacity per the maintainer’s `sdcard.img`; Class 10 or better recommended).
3. Obtain `sdcard.img` from the maintainer.

### Writing the image to the SD card

Windows:

- Use [Rufus](https://rufus.ie/) or a similar tool.
- Select the SD card and `sdcard.img`, and write it as a DD image. Do not merely copy a single file onto the card.

Linux / macOS:

- Confirm the device name first (e.g. `/dev/sdb`, `/dev/disk4`). Writing to the wrong disk will wipe an entire hard drive.
- Example:

```bash
sudo dd if=sdcard.img of=/dev/disk4 bs=1m conv=sync
```

Eject safely when finished.

### Booting and using

1. Power the radio completely off.
2. Insert the written SD card.
3. Power on; the radio boots this firmware from the SD card.

Do not remove the card while in use — hot-plug may freeze the radio or corrupt the filesystem.

### Returning to stock firmware

1. Shut down normally.
2. Remove the SD card.
3. Power on again; the radio generally boots stock firmware from internal eMMC.

---

> Important: Baseband (BASE) must be 1.1.9. Below that version you may see waterfall zoom misalignment, IF/FFT anomalies, and abnormal FT8 / DSP behavior. Upgrade the baseband before using this image.

### About FT8 (read this first)

FT8 QSO and automation logic in this firmware is a full rewrite relative to upstream (stateless QSO engine), not merely “a few extra buttons.” The goal is smoother behavior on busy bands, with multiple callers, and when switching auto / manual — but behavior may still differ from upstream. Treat it as a test-oriented enhanced build; report anomalies to the maintainer (Auto / TX CQ state, on-screen prompts, and approximate frequency are enough).

---

## What you get beyond upstream (quick view)

| Category | Capability | Where you use it |
|----------|------------|------------------|
| FT8 core | Rewritten QSO / auto logic | Throughout FT8 |
| FT8 buttons | Free MSG, Auto (4 levels), Auto Mode, Auto DNF, TX CQ even/odd, Processor (incl. NA VHF) | Right-side buttons across 4 pages |
| FT8 power | Up to 10 W in FT8 (external cooling required; see below) | Entering FT8 may prompt that power was limited |
| TX timing | TX tail align (no separate button) | Always on |
| Call confirmation | Full / Pre require PTT / VFO nod before calling a new station (see 5.1.1) | Always on |
| FT8 logs | RX/TX text under `/mnt/ft8_logs/` | Download via web File browser |
| System logs | TX diagnostics under `/mnt/tx_logs/` | Maintainer troubleshooting |
| Web | Remote, Logbook, OTA (plus upstream Bands / Files / Time / dmesg, etc.) | `http://<radio-IP>/` |

---

## 1. Before you start

1. Callsign and grid are set.
2. Antenna is connected; SWR is acceptable.
3. Obey regulations; unattended automatic FT8 transmit is prohibited.
4. FT8 power and cooling (must read): entering FT8 raises the limit to 10 W (upstream was 5 W; higher settings are clamped to 10 W with a prompt). At 10 W you must use active external cooling (e.g. a fan on the chassis / PA). FT8 duty cycle is high; inadequate cooling can destroy the PA quickly. If you cannot cool properly, use 5 W or less and shorten continuous TX.
5. BASE = 1.1.9.
6. FT8 is enhanced / test-oriented; report issues to the maintainer.

---

## 2. Entering FT8

1. Main screen: APP → FT8.
2. Decoded list is in the center; function buttons are on the right (4 pages).

Exit: press Back. Incomplete QSOs may be flushed to the log on exit per rules (see Logs and records).

---

## 3. Paging FT8 buttons

Press **Page: n:4** to cycle pages.

| Page | Buttons |
|------|---------|
| 1 | Free MSG · TX CQ · Auto · Auto Mode |
| 2 | Show (CQ/All) · Mode (FT8/FT4) · Hold Freq · TX Call |
| 3 | Force QSO save · CQ Modifier · Time Sync · Auto DNF |
| 4 | Processor (everyday / NA VHF, etc.) |

---

## 4. Manual QSO (tap the list yourself)

The most reliable method.

1. Set Auto to Off (page 1).
2. Set TX Call to Enabled (page 2).
3. Select a row in the decode list and confirm (tap that message).
4. The radio builds the next message and transmits in the appropriate slot.
5. Continue when the other station replies, through 73.
6. On completion you usually see “Saved QSO de …”.

Tips:

- Tappable rows are not only CQ: mid-QSO lines from others, and others’ RR73/73, also start a call to that station (similar to double-click in WSJT-X); messages addressed to you advance the next step.
- Tapping the list turns CQ off and Auto back to Off so you follow that one station.
- If the other station stays silent for several rounds, TX stops after about 5 more attempts to avoid empty transmits.
- Further actions while already transmitting may stop TX (TX Call off) — trust what the radio shows.
- If a reply cannot make the start of the slot, that slot may not TX and the next slot is recalculated — that is normal.

---

## 5. Auto station selection (Auto and Auto Mode)

### 5.1 Auto

| Display | Meaning | Best for |
|---------|---------|----------|
| Off | No automation | Pure manual |
| Res | Answer only stations calling you; do not call others’ CQ | Running TX CQ and waiting for callers |
| Full | Also call others’ CQ from the list | Low effort, with an operator present |
| Pre | Can also tail-end when others are finishing | Experienced ops; crowded frequencies |

### 5.1.1 Active calls need your nod (Full / Pre)

When Full / Pre would actively call a station **not yet called since this FT8 entry**, it does not transmit immediately. The screen prompts:

```text
TX2 BH4XXX? PTT/VFO=OK
```

- **Accept**: press PTT once (long or short), or turn the main knob (VFO). If you confirm in time (about 5 s for FT8, about 1.5 s for FT4), that round calls the station.
- **Ignore**: timeout cancels with no TX. The same station will be asked again next time it appears, until you nod.
- Stations you have already nodded for are not asked again in this session; stations you tapped manually also count as nodded. Leaving FT8, switching FT4/FT8, or changing band clears the list.

Details:

- Turning VFO during confirm **does not** change frequency; normal tuning resumes right after confirm.
- Inside the FT8 UI, PTT no longer keys TX (it is unused for TX in FT8) and acts only as the confirm key.
- Replies when someone calls you (report, RR73, 73) need no confirm and proceed automatically.
- The prompt appears **only on the radio screen** — **no beep, and no push to web Remote**. So Full / Pre calling new stations requires someone watching the radio — by design: unattended automatic FT8 TX is not allowed.

### 5.2 Auto Mode (who to pick when many)

| Display | Priority |
|---------|----------|
| SNR | Strongest signal |
| Dist | Farthest distance (needs a complete grid) |
| Rnd | Random |
| Grid | New grids not yet worked |

Auto Mode selection is remembered; Auto defaults to Off every time you enter FT8 and must be turned on manually.

Auto mode also roughly:

- Prefer answering stations that call you;
- Generally avoid actively calling stations already worked (still answers if they call you);
- Briefly skip a CQ after repeated failures instead of hammering it;
- Prompt for PTT/VFO nod before actively calling a new station (see 5.1.1);
- Pre can insert after RR73/73 closeout — use carefully when crowded.

### 5.3 Decode list colors (when viewing CQ)

| Appearance | Meaning |
|------------|---------|
| Bright green CQ | Not yet worked on this band + mode |
| Dark green CQ | Worked before, but not on current band + mode |
| Strikethrough CQ | Already worked on current band + mode (auto usually skips) |
| Red | Calling you (to me) |

### 5.4 Don’t mix manual and auto (experience)

- Tapping a list row tends to turn CQ off and Auto to Off, following that station.
- Changing Auto / Auto Mode usually turns CQ off and restarts auto logic.
- Turning on TX CQ often forces Auto to Res.

Trust on-radio prompts.

---

## 6. Calling CQ yourself (TX CQ)

| Display | Meaning |
|---------|---------|
| Off | Do not call |
| Even | CQ on even slots (e.g. :00, :30) |
| Odd | CQ on odd slots (e.g. :15, :45) |

Upstream only has on/off; this firmware uses Even/Odd for slot parity.

1. Set callsign and grid.
2. Set TX CQ to Even or Odd.
3. After “Next TX: CQ …” it repeats per the setting.
4. When someone calls you, answering takes priority and CQ yields; CQ can resume after that exchange.
5. After enough unanswered CQs, TX CQ may return to Off automatically.
6. Stop calling: set back to Off.

CQ Modifier (page 3) adds a CQ suffix (e.g. `POTA`, contest-related). Under NA VHF, Processor may also affect modifiers — see section 11.

---

## 7. Free MSG (custom short text)

Page 1 Free MSG: up to 13 characters, scheduled for TX.

Allowed characters: space, digits, A–Z, and `+ - . / ?` (lowercase is uppercased).

Common examples: `TU 73`, `PSE QSY`, `QSO B4`.

1. Press Free MSG, enter text, confirm.
2. TX CQ is turned off, and the next eligible TX slot is scheduled (if this slot is missed, the next same-parity slot).
3. Prompt: “Next TX: …”.
4. Until it is sent, auto QSO / CQ will not overwrite this short message.
5. Last content is remembered.

Illegal or too-long text shows failure and does not transmit.

---

## 8. Other page 2 buttons

| Button | Role |
|--------|------|
| Show: CQ / All | Show CQ only, or all decodes |
| Mode: FT8 / FT4 | Switch protocol (clears the current FT8 session) |
| Hold Freq: Disabled | Jump to the other station’s frequency when following |
| Hold Freq: Enabled | Stay on your frequency; only mark the other station’s offset |
| TX Call: Enabled / Disabled | Master TX switch; Disabled = receive only |

---

## 9. Other page 3 buttons

| Button | Role |
|--------|------|
| Force QSO save | Batch-write records that already have reports but no formal 73 into the log |
| CQ Modifier | CQ suffix |
| Time Sync | Align to FT8 timing (try when decode looks wrong) |
| Auto DNF | Auto-notch local strong spikes (next section); often defaults On |

TX tail align has no button: if start is slightly late, the leading part of the waveform may be trimmed to align the end, rather than sending a misaligned block.

---

## 10. Auto DNF (notching local strong signals)

### What it solves

Nearby high-power SSB, strong carriers, or narrow spikes can overload the front end so few weak FT8 signals decode. Auto DNF finds one narrow peak clearly above the noise floor each slot, temporarily applies an ~70 Hz notch, then removes it when leaving the slot or before you transmit.

### What you need to know

1. Scan roughly 0.25–0.75 s after slot start (FT8 slot ≈ 15 s).
2. Default threshold is about 40 dB above the noise floor (tunable via `/mnt/` parameter files; see below).
3. Notch is cleared before you transmit; leaving FT8 restores DNF as it was on entry.
4. If the main UI already has DNF Auto on, FT8 Auto DNF may yield to avoid fighting.

### Waterfall cues

| Display | Meaning |
|---------|---------|
| Blue vertical + dB | Strong peak detected, not yet notched (below threshold or Off) |
| Red vertical + dB | Temporary notch applied |

### How to use

| Situation | Suggestion |
|-----------|------------|
| Normal operation | Keep On |
| Decode count drops suddenly; strong spikes | Confirm On; watch the verticals and dB |
| Leave DNF alone | Off |
| Main UI DNF Auto always on | Keep only one active |

### Advanced tuning (optional)

Single-line numeric files under `/mnt/` (defaults are created if missing):

| File | Default | Role |
|------|---------|------|
| `auto_notch_threshold_db.txt` | 40.0 | Threshold above noise floor (dB) |
| `auto_dnf_scan_start_sec.txt` | 0.25 | Scan start (s) |
| `auto_dnf_scan_end_sec.txt` | 0.75 | Scan end (s) |
| `auto_dnf_clear_time_sec.txt` | 0.25 | Clear notch early before slot end (s) |

---

## 11. Processor / NA VHF (page 4)

1. Set Processor to NA VHF (or the contest profile shown on the radio).
2. Exchange is mainly callsign + grid (including `R` + grid), not the usual multi-step signal reports.
3. CQ may carry contest-related modifiers.
4. After the contest, switch back to the everyday profile so you don’t send the wrong format.

Contest rules are set by the organizer.

---

## 12. Where logs and records live

### QSO ADIF

- When a QSO completes and Saved is shown, it is written.
- File: `/mnt/ft_log.adi`.
- Force QSO save writes the same file.

### Detailed FT8 RX/TX log

- `/mnt/ft8_logs/x6100_log_<timestamp>.txt`.
- Roughly one `RX,…` / `TX,…` line per decode/transmit for review — not a QSO summary.

### System TX diagnostic log

- `/mnt/tx_logs/tx_log_<timestamp>.txt`.
- TX events across modes for maintainer troubleshooting; normal QSOs don’t need this.

### Other

- Free MSG memory: `/mnt/ft8_freetext.txt`.
- Worked database: `/mnt/qso_log.db` (clearable via web Logbook).
- Import staging: `/mnt/incoming_log.adi` (after Logbook upload, imported on the radio).

If paths are unclear, use the web File browser / Logbook.

---

## 13. Phone or PC: web features

Connect the radio to Wi-Fi and open `http://<radio-IP>/` in a browser (example: `http://192.168.1.50/`).

### 13.1 Remote

1. Tap Remote at the top.
2. Keys (POWER, PTT, BAND, MODE, F1–F5, etc.), knobs (VOL / MFK / VFO), refresh screen capture.
3. Remotes the whole radio, not only FT8; entering FT8 still requires APP → FT8 in the UI.
4. Prefer home LAN only; remote PTT is the same as pressing PTT on the radio — an operator must be present. If the screenshot isn’t ready, refresh again shortly.

### 13.2 Logbook

| Action | Notes |
|--------|-------|
| Table | Browse `/mnt/ft_log.adi` |
| Download | Download local ADI |
| Delete | Delete local ADI (UI restarts briefly) |
| Upload | Upload WSJT-X / LoTW etc. `.adi`, import worked |
| Reset qsolog | Clear worked database |

Screen may go black briefly during upload / delete / reset — normal.

### 13.3 OTA (GUI program only)

OTA replaces the on-radio GUI binary without rewriting the whole SD card.

Package requirements: the zip contains exactly one file whose name is the sha256 of that file’s contents (64 lowercase hex digits). On success the UI restarts.

```bash
sha=$(sha256sum x6100_gui | awk '{print $1}')
cp x6100_gui "$sha"
zip -j gui-ota.zip "$sha"
```

OTA does not update the web service itself; major Web changes still need an image or Web-component update per the maintainer.

### 13.4 Other menus

| Menu | Use |
|------|-----|
| File browser | Download ADIF, ft8_logs, tx_logs, screenshots, recordings, etc. |
| Time Editor | NTP / manual time / timezone (strongly recommended before FT8) |
| dmesg | Tail of kernel log; screenshot for the maintainer when troubleshooting |
| Bands / Digital modes / General | Band and digital frequencies (change only if you know what you’re doing) |

File browser: large text may show only a portion — download for the full file. Downloading `params.db` while the radio is writing the DB can occasionally yield an incomplete copy; trust the on-radio file.

---

## 14. Recommended setups

Quiet manual:

- Auto: Off · TX Call: Enabled · tap the list

Call CQ and wait:

- TX CQ: Even or Odd (Auto often Res) · TX Call: Enabled

Hands-off auto hunt for CQ (operator must watch):

- TX CQ: Off · Auto: Full or Pre · Auto Mode: SNR or Grid · TX Call: Enabled

Receive only:

- TX Call: Disabled

Send one short phrase:

- Free MSG

North America VHF digital contests, etc.:

- Processor → NA VHF; switch back after the contest

---

## 15. Safety and operating courtesy

1. This is not an unattended tool — you can turn off TX Call or leave FT8 at any time.
2. Don’t occupy the frequency with long pointless TX; send 73 promptly when done.
3. 10 W with high duty cycle requires active external cooling; don’t use full 10 W without airflow.
4. Keep remote control on your home Wi-Fi; do not expose the admin page to the public internet.
5. See the [Disclaimer](DISCLAIMER.md).

---

## 16. FAQ

Q: No decodes in the list?
A: Check frequency is in the FT8 segment, Mode is correct, try Time Sync, check antenna and SWR.

Q: Tapping a message shows Invalid message?
A: Common cases: a 73 to you (nothing left to send), a partial decode, or a callsign that cannot be parsed. Mid-QSO GRID/report lines from others are usually tappable and start a call to them.

Q: Auto won’t call a certain CQ?
A: Check strikethrough / already worked, recent failure skip, or change Auto Mode / go manual.

Q: Difference between ft8_logs and tx_logs?
A: The former logs only FT8 decode and TX; the latter logs TX events across modes for the maintainer.

Q: How do I stop TX mid-QSO?
A: Set TX Call to Disabled; set Enabled again to resume.

Q: Phone can’t open the web page?
A: Same Wi-Fi, correct IP, and URL includes `http://`.

Q: Does Free MSG conflict with CQ?
A: On confirm it turns TX CQ off and holds priority until sent — no need to wait for “TX busy”.

Q: Fewer decodes; looks like a local strong station?
A: Watch strong spikes on the waterfall; keep Auto DNF On; otherwise QSY, or ask the maintainer to tune the threshold files.

Q: Can I use full 10 W?
A: Yes, but only with active external cooling; otherwise use 5 W or less.

Q: Contest but still sending reports instead of grid?
A: Check page 4 Processor is set to NA VHF.

Q: After OTA the web menus are still old?
A: Expected — OTA replaces only the GUI program.

Q: After Logbook upload, worked didn’t change?
A: Confirm upload succeeded, UI restarted, and the file is `.adi`.

---

Questions, or odd FT8 behavior: contact the maintainer with the on-screen English prompts and the button settings at the time.

If on-radio or web copy differs from this document, the device wins.
