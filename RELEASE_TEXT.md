# Release v1.0.0

Erste oeffentliche Hotspot-Version des Balkonkraftwerk Kombi-Displays.

## Inhalt

- Fertige Factory-Firmware fuer ESP32-2432S028R
- Setup-Hotspot fuer WLAN- und OpenDTU-OnBattery-Daten
- 2 Display-Seiten fuer Wechselrichter- und Marstek-B2500-Akkudaten
- PDF-Kurzanleitung zum Flashen ueber ESPHome Web
- Quellcode fuer PlatformIO

## Firmware

Download:

```text
firmware/balkonkraftwerk-kombi-display-hotspot-v1-factory.bin
```

Flashen ueber:

```text
https://web.esphome.io/?dashboard_install
```

## Einrichtung

Nach dem Flashen startet das Display den Hotspot:

```text
Balkonkraftwerk-Setup
```

Dann per Handy verbinden und im Browser oeffnen:

```text
http://192.168.4.1
```

Folgende Daten eintragen:

- WLAN-Name
- WLAN-Passwort
- OpenDTU-OnBattery IP
- OpenDTU Benutzername
- OpenDTU Passwort
- Inverter-Seriennummer

## Hinweise

- Keine privaten Zugangsdaten in der Firmware.
- Kein offizielles OpenDTU-Projekt.
- Getestet mit ESP32-2432S028R, OpenDTU-OnBattery und Marstek B2500.

