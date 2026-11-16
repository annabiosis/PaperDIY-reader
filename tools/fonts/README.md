# Шрифты и лицензии

В прошивку встроены растровые атласы, сгенерированные из двух семейств:

| Семейство | Роли | Файлы | Лицензия |
|---|---|---|---|
| **PT Serif** | текст книг (Regular / Bold / Italic / Bold Italic) | `src/PTF55F.ttf`, `PTF75F.ttf`, `PTF56F.ttf`, `PTF76F.ttf` | ParaType Free Font License v1.3 |
| **JetBrains Mono** | интерфейс: файловый менеджер, статусные строки, заголовки, проценты (Regular / Bold) | `src/JetBrainsMono-Regular.ttf`, `JetBrainsMono-Bold.ttf` | SIL Open Font License 1.1 (`licenses/JetBrainsMono-OFL.txt`) |

Правило оформления: **засечки только в содержимом книг**, весь служебный интерфейс — моноширинный.

## Лицензия ParaType Free Font License

PT Serif распространяется по ParaType Free Font License v1.3: файлы шрифтов
можно свободно использовать, включая встраивание в документы и программы,
при условии, что сам шрифт не продаётся отдельно и в сопроводительной
документации указано его происхождение. Полный текст лицензии доступен на
сайте ParaType: <https://www.paratype.com/fonts/pt/pt-serif> (раздел License).
Copyright © 2010 ParaType Ltd.

## SIL Open Font License 1.1

JetBrains Mono распространяется по SIL OFL 1.1; текст лицензии лежит в
`licenses/JetBrainsMono-OFL.txt`. Copyright 2020 The JetBrains Mono Project
Authors (<https://github.com/JetBrains/JetBrainsMono>).

## Как пересобрать атласы

```powershell
python tools/fonts/gen_fonts.py
```

Скрипт читает TTF из `tools/fonts/src`, рендерит по 532 глифа на начертание
(ASCII, Latin-1, кириллица, типографика, стрелки, рамки) в 8-битные альфа-маски
и складывает результаты в `firmware/main/text/atlas/`. Размер атласов —
около 3.2 МБ, они лежат во flash и не занимают RAM.

Специально **не** включаются: мягкий перенос (U+00AD), ZWSP/ZWNJ/ZWJ, BOM —
движок текста отбрасывает их до растеризации, поэтому переносы не выглядят
как «?».
