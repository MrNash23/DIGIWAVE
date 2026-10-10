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

## Funktionen

- Kachelmenü mit eigenen Grafiken (Ordner `DIGIWAVE_Firmware_Grafiken`)
- Wasserfall unter den Dekodierungen
- Vollständige FT8/FT4-Verbindungen: CQ rufen oder eine Station mit einem Druck
  beantworten; das Logbuch liegt als ADIF auf der SD-Karte (`/digiwave.adi`)
- CB-Betrieb (11 m) für FT8/FT4 nach den Regeln von WSJT-CB: CB-Rufzeichen wie
  `26AT016`, eigene Nachrichtenfolge, Länderliste nach Divisionsnummer
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
  Läuft auch auf dem Mac und wird dort getestet (42 Tests, `pio test -e native`)
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
- NAVTEX und WEFAX sind bisher nur mit erzeugten Signalen getestet.
- Senden ist in keiner Betriebsart auf dem Band getestet.
- Audio läuft vorerst über Mikrofon und Lautsprecher des Pagers.

## Geplant

- Zusatzplatine für den 12-Pin-Anschluss mit Line-In, Line-Out und PTT
- CW-Tastleitung, Logbuch auch für CW- und RTTY-Verbindungen
- Lizenzdatei für das Repository
