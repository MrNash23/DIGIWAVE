# DIGIWAVE – Projektbeschreibung

## Worum es geht

DIGIWAVE ist eine eigene Firmware für den LilyGo T-LoRa Pager (ESP32-S3, 16 MB
Flash, 8 MB PSRAM, Display 480×222, Tastatur, Drehrad, GPS, SD-Karte). Sie macht
aus dem Pager ein Taschengerät für digitale Betriebsarten: Es hört das Audio
eines Funkgeräts mit, dekodiert es und zeigt das Ergebnis an. In den
sendefähigen Betriebsarten erzeugt es auch das Sendesignal und tastet das
Funkgerät. Ein Computer wird nicht gebraucht.

Repository: https://github.com/MrNash23/DIGIWAVE (öffentlich)

## Betriebsarten

| Betriebsart | Empfang | Senden |
| --- | --- | --- |
| FT8, FT4 | ja, mit Locator und Land je Station | ja, mit automatischem QSO-Ablauf und ADIF-Log |
| CW | ja, in einem 100-Hz-Fenster, das mit dem Rad verschoben wird | ja, über die Tastatur |
| RTTY (45,45 / 50 Baud) | ja | ja |
| NAVTEX | ja | – |
| WEFAX (120 LPM) | ja, Bilder als BMP auf der SD-Karte | – |
| SSTV (Martin 1/2, Scottie 1/2, Robot 36, PD 120/180) | ja, in Farbe, Bilder als BMP auf der SD-Karte | – |

## Funktionen

- Kachelmenü mit eigenen Grafiken (Ordner `DIGIWAVE_Firmware_Grafiken`)
- Wasserfall unter den Dekodierungen
- Vollständige FT8/FT4-Verbindungen: CQ rufen oder eine Station mit einem Druck
  beantworten; das Logbuch liegt als ADIF auf der SD-Karte (`/digiwave.adi`)
- CB-Betrieb (11 m) für FT8/FT4 nach den Regeln von WSJT-CB: CB-Rufzeichen wie
  `26AT016`, eigene Nachrichtenfolge, Länderliste nach Divisionsnummer
- „Schon gearbeitet": Stationen aus dem Logbuch des Geräts erscheinen in der
  Liste grau (dieses Band) bzw. gedämpft (nur anderes Band)
- CQ mit Zusatz (POTA, SOTA, DX ...), Entfernung zu jeder gehörten Station,
  Kennzahlen zum Logbuch (je Band und Betriebsart, Länder, weiteste Verbindung)
- DWD-Wetterfunk über RTTY: Frequenzwahl stellt Tempo und Shift passend ein
- Einheitliche Bedienung: Rad wählt oder stimmt ab, Rad-Druck öffnet in jeder
  Betriebsart die Liste ihrer Funktionen (mit Kurztasten), Enter ist die
  Hauptaktion, Backspace geht eine Ebene zurück bis ins Menü
- Gespeicherte WEFAX-Bilder lassen sich am Pager ansehen und löschen, auf der
  Handy-Seite ansehen, herunterladen und löschen
- Eigene Sendungen stehen in der Liste; beim Rufen oder im QSO zeigt die Liste
  nur das eigene QSO (Taste V wechselt zwischen QSO und allen Empfangenen)
- Band und Frequenz fürs Logbuch: Da das Funkgerät von Hand abgestimmt wird,
  öffnet Taste B in FT8/FT4 die Liste der üblichen Frequenzen je Band oder
  nimmt eine eingetippte; FT8, FT4 und CB-Betrieb merken sich je ihre eigene
- Zeit aus GPS mit Sekundenimpuls, ohne Empfang aus der Echtzeituhr
- Locator automatisch aus der GPS-Position oder von Hand
- WLAN-Zugangspunkt mit einer Seite fürs Handy: Dekodierungen, wichtigste
  Bedienung, WEFAX-Bilder, Download des Logbuchs
- Stromsparen: Display aus nach drei Minuten ohne Eingabe; Tiefschlaf, wenn in
  dieser Zeit auch nichts empfangen wurde. Aufwecken mit dem Rad. Ausschalten
  über „Switch off" im Setup

## Aufbau

- `lib/ft8core` – Signalverarbeitung ohne Hardwarebezug: Dekoder, Encoder,
  QSO-Ablauf, ADIF, Länder- und CB-Tabellen, CW, FSK-Modem, RTTY, NAVTEX, WEFAX.
  Läuft auch auf dem Mac und wird dort getestet (47 Tests, `pio test -e native`)
- `lib/ft8_lib` – übernommene Bibliothek von Karlis Goba (MIT) für die
  FT8/FT4-Codierung
- `src` – Firmware: `engine` (Empfangs- und Sendetask für alle Betriebsarten),
  `ui` (Bildschirme und Bedienung), `web` (Handy-Seite), `timebase` (Zeit),
  `settings`, `logbook`, `hal` (Audio und PTT hinter Schnittstellen)
- `tools/make_assets.py` – wandelt die Grafiken in Bilddaten für die Firmware
- `docs` – Fotos für die GitHub-Seite

## Bauen und Installieren

    pio run -e pager        # Firmware
    pio test -e native      # Tests der Signalverarbeitung

Gebaut wird mit PlatformIO und der pioarduino-Plattform (Arduino-ESP32 3.3).
Auf dem Pager liegt DIGIWAVE als eine App neben anderen (zum Beispiel
Meshtastic) und wird über den Launcher von bmorcelli gestartet.

## Stand

- FT8-Empfang ist am Gerät mit echten Signalen geprüft; ein Zeitschlitz wird in
  etwa 0,7 s dekodiert.
- NAVTEX und SSTV sind bisher nur mit erzeugten Signalen getestet; WEFAX hat echte Wetterkarten empfangen.
- Senden: FT8-CQ über die Zusatzplatine auf dem Band gesendet (PTT und Pegel geprüft), noch ohne Antwort einer Gegenstation.
- Audio läuft über die Zusatzplatine am 12-poligen Anschluss (Line-In, Line-Out,
  PTT), wenn sie steckt; sonst über Mikrofon und Lautsprecher des Pagers. Die
  Auswahl steht im Setup unter „Audio“.

## Geplant

- CW-Tastleitung, Logbuch auch für CW- und RTTY-Verbindungen

## Lizenz

Copyright (C) 2026 Andres Leroi. DIGIWAVE steht unter der GNU General Public License, Version 3 oder später
(`LICENSE`). Die verwendeten Bibliotheken und Daten mit Urhebern und Lizenzen
stehen in `THIRD-PARTY-NOTICES.md` und am Gerät unter Setup, „About DIGIWAVE".
