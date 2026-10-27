# WAVESCAN — synthé à tables d'ondes 8 voix pour M-VAVE FM-1

Firmware alternatif qui transforme le FM-1 en synthé à tables d'ondes sur le modèle du PPG Wave 2.2 / 2.3 (et de son émulation Waldorf PPG Wave 3.V). Projet indépendant, sans lien avec Waldorf, PPG ni M-VAVE. Les tables d'ondes sont originales, calculées par `tools/gen_wavetables.py` ; ce ne sont pas des copies des ROM du PPG.

Bâti sur [foma](https://github.com/konsumer/foma) (plateforme Felucca) : installeur web, mode secours et retour au firmware d'origine inclus.

## Architecture

| Élément | Contenu |
|---|---|
| Voix | 8, polyphonique ou mono avec glide |
| Oscillateurs | 2 par voix (A et B), qui lisent une table de 64 ondes 8 bits de 128 échantillons ; B a son propre décalage en demi-tons et en cents |
| Tables | 20 tables : RESONANT, HARMONICS, PULSE WIDTH, SYNC SWEEP, ORGAN, VOX, BELLS, DIGITAL, OCTAVES, ODD HARM, CHOIR, BRASS, FOLD, PHASE DIST, FM, FM 3:1, STRINGS, GRIT, SAW>SINE, BASICS. Comme sur le PPG, les ondes 60 à 63 de chaque table sont triangle, impulsion, carré et dent de scie |
| Mode des ondes | STEP (façon 2.2 : sauts d'onde en onde et crénelage d'origine) ou SMOOTH (façon 2.3 / 3.V : ondes fondues, échantillons interpolés, copies limitées en bande pour les aigus) |
| Filtre | passe-bas 24 dB résonant (EMPHASIS jusqu'à l'auto-oscillation), suivi du clavier, puis un étage d'overdrive |
| Enveloppes | ENV1 → filtre, ENV2 → volume, ENV3 (attaque-déclin) → position d'onde, hauteur ou onde A seule |
| LFO | triangle, dent de scie descendante et montante, carré, aléatoire ; retard par note ; vers ondes, hauteur, filtre, volume |
| Sortie | delay stéréo ping-pong, répartition stéréo des voix |

La modulation est calculée par blocs, comme la fréquence de contrôle de l'original, ce qui garde un peu de son côté « en escalier » sur les balayages.

## Commandes

| Commande FM-1 | Fonction |
|---|---|
| HOME | page PERFORM : TABLE, WAVES A, CUTOFF, EMPHASIS |
| SCL | oscillateurs : WAVES A, WAVES B, B SEMI, DETUNE (re-presser : mix A/B, STEP/SMOOTH, suivi clavier des ondes, table) |
| ENV | ENV1 filtre, re-presser ENV2 volume, re-presser ENV3 |
| EDIT | filtre : CUTOFF, EMPHASIS, ENV1, KEYTRACK |
| LFO | forme, vitesse, retard, → ondes (re-presser : → hauteur, → filtre, → volume) |
| FX | delay : mix, temps, réinjection ; écart stéréo |
| GLO | voix : POLY/MONO, glide, drive, volume |
| KNOB 1–4 | les quatre paramètres de la page (tourner vite : pas de 4) |
| SELECT | choix de la table d'ondes |
| ALGORITHM | WAVES A : le geste PPG, balayer la table à la main |
| PRESETS | F1–F8 usine, U1–U8 utilisateur |
| SAVE | enregistrer : PRESETS choisit l'emplacement, SAVE confirme, HOME annule |
| PLAY | HOLD (les notes continuent de sonner) |
| OCT− / OCT+ | octave du clavier (les deux : remise à zéro) |
| MASTER | volume |
| MIDI | notes, pitch bend ±2 demi-tons, CC 20–59 = les 40 paramètres |

L'écran montre la table en cascade 3D (onde A en orange, onde B en bleu), une règle 0–63 avec la position d'onde qui joue en direct, l'afficheur LCD ambre des paramètres, et les 8 voix actives.

Presets d'usine : WAVE PAD, DIGI BELL, SWEEP BRASS, VOX CHOIR, SYNC LEAD, PWM STRINGS, ORGAN, WAVE BASS.

## Obtenir et installer le firmware

- **GitHub** : pousse ce dossier sur la branche `main` d'un dépôt. Le workflow compile tout ; le `.fwsc` est dans l'artefact `wavescan-fwsc` et sur la branche `firmware`. Avec Settings → Pages → Source : GitHub Actions, l'émulateur et l'installeur sont publiés sur GitHub Pages.
- **Docker Desktop** : `./fm1 build` ou `./fm1 dev` (http://localhost:8000).

Installation depuis la page Install (Chrome ou Edge, FM-1 en USB). **Secours** : OCT− maintenu à l'allumage ; l'onglet Rescue remet le firmware M-VAVE V15. Installer un firmware non officiel se fait à vos risques.

## Développement

- `tools/gen_wavetables.py` : les tables (et `--png` pour les voir) → `src/wavetables.h`
- `src/synth.c` : voix, oscillateurs, filtre, enveloppes, LFO, delay (entiers uniquement)
- `src/main.c` : pages, écran, LEDs, MIDI, presets ; `src/presets.c` : sons d'usine
- `tests/run.sh` : rend les 8 presets en WAV, mesure le coût CPU, fuzz
- Identité du paquet : `FM-1_82` (dans `firmware.json`)

## Licence

GPL-3.0-only. Crédits : [LICENSING.md](LICENSING.md).
