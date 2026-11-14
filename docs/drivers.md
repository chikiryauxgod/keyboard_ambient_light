# Драйверы и совместимость

Прямые драйверы используют HIDAPI и входят в portable-приложение для Linux,
Windows и macOS. Отдельный RGB-сервис для них не нужен. Совпадение марки само
по себе не означает совместимость: проверяются модель и USB-интерфейс.

Все драйверы сейчас задают один цвет всей клавиатуре. Поклавишная геометрия
и несколько устройств одновременно пока не реализованы.

| Драйвер | Профили | Подключение | После Stop |
|---|---|---|---|
| VIA | Standard VIA 12/13, RGB Matrix (канал 3) или RGB Light (канал 2) | HID; радио зависит от прошивки приёмника | Восстанавливает прочитанные эффект, цвет и яркость |
| VialRGB | Protocol 1, прошивка с эффектом Solid Color (ID 2) | QMK/Vial Raw HID | Восстанавливает режим, скорость, HSV |
| Logitech | G213, G410, G512, G513, G810, G910, G Pro — IDs ниже | USB, интерфейс 1 | Оставляет последний цвет; исходный эффект не читается |
| Razer | BlackWidow Chroma, Chroma TE, X Chroma, X Chroma TE | USB, интерфейс 1 | Восстанавливает режим устройства и яркость; прежняя анимация не читается |
| Corsair | K65 RGB/LUX RGB/RGB RAPIDFIRE, K70 RGB, K95 RGB — legacy protocol | USB, интерфейс 1, usage page `ffc2` | Возвращает аппаратную подсветку; прежние программные цвета не читаются |
| OpenRGB | Контроллеры сервера с Direct mode и адресуемыми цветами | TCP SDK | Восстанавливает прочитанные цвета и режим |

Из перечисленного аппаратно подтверждена только Epomaker Luma100 `342d:e506`
через VIA и приёмник 2.4 ГГц. Новые VialRGB/Logitech/Razer/Corsair-драйверы
экспериментальные: тесты проверяют пакеты и жизненный цикл на fake HIDAPI,
но не заменяют проверку настоящего устройства на каждой ОС.

VIA и VialRGB отображаются как кандидаты по Raw HID-интерфейсу. Версия протокола
и поддержка нужных функций проверяются при подключении. Наличие VIA не означает
наличие VialRGB. Неподходящий профиль вернёт ошибку до изменения подсветки.

## Точные USB-профили

| VID | PID | Модель |
|---|---|---|
| `046d` | `c336` | Logitech G213 |
| `046d` | `c330` | Logitech G410 |
| `046d` | `c342` | Logitech G512 |
| `046d` | `c33c` | Logitech G513 |
| `046d` | `c331`, `c337` | Logitech G810 |
| `046d` | `c32b`, `c335` | Logitech G910 |
| `046d` | `c339` | Logitech G Pro |
| `1532` | `0203` | Razer BlackWidow Chroma |
| `1532` | `0209` | Razer BlackWidow Chroma TE |
| `1532` | `0216` | Razer BlackWidow X Chroma |
| `1532` | `021a` | Razer BlackWidow X Chroma TE |
| `1b1c` | `1b17` | Corsair K65 RGB |
| `1b1c` | `1b37` | Corsair K65 LUX RGB |
| `1b1c` | `1b39` | Corsair K65 RGB RAPIDFIRE |
| `1b1c` | `1b13` | Corsair K70 RGB |
| `1b1c` | `1b11` | Corsair K95 RGB |

Bluetooth и фирменные радиоприёмники новых трёх драйверов не поддерживаются.
G815 исключена: исследованный вариант команды использует флаг сохранения;
приложение не должно записывать постоянный профиль на каждом кадре.
Одноцветные Logitech G413/G610 и новые Corsair MK2/Platinum/V2 также не включены.

На Linux доступ к выбранному HID-интерфейсу можно настроить правилом из
Connections → Export USB access rule. Одновременно управлять одной клавиатурой
через несколько RGB-приложений не следует: ответы и эффекты могут конфликтовать.

## Спецификации протоколов

Реализации написаны отдельно по формату сообщений и идентификаторам из первичных
источников; внешние OpenRazer, ckb-next и OpenRGB для прямых драйверов не запускаются.

- [VialRGB: описание](https://get.vial.today/docs/lighting.html), [обработка протокола](https://github.com/vial-kb/vial-qmk/blob/vial/quantum/vialrgb.c).
- [Logitech: сообщения](https://github.com/MatMoul/g810-led/blob/master/src/classes/Keyboard.cpp), [идентификаторы](https://github.com/MatMoul/g810-led/blob/master/src/classes/Keyboard.h).
- [Razer: сообщения](https://github.com/openrazer/openrazer/blob/master/driver/razerchromacommon.c), [транспорт](https://github.com/openrazer/openrazer/blob/master/driver/razercommon.c), [профили](https://github.com/openrazer/openrazer/blob/master/driver/razerkbd_driver.h).
- [Corsair: legacy USB](https://github.com/CalcProgrammer1/OpenRGB/tree/master/Controllers/CorsairPeripheralController).
