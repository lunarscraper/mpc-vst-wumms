# mpc-vst-wumms

**Wumms** (Arbeitstitel) – End-of-Chain-Gerät als VST2-**Insert-Effekt** für die Akai Force
(und MPC Live/One/X/Key), nachempfunden nach dem OTO Machines Boum: Gate, Ein-Knopf-Kompressor,
Lo Cut, vier Verzerrungstypen, Hi Cut, Dry/Wet-Mix. Gedacht für den Master oder eine Gruppe.
Dazu eine zuschaltbare **MASTER-Seite** (EQ, Mono-Bass, Stereobreite, True-Peak-Limiter, Lautheits-
und Limiter-Anzeige), mit der sich auf der Force ein fertiges Master machen lässt, ohne Umweg über
den PC.

**Status:** Version 0.2.0 (MASTER-Seite neu). Offline-Test (`vst/test.sh`) besteht, Actions-Build
und Test auf der Force stehen aus.

## Abgrenzung

- Kein Port und keine Schaltungsemulation: Das Original ist analog. Hier sind Bedienkonzept und
  Signalweg nachgebaut, die Kennlinien sind eigene. Ziel ist der Charakter, nicht die Messkurve.
- Kein OTO-Branding, eigener Skin. „OTO" und „Boum" gehören ihren Inhabern, keine Verbindung.
- Nicht übernommen: Sidechain-Eingang, LED-Anzeige (Pegel / Gain Reduction), die 36 Presets des
  Originals (stattdessen 32 Speicherplätze nach dem Muster von Rattler).
- Die MASTER-Seite hat kein Vorbild im Boum, sie ist eine Ergänzung.

## Signalweg

Wie beim Original:

