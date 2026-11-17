# Freematics ONE+ Telelogger

Firmware für den [Freematics ONE+](https://freematics.com/products/freematics-one-plus/) (Model A/B/H). Sie sammelt GNSS-Position, OBD-II- und CAN-Daten und sendet sie über WLAN oder Mobilfunk an [Traccar](https://www.traccar.org) oder [Home Assistant](https://www.home-assistant.io).

Das Repository enthält nur noch den Telelogger, komplett neu aufgebaut auf Basis des ursprünglichen `firmware_v5/telelogger`.

## Funktionen

| Bereich | Umsetzung |
|---|---|
| Protokoll | **OsmAnd** (HTTP GET/POST). Traccar (Port 5055) und die Home-Assistant-Integration „Traccar Client“ verstehen es direkt. OBD- und CAN-Werte gehen als zusätzliche Attribute im selben Request mit. |
| Fahrzeugdaten | Frei konfigurierbare OBD-II-PIDs. **CAN-Signale** passiv (Broadcast-Frames mitlesen) oder aktiv (UDS-/OBD-Anfrage, z. B. `22F40D`). |
| Verbindung | WLAN bevorzugt, sonst Mobilfunk (SIM7600/SIM7670/SIM7070). **Roaming-fähig.** |
| Offline | Warteschlange im RAM (PSRAM: 3000 Punkte), danach im Flash. Nach Funklöchern oder bei fehlender Abdeckung wird alles nachgeliefert. |
| Konfiguration | **Web UI** über den eigenen WLAN-Access-Point oder das Heim-WLAN. Die BLE-App wird nicht mehr verwendet. |
| Start | Aufwachen **ohne Neustart** bei Spannungsanstieg oder Bewegung. Die erste Position wird sofort gesendet. GNSS bleibt optional warm, das Modem bleibt im Flugmodus registrierbar. |
| Update | Firmware-Update (OTA) im Web UI |

## Behobene Probleme der alten Firmware

**Position bleibt im Ausland am letzten Punkt in der Schweiz hängen.** Dafür gab es zwei Ursachen, beide sind behoben:

1. **GNSS-Filter in `FreematicsPlus`:** Die Bibliothek verwarf jede Position, die mehr als 0,1° (etwa 11 km) von der vorherigen abwich, und zwar dauerhaft. Nach einer längeren GNSS-Lücke (Gotthard- oder Lötschberg-Tunnel, Autoverlad, Fähre) wurden alle weiteren Positionen bis zum nächsten Neustart verworfen. Neu prüft `src/gnss.cpp`, ob die Position anhand von Zeit und Geschwindigkeit plausibel ist. Nach 60 s ohne Fix wird jede Position akzeptiert, ebenso drei in sich konsistente Fixes.
2. **Netzregistrierung im Roaming:** Das Modem wartete nur auf `AT+CREG` (CS-Domäne). Viele SIMs, vor allem IoT- und Daten-SIMs, registrieren sich im Ausland nur über LTE/EPS (`AT+CEREG`). Dann kam die Verbindung nie zustande. Neu wird eine Registrierung auf CS, PS oder EPS akzeptiert, ob im Heimnetz (1) oder im Roaming (5). Ist manuell ein Netz ausgewählt, wird auf automatische Netzwahl (`AT+COPS=0`) umgestellt. Die APN ist konfigurierbar.

**Langsamer Start.** Nach dem Aufwachen machte die alte Firmware einen kompletten Neustart: Modem-Boot, GNSS-Kaltstart und vollständige OBD-Protokollsuche. Neu:

- Aufwachen ohne Reboot.
- Zündungserkennung über die Bordspannung (≥ 13,2 V oder Anstieg ≥ 0,4 V) und Bewegung (0,15 g statt 0,4 g).
- Das zuletzt erkannte OBD-Protokoll wird gespeichert und direkt verwendet.
- GNSS bleibt im Standby optional eingeschaltet (Hot-Start).
- Das Modem wird nur in den Flugmodus geschaltet (`AT+CFUN=0`) und ist nach dem Aufwachen in wenigen Sekunden wieder im Netz.
- Ein Positionspunkt wird sofort beim Aufwachen gesendet.

## Installation

Voraussetzung: [PlatformIO](https://platformio.org).

```bash
pio run -e freematics_one_plus -t upload
```

Für Model B/H mit PSRAM gibt es eine grössere Warteschlange:

```bash
pio run -e freematics_one_plus_psram -t upload
```

Die serielle Konsole (115200 Baud) zeigt den Ablauf:

```bash
pio device monitor
```

Spätere Updates gehen über das Web UI (*System → Firmware-Update*) mit `.pio/build/<env>/firmware.bin`.

## Einrichtung

1. Gerät einstecken und Zündung einschalten.
2. Mit dem WLAN **`FREEMATICS-xxxx`** verbinden (Passwort `freematics`).
3. **http://192.168.4.1** öffnen. Im Heim-WLAN ist das Gerät auch unter `http://freematics.local` erreichbar.
4. Unter **Server** die URL eintragen und die **Geräte-ID** notieren. Ohne Eingabe gilt die automatische ID (`FM…`).
5. Unter **Netzwerk** Heim-WLAN, APN und SIM-PIN eintragen. Das Passwort des Access-Points und des Web UI sollte man ändern.
6. Speichern. Das Gerät startet neu.

Nach dem Einstecken bleibt das Gerät mindestens 10 Minuten wach, und solange ein Gerät mit dem Access-Point verbunden ist, geht es nie in den Standby. Ist das Heim-WLAN nicht erreichbar, sucht das Gerät nur 15 s danach und versucht es danach alle 2 Minuten erneut, aber nur, wenn niemand mit dem Access-Point verbunden ist. Die Suche würde sonst den Access-Point stören. Im Standby sind WLAN und Web UI aus, um die Batterie zu schonen. Zum Konfigurieren die Zündung einschalten oder das Fahrzeug bewegen.

### Traccar

- Gerät anlegen, *Identifier* = Geräte-ID aus dem Web UI.
- Server-URL: `http://<traccar-server>:5055/`
- Folgende Attribute kommen an:

| Attribut | Bedeutung |
|---|---|
| `ignition`, `motion` | Zündung, Bewegung (Traccar nutzt sie für Fahrten und Motorstunden) |
| `power` | Bordspannung in V |
| `rssi`, `sat`, `hdop` | Signal, Satelliten, Genauigkeit |
| `event` | `ignitionOn`, `ignitionOff`, `heartbeat`, `boot` |
| `vin`, `dtcs` | Fahrgestellnummer und Fehlercodes (bei Zündung an) |
| `deviceTemp` | Gerätetemperatur |
| OBD/CAN | frei benannt, Standard: `obdSpeed`, `rpm`, `engineLoad`, `throttle`, `coolantTemp`, `intakeTemp`, `fuel`, `ambientTemp`, `obdOdometer` |

### Home Assistant

- **Empfohlen:** Traccar als Zentrale, dazu die HA-Integration **Traccar Server**. Position und alle Attribute (OBD, CAN, Spannung, Zündung) stehen dann als Entitäten zur Verfügung.
- **Direkt:** Integration **Traccar Client** einrichten. Die angezeigte Webhook-URL als *Server-URL* eintragen, oder als *Live-Ziel*, wenn zusätzlich Traccar verwendet wird. HA bekommt so den Device-Tracker. Das Live-Ziel erhält nur die jeweils neueste Position, ohne nachgelieferte Punkte.

## CAN-Signale

Unter **CAN** werden Signale angelegt. Der **CAN-Monitor** zeigt bei laufendem Fahrzeug alle empfangenen IDs. Damit findet man Broadcast-Frames.

| Feld | Passiv | Anfrage |
|---|---|---|
| CAN-ID | ID des Broadcast-Frames | (nicht verwendet) |
| Sende-ID | – | Adresse des Steuergeräts, z. B. `7E0`, `7E4`, `18DA10F1` |
| Anfrage | – | Hex-Request, z. B. `22F40D` (UDS ReadDataByIdentifier) oder `015B` |
| Startbit / Bits | Position im Frame | Position in den Daten **nach** SID+DID |
| BE | Big-Endian: Bit 0 = MSB von Byte 0, Byte n → Startbit n·8 | gleich |
| ohne BE | Little-Endian (Intel, wie in DBC-Dateien): Byte n Bit b → n·8+b | gleich |
| Faktor / Offset | Wert = Rohwert × Faktor + Offset | gleich |

Beispiel: Ein 16-Bit-Wert in den Bytes 2–3 einer UDS-Antwort mit Skalierung 0,1 wird so eingetragen: *Anfrage*, Startbit `16`, Bits `16`, BE an, Faktor `0.1`. Welche DIDs ein Fahrzeug unterstützt, ist herstellerspezifisch.

Das passive Mitlesen braucht direkten Zugriff auf den CAN-Bus am OBD-Stecker. Viele neuere Fahrzeuge haben dort ein Gateway, das keine Broadcast-Frames durchlässt. UDS-Anfragen funktionieren in der Regel trotzdem.

## Projektstruktur

```
platformio.ini
src/
  main.cpp        Zustandsautomat: Fahrt / Zündung aus / Standby, Aufwachen, Punkte erzeugen
  settings.*      Konfiguration (JSON im NVS) inkl. Defaults
  gnss.*          GNSS (Empfänger oder Modem), Plausibilitätsfilter, Systemzeit
  vehicle.*       OBD-II-PIDs, CAN passiv/aktiv, CAN-Monitor, Spannung
  modem.*         SIMCOM-Treiber (Registrierung inkl. Roaming, APN, HTTP, GNSS, Zeit)
  uplink.*        WLAN/AP, Mobilfunk-Zustandsautomat, Upload aus der Warteschlange
  queue.*         Warteschlange RAM + LittleFS
  osmand.*        OsmAnd-Protokoll
  webui*.         Web-Server, REST-API, Oberfläche
  util.*          reine Hilfsfunktionen (Parser, Bits, Zeit), mit Unit-Tests
lib/FreematicsPlus  Hardware-Bibliothek (BLE und alte Netzwerkklassen entfernt, GNSS-Filter korrigiert)
test/             Host-Unit-Tests: sh test/run_tests.sh
```

### REST-API

| Methode | Pfad | Zweck |
|---|---|---|
| GET | `/api/status` | Live-Daten (JSON) |
| GET/POST | `/api/config` | Konfiguration lesen/schreiben (Teil-Updates möglich, danach Neustart) |
| POST | `/api/restart`, `/api/reset`, `/api/queue/clear` | Wartung |
| POST | `/api/can/monitor?seconds=30` | CAN-Monitor starten |
| GET | `/api/can/frames` | gesehene CAN-Frames |
| POST | `/api/update` | Firmware-Upload (multipart) |

## Hinweise und Grenzen

- Die Firmware kompiliert und linkt fehlerfrei gegen Arduino-ESP32 2.0.17, und die Unit-Tests laufen durch. **Auf echter Hardware ist sie noch nicht getestet.** Bitte zuerst mit der seriellen Konsole prüfen.
- SIM7070/SIM7080 können GNSS und LTE nicht gleichzeitig nutzen. Ohne separaten GNSS-Empfänger ist die Position mit diesen Modulen deshalb eingeschränkt.
- SIM5360 (3G) wird nicht mehr unterstützt.
- Die alte Konfiguration aus der BLE-App wird nicht übernommen.
