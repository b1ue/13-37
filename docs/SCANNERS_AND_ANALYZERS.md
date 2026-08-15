# Scanners and analyzers

The detector tiles answer “did I see a known signature?” while the analyzers
show the surrounding radio activity without decoding payload content. Counts
are observations, not guaranteed unique physical devices: randomized MAC
addresses, retransmissions, and overlapping signatures can affect totals.

## Wi-Fi analyzer

The analyzer puts the ESP32-S3 Wi-Fi radio into passive promiscuous mode and
hops through channels 1–13 at 150 ms per channel. Each bar is an
exponentially-smoothed packet rate; the white outline marks the channel being
sampled. `peak dBm` is the strongest frame seen on that channel, while the
quiet/busy summary compares observed packet rates. This measures airtime
activity, not throughput or interference from non-Wi-Fi transmitters. Opening
the analyzer disconnects station Wi-Fi and restores the prior radio mode when
you leave.

## Bluetooth analyzer

The Bluetooth view passively receives BLE advertisements and groups recently
seen addresses by RSSI. A device expires after ten seconds without another
advertisement. RSSI is only a rough proximity signal: antenna orientation,
the body, walls, and each transmitter's power can shift it substantially. The
display reports active addresses, advertisement rate, the session peak RSSI,
and the address responsible for that peak. BLE scanning and the HID mouse are
mutually exclusive because the ESP32 Bluedroid stack exposes one GAP callback
owner; stop one before starting the other.

## LoRa spectrum analyzer

This is an RSSI sweep, not a LoRa packet decoder. It samples 13 frequency bins
with a 30 ms dwell and a 500 kHz receive bandwidth. The band button cycles the
US 915 MHz, EU 868 MHz, and 433 MHz ranges. Bar height is instantaneous RSSI;
the status line retains the strongest session sample and its frequency. The
watch antenna and matching network are optimized for the hardware variant's
native band, so off-band values are comparative rather than calibrated.

The SX1262 is shared with Meshtastic, pager, TPMS, and APRS features. The
analyzer refuses to take the radio from Meshtastic; it pauses the other three
and restores whichever one was running when you exit.

## Rolling RX analyzer

Rolling RX is a receive-only amplitude/pulse-timing observer at 433.92 or
868.35 MHz. It calibrates an RSSI threshold above the local noise floor,
records high/low run lengths from a strong nearby burst, normalizes those
durations against the estimated short pulse, and compares each fingerprint to
the preceding capture. Repeats suggest a static frame; multiple changed frames
with similar pulse count and base timing are labelled a rolling-code candidate.
Noise, multipath, multiple buttons, and unrelated remotes can produce similar
results, so the label is a correlation aid rather than a protocol verdict.

The receiver never decrypts a payload and the module exposes no transmit or
replay path. With an SD card present, `/Rolling/<session>.csv` stores the
frequency, RSSI, normalized signature, comparison result, and original pulse
durations for offline study. The SX1262 has no hardware OOK demodulator, so
weak signals are less reliable than they would be on a CC1101-class receiver.
The 315 MHz option is intentionally absent because this board's antenna/front
end was not usable for receive at that frequency.

## NFC inspector

NFC inspection polls A, B, F/FeliCa, V/ISO15693, and ST25TB technologies. It
reports the RF interface, identifier, activation metadata, NFC Forum tag type,
NDEF state/capacity, decoded records, and a raw-message preview. Wi-Fi keys are
masked on screen. Tapping Save writes the full report and raw NDEF message to
`/NFC`, so saved scans should be handled as potentially sensitive data.

## Signature scanners

- AirTag listens for Apple Find My-style BLE advertisements. A hit indicates a
  matching broadcast format, not ownership or malicious intent.
- Flipper looks for BLE advertisement patterns commonly emitted by Flipper
  Zero spam/demo applications; compatible third-party emitters can match too.
- Skimmer watches for selected BLE names/services associated with suspicious
  point-of-sale overlays. Treat a hit as a lead to inspect, not proof.
- Evil Twin compares nearby Wi-Fi identifiers and security/channel traits for
  suspicious duplicates. Legitimate mesh and enterprise deployments can look
  similar.
- Flock detects configured Wi-Fi/BLE signatures associated with supported
  surveillance vendors. Name-only hits are Low confidence, OUI hits are
  Medium, and a name/OUI vendor agreement is High. These grades describe
  signature corroboration, not certainty that a particular camera is present;
  radio signatures and vendors change over time. Optional banners/haptics are
  local-only, and `/Flock/sightings.csv` can be mapped offline.
- Wardriver records general Wi-Fi and BLE observations to SD. It is a survey
  logger rather than a signature verdict and should only be used where lawful.

For meaningful comparisons, let an analyzer complete several full sweeps,
keep the watch orientation fixed, and compare changes from the same location.
