# 🚀 Rymdlist

Fristående ESP32-firmware som driver en **WS2812B-ledlist** och låter vem som helst
på samma WiFi justera ljuset live från mobilen — via en webbsida som ESP32:n
**själv serverar**. Ingen extern server, ingen molntjänst, ingen MQTT-broker.

Två telefoner ser samma sak samtidigt (state speglas över WebSocket), och senaste
inställningen sparas i flashminnet (NVS) så listen ser likadan ut efter en omstart.

---

## Vad den gör

**Effekter** (välj i UI:t): `Fast sken` · `Av` · `Puls` (andning) · `Lugn` (långsam våg)
· `Alarm` (snabb röd blink) · `Regnbåge` (rullande).
**Ljusstyrka** och **hastighet** gäller alla effekter. Färgväljaren gäller alla
utom Alarm (alltid röd) och Regnbåge (egen färgcykel).

---

## Hårdvara & koppling

| ESP32           | WS2812B-list        |
|-----------------|---------------------|
| `LED_PIN` (GPIO 5) | **DIN** (datain)  |
| **GND**         | **GND**  ← måste vara gemensam! |
| —               | **+5V** matas separat (se nedan) |

**Ström — läs detta för längre lister:**

- WS2812B drar upp till **~60 mA per pixel** vid full vit. 60 pixlar ≈ **3,6 A** i teoretiskt max.
- Mata listen från en **5V-nätadapter**, inte från ESP32:ns 5V-pinne (den orkar bara någon enstaka amp).
- **Gemensam GND** mellan ESP32 och listen är ett krav — annars flimrar/fungerar inget.
- Sätt en **300–500 Ω resistor** i serie på datalinjen och en **1000 µF-kondensator** över 5V/GND vid listens början (dämpar spikar). Rekommenderas men inte tvingande för en kort test-list.
- Firmwaren har ett **strömtak** (`MAX_MA` / `VOLTS` i `config.h`) — FastLED dämpar automatiskt ljuset så att en klen adapter inte överbelastas. Sätt `MAX_MA` lägre än vad din adapter klarar, med marginal.
- För lister > ~150 pixlar: mata in 5V på flera ställen längs listen.

---

## Kom igång

### 1. Fyll i config

```bash
cp src/config.h.example src/config.h
```

Öppna `src/config.h` och sätt minst:

```c
#define WIFI_SSID  "DittWiFi"
#define WIFI_PASS  "DittLösenord"
#define NUM_LEDS   60          // antal pixlar i din list
#define LED_PIN    5           // GPIO till DIN
```

`src/config.h` är gitignore:ad — dina WiFi-uppgifter checkas aldrig in.

### 2. Flasha

Med [PlatformIO](https://platformio.org/) installerat (VS Code-tillägget eller `pio` i terminalen):

```bash
pio run --target upload      # bygg + flasha över USB
pio device monitor           # se serieutskrift (115200 baud)
```

Byt kort genom att ändra `default_envs` / board i `platformio.ini` (t.ex. ESP32-C3).

### 3. Öppna UI:t

Vid boot skriver ESP32:n ut IP och namn på serieporten. Öppna sedan på mobilen
(samma WiFi):

```
http://rymdlist.local
```

Funkar inte `.local` (vissa Android-enheter saknar mDNS)? Använd IP-adressen som
står i serieutskriften, t.ex. `http://192.168.1.42`.

---

## Om WiFi inte nås (AP-fallback)

Får ESP32:n **inte** kontakt inom ~15 sekunder startar den ett **eget öppet
accesspunkt-nät** i stället:

- SSID: **`Rymdlist-setup`**
- Anslut telefonen till det nätet och öppna **`http://192.168.4.1`**

Samma UI serveras där. Det gör att du kommer in och styr listen även på ett nät
som isolerar klienter från varandra, eller innan WiFi är ifyllt.

---

## Så hänger det ihop (för vidareutveckling)

- **State-kontraktet** (`struct State { effect, color, brightness, speed }`) är den
  enda sanningen. Fältnamnen är avsiktligt stabila — samma namn ska användas av
  framtida Blocks/MQTT-styrning.
- **`applyState(State)`** är den *enda* vägen in. Webb-UI:t, NVS-återläsningen vid
  boot och (framöver) en MQTT-klient går alla genom den. Effektmotorn och alla
  anslutna telefoner speglar automatiskt varje ändring.
- **Effektmotorn** är non-blocking och ritar i `loop()` med fast bildfrekvens
  (FastLED `beat`-funktioner → hastighet styrs av `speed`).

### Framtida MQTT (kroken finns, aktiverad senare)

Sömmen är redan förberedd: en MQTT-klient (PubSubClient) behöver bara mata payloaden
`{effect,color,brightness,speed}` till `applyStateFromJson()` — samma tratt som
webben använder. Se den kommenterade **MQTT-KROK**-blocket längst ner i
[`src/main.cpp`](src/main.cpp). Ingen broker-kod körs i nuläget.

Tänkt topic: `rymdbasen/<zon>/led`. Bekräfta topic-konventionen mot Rymdbasen/Blocks
innan hopkoppling (dagens faktiska mönster i huset är `station<N>/<signal>`) — men
håll `effect/color/brightness/speed` stabila oavsett transport.

---

## Filer

```
rymdlist/
├── platformio.ini        # board, bibliotek (FastLED, ArduinoJson, ESPAsyncWebServer)
├── src/
│   ├── main.cpp          # all firmware: state, applyState, effektmotor, webb, nät, MQTT-krok
│   ├── config.h.example  # mall — kopiera till config.h
│   └── config.h          # din lokala config (gitignore:ad)
└── README.md
```
