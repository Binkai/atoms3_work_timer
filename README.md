# M5Stack AtomS3 Work-Tracker Cube

Dieses Projekt verwandelt einen **M5Stack AtomS3** in einen Arbeitszeit + Projektzeit-Tracker. Durch einfaches Betätigen des Geräts werden Webhooks (z. B. an Google Apps Script) gesendet, um Projektzeiten zu erfassen.

## Features
- **Setup-Modus:** Integrierter Webserver zur Konfiguration von WiFi und API-Tokens ohne Neu-Kompilierung.
- **Visuals:** Anzeige von Kunden-Icons auf dem 128x128 Display.
- **Flexibilität:** Nutzt `WiFiMulti` für verschiedene Standorte (Home Office/Büro).

## Hardware
- M5Stack AtomS3 (ESP32-S3)

## Installation & Konfiguration
1. Benenne `secrets_example.h` in `secrets.h` um und trage deine Daten + Webhook ein.
2. Füge deine Icons in die `images.h` ein (Beispiele findest du in `images_example.h`).
3. Lade den Sketch über die Arduino IDE hoch.
4. Halte beim Start den Button gedrückt, um in den Setup-Modus zu gelangen (IP: `192.168.4.1`).
