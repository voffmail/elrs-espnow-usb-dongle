# ELRS ESP-NOW USB Dongle

Приём телеметрии ExpressLRS от TX-бэкпака по ESP-NOW на ESP32-C3 и передача
сырого CRSF-потока в смартфон по USB. Android-приложение выделяет CRSF-кадры и
отправляет их по UDP `14550` во внешний CRSF-вьюер. Донгл не формирует MAVLink
и не занимается отображением.

```text
ELRS TX module ──► TX backpack ──ESP-NOW──► ESP32-C3 dongle ──USB──► Android
                                                                     │
                                                       raw CRSF UDP 14550
                                                                     │
                                                            CRSF viewer
```

## Web Flasher

Прошить native-USB C3 можно прямо из браузера:

**https://voffmail.github.io/elrs-espnow-usb-dongle/**

- Только вариант **`esp32-c3-usb`** (ESP32-C3 с native USB). Платы
  DevKitM-1/CP2102N (`esp32-c3`) через эту страницу не прошиваются.
- Нужен **Chrome или Edge на компьютере**; страница должна быть открыта по
  HTTPS или на `localhost`. Мобильные браузеры Web Serial не поддерживают.
- Прошивка стирает NVS: phrase/UID и target нужно задать заново после
  прошивки.
- Перед прошивкой закрой приложения и мониторы, которые держат последовательный
  порт.

Кратко: подключи донгл по USB → открой ссылку в Chrome/Edge → **«Прошить
донгл»** → выбери порт → подтверди стирание → дождись перезагрузки → настрой
донгл в Android-приложении или через `tools/dongle_config.py`.

Релизный workflow собирает merged-образ, публикует его на GitHub Pages и
прикладывает к GitHub Release `firmware-merged.bin`, `SHA256SUMS` и
`firmware.elf`. Android-workflow прикладывает к тому же релизу
`elrs-dongle-stub.apk`.

## Установка Android-приложения

После прошивки донгла установи приложение на смартфон. Готовый APK лежит в
последнем GitHub Release:

**https://github.com/voffmail/elrs-espnow-usb-dongle/releases/latest**

Со смартфона:

1. Открой ссылку в браузере телефона.
2. В разделе **Assets** скачай `elrs-dongle-stub.apk`.
3. Если Android попросит, разреши установку из браузера/неизвестного
   источника.
4. Открой скачанный APK и установи приложение.
5. Подключи донгл через USB-OTG, открой приложение и разреши доступ к
   USB-устройству.

Прямая ссылка на APK:

**https://github.com/voffmail/elrs-espnow-usb-dongle/releases/latest/download/elrs-dongle-stub.apk**

Нужен Android 7.0+ с поддержкой USB Host (OTG).

## Возможности

- Приём ESP-NOW и передача сырых payload'ов без разбора CRSF.
- Binding phrase **или ручной 6-байтовый UID** и канал в NVS.
- Автоматическое чтение конфига при подключении Android-приложения.
- Защита от пустого/нулевого UID; пустая phrase означает factory MAC.
- Варианты native-USB C3 и DevKitM-1/CP2102N; опционально 802.11 LR.
- Индикация состояния линка синим LED.
- Android: unicast `host:port`, пустой target = broadcast, тихое
  foreground-уведомление.