```
In Gain ─► Gate ─► Kompressor ─► Lo Cut ─► Verzerrung ─► Hi Cut ─► Mix ─► Level ─► MASTER
   │                                (6 dB)   (4 Typen)   (12 dB)    ▲
   └──────────────────────── Dry ───────────────────────────────────┘

MASTER (zuschaltbar):
Low Shelf ─► High Shelf ─► Mono Bass ─► Width ─► Push ─► Limiter ─► Ausgang
 (120 Hz)     (8 kHz)      (LR4, M/S)                    (True Peak,
                                                          Vorausschau)
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

**Tab MASTER** – die Endstufe, bei Bedarf mit dem Feld MASTER (OFF/ON) eingeschaltet. Das Feld
liegt bewusst nicht auf einem Q-Link, damit es beim Drehen nicht umspringt; die sieben Regler schon.

| Regler | Bereich | Wirkung |
|---|---|---|
| LOW | -6 .. +6 dB | Low Shelf bei 120 Hz: Fundament von Kick und Bass |
| HIGH | -6 .. +6 dB | High Shelf bei 8 kHz: Luft und Glanz, oder Schärfe wegnehmen |
| MONO BASS | OFF, 40 .. 250 Hz | Darunter wird das Signal mono (Linkwitz-Riley-Weiche, 24 dB/Okt, phasengleich, darüber bleibt das Stereobild unberührt). Für Club und Vinyl 100 bis 140 Hz. |
| WIDTH | 0 .. 200 % | Stereobreite über Mitte/Seite. 100 % = unverändert, 0 % = mono. |
| PUSH | 0 .. +12 dB | Pegel in den Limiter: macht laut. |
| CEILING | -6,0 .. -0,1 dB | Höchster Ausgangspegel (True Peak, Näherung auf etwa 0,1 dB). -1,0 dB ist der übliche Wert. |
| RELEASE | AUTO, 10 ms .. 1 s | Wie schnell der Limiter loslässt. AUTO: 20 ms nach einzelnen Spitzen, bis 500 ms, wenn er dauernd arbeitet (weniger Verzerrung). |
| MASTER | OFF, ON | Schaltet die Seite ein (Überblendung in 20 ms, kein Knacksen). |

Zwischen den Reglerreihen zwei Anzeigen, die die Force laufend nachliest:
- **LOUDNESS**: Short-Term-Lautheit des Ausgangs nach ITU-R BS.1770 (K-Filter, 3-s-Fenster), z. B.
  `-8.4 LUFS`. Misst immer, auch bei MASTER OFF.
- **LIMITER**: die Pegelreduktion des Limiters, z. B. `GR -2.5 dB` (Spitzen werden kurz gehalten und
  fallen mit 10 dB/s), bei abgeschalteter Seite `GR OFF`.

So geht man vor: Wumms als **letztes** Plugin auf den Master-Kanal, den Master-Fader der Force auf
0 dB lassen (sonst verschiebt er das Ceiling). MASTER auf ON, CEILING auf -1,0 dB, dann PUSH
hochdrehen, bis LIMITER in den lautesten Teilen etwa 2 bis 4 dB zeigt. Mehr als 6 dB klingt meist
platt. Als Anhaltspunkt in den lauten Teilen: Club- und DJ-Material etwa -9 bis -6 LUFS, für
Streaming reichen -14 LUFS (die Dienste regeln ohnehin auf ihren Wert). MONO BASS um 120 Hz und
leichte LOW-/HIGH-Korrekturen erledigen den Rest; WIDTH sparsam, über 130 % wird es dünn in Mono.

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

Die Plätze 17 bis 24 sind Master-Presets mit eingeschalteter MASTER-Seite. Den PUSH-Wert danach am
eigenen Mix nachstellen (Anzeige LIMITER), denn wie laut es wird, hängt vom Eingangspegel ab:

| Platz | Name | Gedacht für |
|---|---|---|
| 17 | MASTER SAFE | nur der Limiter bei -1 dB als Sicherung, sonst alles neutral (live) |
| 18 | MASTER LOUD | etwas Glue, Mono Bass 100 Hz, +6 dB in den Limiter |
| 19 | GOA MASTER | Glue mit TUBE, Low und High leicht angehoben, Mono Bass 120 Hz, etwas breiter |
| 20 | TECHNO MASTER | druckvoller Glue, mehr Fundament, Mono Bass 100 Hz, Ceiling -0,8 dB |
| 21 | WARM MASTER | TUBE-Sättigung, Höhen leicht zurück, Mono Bass 140 Hz |
| 22 | BRIGHT MASTER | sanfter Glue, +3 dB Luft, etwas breiter |
| 23 | AMBIENT MASTER | kaum Kompression, breit (135 %), Mono Bass 80 Hz, langer Release, leise |
| 24 | LIVE CLUB | kräftiger Glue, +9 dB in den Limiter, Ceiling -0,5 dB, schneller Release |

Die Plätze 25 bis 32 sind frei. Wer auf 17 bis 24 schon eigene Einstellungen gespeichert hat,
behält sie: Ein gespeicherter Platz geht immer vor dem Werkspreset. Alte Projekte und Presets ohne
die neuen Werte laden mit abgeschalteter MASTER-Seite.

Das Plugin startet mit CLEAN: Das Signal geht unverändert durch.

## Technik

- `vst/wumms_core.h`: der DSP-Kern, ohne Abhängigkeiten.
- `vst/wumms_master.h`: die MASTER-Seite, ebenso ohne Abhängigkeiten. Limiter: Spitzen 4x
  überabgetastet gemessen, gleitendes Minimum und gleitender Mittelwert über 61 Samples, dadurch
  ohne Überschwinger; Ausgang nie über CEILING (Sample-Peak garantiert, True Peak auf etwa 0,1 dB).
- `vst/wumms_vst.cpp`: die Plugin-Hülle, von Hand geschrieben nach `mpc-vst-vfilter` (kein
  Wrapper, kein SDK), Speicherplätze nach `mpc-vst-rattler`.
- Verzerrung und Hi Cut laufen mit 2x-Oversampling (linearphasiges Halbband-FIR, 15 Samples),
  der Limiter schaut 64 Samples voraus. Zusammen **79 Samples Latenz** (1,8 ms bei 44,1 kHz), die
  das Plugin dem Host meldet. Die Vorausschau bleibt auch bei MASTER OFF im Signalweg, damit sich
  die Latenz beim Umschalten nicht ändert.
- Ausgangsschutz: bei MASTER OFF ab -1,4 dBFS weiche Begrenzung, nie über 0,99; bei MASTER ON
  übernimmt der Limiter.
- CPU (x86, ein Kern, alles an): rund 1,1 % mit MASTER-Seite, 0,7 % ohne. Abgeschaltet kostet die
  Seite nur die Verzögerung und die Lautheitsmessung.
- `vst/test.sh` prüft: Startwerte lassen das Signal unverändert, In Gain/Level, Kompressor
  (2:1, Limiter, Umkehr, Attack, Release), Gate, beide Filter, jede Verzerrung (sauber bei
  Drive 0, Obertöne, kein Gleichanteil), Mix, Kanaltrennung, Speicherplätze, Pegel unter
  0 dBFS, Projekt-Wiederherstellung, Zufallslauf bei 44,1/48/96 kHz; dazu die MASTER-Seite:
  neutral bei ON, Ceiling (Sample- und True Peak, auch bei plötzlichem Einsatz nach Stille),
  beide Shelves, Mono Bass, Width, Release, LOUDNESS-Anzeige (-20 dBFS Sinus = -20,0 LUFS),
  LIMITER-Anzeige, Master-Presets unter ihrem Ceiling, Zurückschalten.

Build und Installation wie bei den anderen Ports: Actions → „VST release (draft)", siehe
`docs/PORTING.md` in `sd88me/mpc-vst-plugins`. Lizenz: MIT.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
