# mpc-vst-wumms

**Wumms** (Arbeitstitel) – End-of-Chain-Gerät als VST2-**Insert-Effekt** für die Akai Force
(und MPC Live/One/X/Key), nachempfunden nach dem OTO Machines Boum: Gate, Ein-Knopf-Kompressor,
Lo Cut, vier Verzerrungstypen, Hi Cut, Dry/Wet-Mix. Gedacht für den Master oder eine Gruppe.

**Status:** Version 0.1.0. Offline-Test (`vst/test.sh`) besteht, Actions-Build und Test auf der
Force stehen aus.

## Abgrenzung

- Kein Port und keine Schaltungsemulation: Das Original ist analog. Hier sind Bedienkonzept und
  Signalweg nachgebaut, die Kennlinien sind eigene. Ziel ist der Charakter, nicht die Messkurve.
- Kein OTO-Branding, eigener Skin. „OTO" und „Boum" gehören ihren Inhabern, keine Verbindung.
- Nicht übernommen: Sidechain-Eingang, LED-Anzeige (Pegel / Gain Reduction), die 36 Presets des
  Originals (stattdessen 32 Speicherplätze nach dem Muster von Rattler).

## Signalweg

Wie beim Original:

```
In Gain ─► Gate ─► Kompressor ─► Lo Cut ─► Verzerrung ─► Hi Cut ─► Mix ─► Level
   │                                (6 dB)   (4 Typen)   (12 dB)    ▲
   └──────────────────────── Dry ───────────────────────────────────┘
```

Durchgehend stereo. Gate und Kompressor messen beide Kanäle gemeinsam, damit das Stereobild
nicht wandert.

## Regler

**Tab MAIN** – das ganze Gerät auf den acht Q-Links:

| Regler | Bereich | Wirkung |
|---|---|---|
| IN GAIN | -12 .. +24 dB | Pegel in die Kette. Bestimmt, wie hart Gate, Kompressor und Verzerrung arbeiten. |
| COMP | OFF .. LIMIT .. REV | Ein Regler für Threshold, Ratio und Makeup zusammen. Anzeige als Ratio. Mitte = Limiter (∞:1), rechts davon Umkehr: Lautes wird leiser als Leises. |
| ATTACK | 0,05 .. 33 ms | Kurz fängt Transienten, lang lässt Kick und Snare durch. |
| RELEASE | 50 ms .. 1 s | Kurz pumpt, lang hält zusammen. |
| DRIVE | 0 .. 100 % | Stärke der Verzerrung. 0 = ganz aus dem Signalweg. Die Lautheit wird grob ausgeglichen. |
| DISTO | BOOST, TUBE, FUZZ, SQUARE | siehe unten |
| MIX | 0 .. 100 % | Dry/Wet, für Parallelkompression. Das Dry-Signal läuft zeitgleich mit, es gibt keinen Kammfilter. |
| LEVEL | -24 .. +12 dB | Ausgangspegel |

**Tab TONE** – Filter und Gate, dazu DISTO, DRIVE, COMP, MIX und LEVEL noch einmal:

| Regler | Bereich | Wirkung |
|---|---|---|
| LO CUT | OFF, 75, 150, 300 Hz | 6 dB/Oktave, **vor** der Verzerrung: nimmt ihr den Bass weg, damit sie nicht matscht. Für ein sauberes Fundament mit MIX kombinieren. |
| HI CUT | 20 Hz .. 20 kHz, OFF | 12 dB/Oktave, **nach** der Verzerrung: glättet die Obertöne. Ganz rechts umgangen. |
| GATE | OFF, -50 .. 0 dB | Schwelle in 10-dB-Schritten, feste Zeiten wie beim Original. |

**Tab PRESET** – PRESET blättert nur, LOAD und SAVE handeln (wie bei Rattler).

### Die vier Verzerrungen

| Typ | Kennlinie | Klang |
|---|---|---|
| BOOST | symmetrisch weich, bis 30 dB | Sättigung, dichter und lauter, ungerade Obertöne |
| TUBE | unsymmetrisch weich, bis 36 dB | wärmer, gerade Obertöne |
| FUZZ | oben hart, unten weich, Arbeitspunkt wandert mit dem Pegel, bis 48 dB | bröselig, schief |
| SQUARE | hart und steil, bis 60 dB | macht aus allem ein Rechteck |

### Der Kompressor-Regler

| COMP | Anzeige | Threshold | Makeup |
|---|---|---|---|
| 0 | OFF | – | 0 dB |
| 15 | 1.4:1 | -9 dB | +1,6 dB |
| 25 | 2.0:1 | -15 dB | +4,5 dB |
| 40 | 5.0:1 | -24 dB | +11,5 dB |
| 50 | LIMIT | -30 dB | +18 dB |
| 100 | REV 100 % | -30 dB | +18 dB |

Weiches Knie (6 dB). Zum „Zusammenkleben" auf dem Master: COMP 10 bis 25, ATTACK eher lang,
dazu wenig DRIVE mit BOOST oder TUBE.

## Presets

32 Speicherplätze, gemeinsam für alle Instanzen und Projekte (`wumms_presets.txt` neben dem
Plugin-Ordner auf der SD-Karte). Sie erscheinen auch in der Preset-Liste der Force. Die ersten 16
sind Werkseinstellungen, bis man sie überschreibt:

CLEAN, MASTER GLUE, GOA GLUE, TECHNO PUNCH, TUBE WARMTH, TAPE DARK, PARALLEL SMASH,
PARALLEL FUZZ, LIMIT, PUMP, REVERSE DUCK, FUZZ BUS, SQUARE CRUSH, LOFI RADIO, GATED DRIVE,
DRUM CRUNCH.

Das Plugin startet mit CLEAN: Das Signal geht unverändert durch.

## Technik

- `vst/wumms_core.h`: der DSP-Kern, ohne Abhängigkeiten.
- `vst/wumms_vst.cpp`: die Plugin-Hülle, von Hand geschrieben nach `mpc-vst-vfilter` (kein
  Wrapper, kein SDK), Speicherplätze nach `mpc-vst-rattler`.
- Verzerrung und Hi Cut laufen mit 2x-Oversampling (linearphasiges Halbband-FIR). Das kostet
  **15 Samples Latenz** (0,34 ms bei 44,1 kHz), die das Plugin dem Host meldet.
- Ausgangsschutz: ab -1,4 dBFS weiche Begrenzung, nie über 0,99.
- CPU: rund 0,9 % eines x86-Kerns mit allem an (zum Vergleich V-Filter rund 0,5 %).
- `vst/test.sh` prüft: Startwerte lassen das Signal unverändert, In Gain/Level, Kompressor
  (2:1, Limiter, Umkehr, Attack, Release), Gate, beide Filter, jede Verzerrung (sauber bei
  Drive 0, Obertöne, kein Gleichanteil), Mix, Kanaltrennung, Speicherplätze, Pegel unter
  0 dBFS, Projekt-Wiederherstellung, Zufallslauf bei 44,1/48/96 kHz.

Build und Installation wie bei den anderen Ports: Actions → „VST release (draft)", siehe
`docs/PORTING.md` in `sd88me/mpc-vst-plugins`. Lizenz: MIT.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
