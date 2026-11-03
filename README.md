# burn

Пакетный менеджер [CleanLinux](https://github.com/vovawees/CleanLinux), написанный на C: сборка из исходников
под твоё железо, rolling release+++ (сначала собрать всё, потом применить; ABI-пересборка; откат), установка
в любой корень, cross-сборка.

> **Статус:** burn CleanLinux — это он: bootstrap, тесты и образ используют C-версию, пакет `burn` входит в `base`.
> Все команды bash-прототипа ([тег `burn-prototype`](https://github.com/vovawees/CleanLinux/tree/burn-prototype/burn))
> перенесены; рецепты, `burn.conf`, профили, база установленных пакетов и формат `.pkg.tar.zst` остались теми же.
> C-версия сверяется с прототипом на всех рецептах [CleanLinux-Recipes](https://github.com/vovawees/CleanLinux-Recipes)
> и на тестовых сборках, а настоящая base (glibc, bash, coreutils, util-linux, …) собирается ею байт в байт так же.

## Зачем C

- **Скорость.** Рецепты разбирает свой парсер, без запуска bash: `search`, `plan` и порядок зависимостей по всему
  репозиторию — за миллисекунды. tar, zstd, sha256 и разбор ELF работают внутри процесса, без конвейеров
  `tar | zstd` и без `readelf` и `sha256sum` на каждый файл.
- **Надёжность.** Установка атомарна: каждый файл пишется рядом под временным именем и встаёт на место через
  `rename()`, так что обновление glibc или bash на живой системе не оставляет полузаписанных файлов. Пути пакета
  никогда не выходят за пределы корня.
- **Цельность.** Один бинарник, зависимости — только libc и libzstd. bash нужен лишь для команд сборки внутри
  рецептов.

## Сборка

```sh
make
make check               # RECIPES=../CleanLinux-Recipes/packages: ещё и сверка всех рецептов с bash
make proto               # каждая команда против bash-прототипа (рядом ../CleanLinux-prototype и ../CleanLinux-Recipes)
make check-btrfs         # снимки на настоящей btrfs (loop-устройство; KERNEL=bzImage — в qemu)
sudo make install        # PREFIX=/usr DESTDIR=: burn и пример burn.conf
make dist                # burn-VERSION.tar.gz — его собирает рецепт burn
```

Нужны компилятор C17 (GCC или Clang), glibc и libzstd. Для `make proto` ещё bash, GNU tar, zstd, patch, unzip,
python3, autoconf, cmake, meson и ninja: тестовые рецепты собираются обеими версиями. Прототип — CleanLinux на теге
`burn-prototype`:

```sh
git clone -q --depth 1 --branch burn-prototype https://github.com/vovawees/CleanLinux ../CleanLinux-prototype
```

На устройстве burn нужны ещё bash (функции рецептов), git (`sync`), curl или wget (`fetch`) и инструменты сборки.
Профили железа ищутся в `BURN_PROFILE_DIR`, `/etc/burn/profiles` и `/usr/share/burn/profiles` (их ставит CleanLinux).

## Статус переноса

| Часть | Команды | Статус |
|---|---|---|
| основа: вывод, строки, файлы, процессы, SHA-256, сравнение версий | `vercmp` | ✅ |
| парсер языка рецептов (сверка с bash на всех рецептах) | — | ✅ |
| рецепты, особенности, зависимости | `info`, `query`, `search`, `lint` | ✅ |
| конфиг, профили, цель и CPU, определение железа | `profile` | ✅ |
| база пакетов | `list`, `files`, `owner`, `hold`, `unhold`, `history`, `doctor` | ✅ |
| пакеты: tar + zstd в процессе, атомарная установка, удаление, откат, сохранённые библиотеки, триггеры | `install <файл>`, `remove`, `rollback` | ✅ |
| источники: параллельная загрузка, sha256 внутри процесса | `fetch`, `checksum` | ✅ |
| сборка: флаги пакета, типы сборки в C (bash — только для функций рецепта), чистка, ABI-пересборка | `unpack`, `build`, `install <рецепт>` | ✅ |
| rolling+++: репозитории рецептов, план с рисками и списком изменений, «сначала собрать всё, потом применить», хуки | `sync`, `plan`, `update` | ✅ |
| снимок btrfs перед обновлением через ioctl (без btrfs-progs; у прототипа был хуком) | `snapshot` | ✅ |

C-версия и прототип работают с одной и той же базой и одним кэшем пакетов, так что их можно чередовать.

Устройство, форматы и решения — в [docs/DESIGN.md](docs/DESIGN.md).

## Лицензия

MIT
