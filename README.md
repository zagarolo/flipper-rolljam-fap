# RollJam Dual-Radio — Flipper Zero FAP (v0.1 scaffolding)

Custom FAP che porta sul Flipper la logica RollJam 90/10 già validata nel
toolchain Python dell'utente (`/home/pi/rf_tools/rolljam_full.py`).

## Hardware target

Carrier board custom con due moduli Ebyte sopra i GPIO del Flipper:

- **E07-433M20S MK2 "Ultimate Edition"** — CC1101 + PA integrato, 1W output.
- **E21-400G30S** — CC1101 high-power, 1W output.

Entrambi i moduli parlano via **SPI** (bus condiviso, CS indipendenti).
Designer originale della board: "Uchuu007" (francese, contatto perso).

## Scope legale

TX continuo ~1W a 433 MHz è **fuori limite ISM EU** (10 mW ERP).
Uso esclusivamente in ambiente confinato/schermato (capannone privato,
Faraday cage) contro **proprio hardware** per test pentest legittimo.
Mai contro target di terzi o in aree pubbliche.

## Stato implementazione (v0.1)

- [x] Scaffolding FAP con `ufbt`
- [x] Manifest `application.fam` con metadata corretti
- [x] State machine base (Menu → Armed → Jamming → Captured → Replay)
- [x] UI canvas 128x64 con stato, contatore, freq, status line
- [x] Jam loop pattern 90/10 (19 burst a 433.920 + 1 burst a 433.720)
- [x] Radio init via `furi_hal_subghz_*` (usa CC1101 interno per prima validazione)
- [ ] Driver SPI dedicato per E07/E21 esterni (CS pin da tracciare su board)
- [ ] Capture reale del codice rolling nel micro-gap del jammer (MARCSTATE RX→FIFO)
- [ ] Storage delle cattura in `/ext/subghz/rolljam/*.sub`
- [ ] Replay del codice catturato via `furi_hal_subghz_async_tx`
- [ ] Integrazione opzionale con decoder KeeLoq (porting da `keeloq_crack.py`)

## Build

```bash
cd /home/pi/rf_tools/flipper_rolljam_fap
~/.venv_ufbt/bin/ufbt
```

Output atteso: `dist/rolljam_dual.fap`. Copia su SD Flipper in
`/ext/apps/Sub-GHz/rolljam_dual.fap`.

Alla prima build, ufbt scarica la toolchain aarch64 (~200 MB) nella cartella
`~/.ufbt/`. Il target SDK può essere aggiornato con `~/.venv_ufbt/bin/ufbt update`.

## Roadmap

**v0.2** — wiring E07/E21:
- Tracciare CS pin esatti sulla board (tester continuity su header GPIO).
- Aggiungere driver SPI custom con CS switching per i due moduli.
- Testare TX pattern 90/10 da E21 (più potente) mentre E07 resta RX.

**v0.3** — capture reale:
- RX continuo su E07 durante i gap del jammer.
- Trigger sul picco di amplitudine (stessa logica `monitor_iq` del Python).
- Demodulazione OOK e salvataggio raw frame in `.sub` formato standard Flipper.

**v0.4** — replay intelligente:
- Replay frame catturato via CC1101 async TX (Flipper internal o E07 per portabilità).
- Opzione: se riconosciuto KeeLoq, estrazione rolling/fixed code e scarico su
  display + esportazione JSON per crack offline su Pi5.

**v0.5** — KeeLoq decoder on-device:
- Porting semplificato di `keeloq_crack.py` in C (solo known-key lookup).
- DB manufacturer key pre-compilato (Chrysler/Fiat/GM/VW/Toyota/Renault/
  Hyundai/Honda/Mazda/Nissan + CAME/NICE/FAAC/BFT/Hörmann).
- Match automatico → mostra "DETECTED: Fiat" sul display.

## Relazione con il codice Python esistente

Questa FAP riprende la logica algoritmica di:

- `/home/pi/rf_tools/rolljam.py` — capture+replay base
- `/home/pi/rf_tools/rolljam_full.py` — pattern 90/10 + monitor realtime
- `/home/pi/rf_tools/keeloq_crack.py` — decode rolling code + DB mfg keys

Il Python resta la reference per development/debug con HackRF+RTL-SDR sul Pi5.
La FAP è la versione portable standalone per operare senza Pi5.
