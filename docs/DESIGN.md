# ELRS ESP-NOW USB Dongle — техническое описание

## 1. Назначение

USB-донгл принимает телеметрию ExpressLRS от TX-бэкпака по ESP-NOW и отдаёт
её в смартфон по USB UART/USB CDC. Разбор CRSF и отображение остаются на
стороне Android или внешнего вьюера. Донгл — прозрачный транспорт.

```text
ELRS TX module
      │ UART
      ▼
TX backpack (ESP-NOW, channel 1, group MAC)
      │ ESP-NOW
      ▼
ESP32-C3 dongle ──USB──► Android
                            │
                      raw CRSF UDP 14550
                            │
                     CRSF viewer
```

## 2. Решения

| Вопрос | Решение |
|---|---|
| Формат выхода | CRSF в UDP `14550`; одна датаграмма — один кадр |
| MAVLink | не формируется |
| Android | отдельное приложение в `android-stub/` |
| Конфигурация | NVS + протокол `AA 55` по USB/UART |
| Внешний вьюер | [`voffmail/ELRS-backpack-telemetry`](https://github.com/voffmail/ELRS-backpack-telemetry) |
| Лицензия | MIT |

## 3. Варианты прошивки

| Прошивка | Плата | Данные |
|---|---|---|
| `esp32-c3-usb` | native-USB C3 | `Serial` (USB CDC) |
| `esp32-c3` | DevKitM-1/CP2102N | `Serial0` (UART0) |
| `esp32-c3-usb-lr` | native-USB C3, LR | `Serial` |
| `esp32-c3-lr` | DevKitM-1, LR | `Serial0` |

По умолчанию используется native USB C3 и PHY BGN. LR включается отдельными
env, если реальный бэкпак требует LR. Фраза/UID и канал хранятся в NVS и не
зависят от выбранного env.

## 4. ESP-NOW и binding

### 4.1. UID

Два режима:

- **phrase**: UID = первые 6 байт
  `MD5("-DMY_BINDING_PHRASE=\"<phrase>\"")`, младший бит первого байта
  очищен. Пустая phrase означает factory MAC.
- **manual UID**: ровно 6 байт; all-zero запрещён; LSB первого байта
  очищается как в ELRS. Phrase может быть пустой.

Донгл ставит UID на STA-интерфейс до старта ESP-NOW. Android и
`tools/dongle_config.py` читают и меняют режим, phrase/UID и канал через
конфигурационный протокол.

### 4.2. Канал и PHY

ExpressLRS Backpack использует **канал 1**. Донгл по умолчанию тоже на
канале 1. В Android-приложении канал не показывается и всегда применяется
`1`; для стенда/отладки канал задаётся через `tools/dongle_config.py`.

PHY по умолчанию — BGN (`11B|11G|11N`). LR-сборки добавляют
`WIFI_PROTOCOL_LR`. На стенде C3→C3 LR показывал потери, BGN — ноль.

### 4.3. Приём

- Wi-Fi STA без подключения к AP.
- `esp_wifi_stop` → `esp_wifi_set_mac` → `esp_wifi_start`.
- `esp_now_init`, `esp_now_register_recv_cb`.
- Callback только копирует кадр в очередь; запись в USB — в `loop`.
- При переполнении очереди отбрасывается самый старый кадр.

## 5. USB и поток данных

### 5.1. Порты

- `esp32-c3-usb`: данные и конфиг в `Serial` (native USB CDC).
- `esp32-c3`: данные и конфиг в `Serial0` (UART0 → CP2102N).
- Скорость UART — 115200 8N1.
- `CORE_DEBUG_LEVEL=0`; в поток данных ничего не печатается.

### 5.2. Формат

Донгл отдаёт сырой ESP-NOW payload. Обычно это MSP v2-обёртка `$X<` + CRSF.
Android сканирует поток на CRSF-адреса `0xC8/0xEA/0xEE`, проверяет длину и
CRC8, отбрасывает обёртку и отправляет каждый валидный CRSF-кадр отдельной
UDP-датаграммой.

## 6. Конфигурационный протокол

Кадр:

```text
AA 55 | ver=1 | type | len LE16 | payload | CRC16 LE16
```

CRC16-CCITT-FALSE по `ver..payload`. Команды:

- `GET_CONFIG` — channel, UID, phrase, version, uidMode;
- `SET_CONFIG` — channel + phrase (phrase-режим; пустая phrase разрешена);
- `SET_UID` — channel + 6-байтовый UID (manual-режим);
- `GET_STATS` — счётчики, Wi-Fi-канал, host, link stats;
- `RESET`, `PING`.

Фраза/UID и канал сохраняются в NVS. Factory MAC используется только как
boot-fallback, когда UID не настроен.

## 7. Android-приложение

- USB host + `usb-serial-for-android` (CP210x/CDC ACM), без root.
- При открытии приложение само подключается к найденному USB-донглу.
- Конфиг читается автоматически; отдельной кнопки Read config нет.
- Phrase/Manual UID — переключатель; редактируется только одно поле.
- Канал скрыт и всегда равен 1.
- Target — одно поле `host:port`: по умолчанию `localhost:14550`, пусто =
  broadcast, порт по умолчанию 14550. Неверные значения не сохраняются.
- `localhost` отправляется через loopback без привязки к Wi-Fi; LAN-адреса —
  через Wi-Fi. VPN-сети при выборе Wi-Fi пропускаются.
- Foreground service + `PARTIAL_WAKE_LOCK` + Wi-Fi lock.
- Уведомление тихое (LOW, без звука/вибрации), обновляется только при смене
  состояния/target; счётчики — в UI.

## 8. Индикация

- Native USB C3: синий LED GPIO8, active-low, управление `digitalWrite`.
- DevKitM-1: WS2812 на GPIO8, синий цвет.

`LinkStatus` не зависит от протокола:

```cpp
struct LinkStatus {
    bool known;
    bool up;
    uint8_t quality;
    uint32_t lastMs;
    LinkSource source; // None, Crsf, Mavlink
};
```

Сейчас его заполняет CRSF-парсер (`0x14`); позже сможет заполнять
MAVLink-парсер. Состояния LED:

- off — нет UID, нет USB-хоста или нет пакетов >3 с;
- solid — пакеты идут, но линка/телеметрии нет (включая LQ=0);
- blink 200/200 — пакеты и `LinkStatus.up`;
- 3 импульса / 2 с — ошибка ESP-NOW или queue drops.

Таймаут потери линка — 0.5 с.

## 9. Симулятор и стенд

`sim/` имитирует Betaflight и отдаёт полный набор CRSF-кадров: GPS, GPS time,
GPS extended, Vario, Battery, Baro, Link, Attitude, Flight mode, Device info
и sequence. GPS движется по кругу (радиус/скорость — build flags), ~46
пакетов/с.

Стенд без телефона: `sim` → донгл → USB → `tools/serial_bridge.py` → UDP
`14550` → внешний вьюер. Для стенда можно использовать канал 11, чтобы уйти
от загруженного канала 1.

## 10. Web Flasher и CI

- `webflasher/` — страница ESP Web Tools (Web Serial).
- Прошивается только `esp32-c3-usb`; образ — merged binary с offset 0.
- `scripts/make_webflasher.py` собирает Pages-сайт: index, manifest,
  merged-образ, SHA256SUMS.
- `.github/workflows/webflasher.yml` на GitHub Release собирает образ,
  деплоит GitHub Pages и прикладывает assets к релизу.
- Pages включается один раз: Settings → Pages → Source: GitHub Actions.

Другие workflows: сборка прошивки и APK, тесты.

## 11. Риски и ограничения

1. **Model TX-бэкпака/PHY**: может понадобиться LR-сборка.
2. **Binding phrase/UID**: неверные значения → тишина; нужна диагностика
   счётчиков и LED.
3. **Канал 1**: Wi-Fi AP на том же канале может влиять на приём.
4. **Native-USB reset**: открытие CDC-порта может один раз перезагрузить
   плату; приложение и мост переживают reconnect.
5. **Broadcast**: может блокироваться client isolation; в поле нужен
   unicast.
6. **Питание**: телефон не заряжается при OTG; учитывать ток и нагрев.
7. **MAVLink**: в v1 не поддерживается; для QGC нужен отдельный мост.
8. **Web Serial**: только desktop Chrome/Edge; fallback — PlatformIO/esptool.

## 12. Лицензия

Новый код — MIT. Сторонние компоненты и лицензии —
в [THIRD_PARTY.md](../THIRD_PARTY.md).