- Симулятор TX-бэкпака для стенда без реальной аппаратуры.
- Совместимость с внешним вьюером
  [`voffmail/ELRS-backpack-telemetry`](https://github.com/voffmail/ELRS-backpack-telemetry).

## Железо и варианты прошивки

**По умолчанию — `esp32-c3-usb`**: native USB C3, без пайки, используется
штатный USB-разъём платы.

| Прошивка | Плата | Данные | USB на ПК |
|---|---|---|---|
| `esp32-c3-usb` | native-USB C3 | `Serial` (USB CDC) | `/dev/ttyACM*`, VID:PID `303A:1001` |
| `esp32-c3` | DevKitM-1 с CP2102N | `Serial0` (UART0) | `/dev/ttyUSB*`, VID:PID `10C4:EA60` |
| `esp32-c3-usb-lr` | native-USB C3 + LR | `Serial` | `/dev/ttyACM*` |
| `esp32-c3-lr` | DevKitM-1 + LR | `Serial0` | `/dev/ttyUSB*` |

- `esp32-c3` оставлен для совместимости с DevKitM-1; основным не считается.
- LR-сборки нужны только если реальный бэкпак требует LR.
- Фраза и канал в NVS **не зависят от env**. Перепрошивка другого варианта их
  не стирает (если не делать erase).
- Android не выбирает env: он открывает USB-serial, который видит телефон.

### Канал

ExpressLRS Backpack работает на **канале 1**. Донгл по умолчанию тоже на
канале 1. Поле канала в Android-приложении убрано; при применении конфига
всегда используется `1`. Для стенда/отладки канал можно задать через
`tools/dongle_config.py`.

## Сборка

```bash
./init.sh
./tools/pio run -d firmware -e esp32-c3-usb    # основной вариант, BGN
./tools/pio run -d firmware -e esp32-c3        # DevKitM-1, UART0, BGN
./tools/pio run -d sim -e esp32-c3             # симулятор бэкпака, BGN

# опционально LR
./tools/pio run -d firmware -e esp32-c3-usb-lr
./tools/pio run -d firmware -e esp32-c3-lr
./tools/pio run -d sim -e esp32-c3-lr
```

`init.sh` создаёт локальный `.toolchain/venv` с PlatformIO 6.2.0 и не пишет в
`~/.platformio`.

Прошивка через USB (порт брать из `/dev/serial/by-id/` или
`./tools/pio device list`):

```bash
./tools/pio run -d firmware -e esp32-c3-usb -t upload --upload-port <port>
./tools/pio run -d sim -e esp32-c3 -t upload --upload-port <port>
```

Не использовать `cat` для чтения serial: DTR/RTS могут удерживать плату в
reset. Инструменты проекта используют `tools/serial_port.py` и не дёргают
DTR/RTS.

## Runtime-конфигурация

Фраза/UID и канал хранятся в NVS и применяются без перепрошивки.

```bash
# показать текущий конфиг и UID
python3 tools/dongle_config.py --port <port> --show

# задать фразу (фраза не попадает в репозиторий)
ELRS_BINDING_PHRASE='<phrase>' python3 tools/dongle_config.py --port <port> --channel 1

# либо задать ручной 6-байтовый UID
python3 tools/dongle_config.py --port <port> --uid AA:BB:CC:DD:EE:FF --channel 1

# счётчики, ping, сброс
python3 tools/dongle_config.py --port <port> --stats
python3 tools/dongle_config.py --port <port> --ping
python3 tools/dongle_config.py --port <port> --reset
```

Режимы binding:

- `phrase`: UID = MD5(`-DMY_BINDING_PHRASE="<phrase>"`). Пустая phrase —
  factory MAC.
- `manual UID`: ровно 6 байт, не all-zero; LSB первого байта очищается как
  в ELRS. Phrase может быть пустой.
- All-zero UID отклоняется. Factory MAC используется только как
  boot-fallback, а не сохраняемый UID.
- Android при подключении автоматически делает `GET_CONFIG` и заполняет
  mode/phrase/UID.

Протокол: `AA 55 | ver | type | len LE | payload | CRC16`. Команды:
`GET_CONFIG`, `SET_CONFIG` (phrase), `SET_UID` (manual), `GET_STATS`,
`RESET`, `PING`.

## Android-приложение

Сборка (нужны JDK 21 и Android SDK 35):

```bash
cd android-stub
./gradlew :app:assembleDebug
```

APK: `android-stub/app/build/outputs/apk/debug/app-debug.apk`.

Установка по Wi-Fi ADB (порт Wireless debugging ищется через mDNS, не 5555):

```bash
ADB=adb
$ADB connect <phone-ip>:<debug-port>
$ADB install -r android-stub/app/build/outputs/apk/debug/app-debug.apk
```

В приложении:

- phrase/UID заполняются из донгла; редактируется только одно поле в
  зависимости от режима;
- канал не показывается и всегда равен 1;
- target — одно поле `host:port`; по умолчанию `localhost:14550`,
  пусто = broadcast, порт по умолчанию 14550;
- неверный UID/host/port не сохраняется;
- одна кнопка «Запустить/Остановить телеметрию»; автоподключение при открытии;
- тихое foreground-уведомление, без счётчиков и без обновления каждую секунду.

## Индикация LED

На native-USB C3 используется синий LED на **GPIO8**, активный низкий уровень.
На DevKitM-1 это WS2812 RGB; цвет — синий.

| LED | Условие |
|---|---|
| **Не горит** | Нет UID, USB-хост/приложение не открыло порт, или нет ESP-NOW-пакетов дольше 3 с |
| **Горит ровно** | Порт открыт, ESP-NOW готов, пакеты идут, но телеметрии/линка нет |
| **Мигает 200/200 мс** | Пакеты идут и `LinkStatus.up = true` |
| **3 импульса раз в 2 с** | ESP-NOW не стартовал или были queue drops |

`LinkStatus` не зависит от протокола: сейчас его заполняет CRSF-парсер
(`0x14`), позже сможет заполнять MAVLink-парсер. Пакеты `0x14` с `LQ=0`
дают ровный свет; отсутствие пакетов >3 с гасит LED. Таймаут потери линка —
0.5 с.

## Стенд без смартфона

```text
C3 №1 (sim) ──ESP-NOW──► C3 №2 (dongle) ──USB CDC──► PC
                                                         │
                                         tools/serial_bridge.py
                                                         │ UDP 14550
                                                  CRSF viewer
```

1. Прошить `sim` и `esp32-c3-usb` на разные платы.
2. Настроить обе платы одной фразой/UID и каналом:
   ```bash
   ELRS_BINDING_PHRASE='<phrase>' python3 tools/dongle_config.py --port <sim-port> --channel 11
   ELRS_BINDING_PHRASE='<phrase>' python3 tools/dongle_config.py --port <dongle-port> --channel 11
   ```
   Канал 11 — только для стенда; в поле канал 1.
3. Запустить мост и вьюер:
   ```bash
   python3 tools/serial_bridge.py --serial <dongle-port> --udp 127.0.0.1:14550
   ```
4. Проверить live-данные во вьюере и `crc_err`, `seq_lost`, `drops` в логе
   моста.

## Симулятор бэкпака

`sim/` имитирует Betaflight и отдаёт полный набор CRSF-кадров:

- `0x02` GPS, `0x03` GPS time, `0x06` GPS extended;
- `0x07` Vario, `0x08` Battery, `0x09` Baro;
- `0x14` Link statistics, `0x1E` Attitude, `0x21` Flight mode;
- `0x29` Device info и `0x7F` sequence для измерения потерь.

GPS движется по кругу (радиус 120 м, 18 км/ч, 10 Гц), высота и vario
меняются, спутники 10–12, fix 3D. Координаты — build flags:

```ini
-DSIM_GPS_LAT=55.753930
-DSIM_GPS_LON=37.620795
-DSIM_GPS_RADIUS_M=120.0
-DSIM_GPS_SPEED_KMH=18.0
```

Итого ~46 пакетов/с.

## Проверки

```bash
python3 -m py_compile tools/*.py
python3 -m unittest discover -s tests
```

CI:

- `.github/workflows/firmware.yml` — сборка `esp32-c3`, `esp32-c3-usb`,
  `esp32-c3-usb-lr`, `sim`, `sim -e esp32-c3-lr`, тесты;
- `.github/workflows/android.yml` — JDK 21, SDK 35, `assembleDebug`, APK;
- `.github/workflows/webflasher.yml` — release: сборка `esp32-c3-usb`,
  merged-образ, GitHub Pages и release assets.

Для публикации страницы один раз включи GitHub Pages:
**Settings → Pages → Source: GitHub Actions**.

## Ограничения

- Донгл отдаёт CRSF; MAVLink не формируется. Для QGC нужен отдельный мост.
- Открытие native-USB порта может один раз перезагрузить плату; приложение и
  мост переживают переподключение.
- UART0/CP2102N-путь проверяется на DevKitM-1; основной стенд — native USB C3.
- Broadcast может блокироваться client isolation; для поля используйте
  unicast `host:port`.
- Web Flasher требует Chrome/Edge desktop и исправный USB-кабель; fallback —
  PlatformIO/esptool.
- Исходный код вьюера не входит в этот репозиторий.

## Лицензия

MIT. См. [LICENSE](LICENSE) и [THIRD_PARTY.md](THIRD_PARTY.md).
