# Как участвовать

**burn** — пакетный менеджер CleanLinux на C, часть [CleanLinux](https://github.com/vovawees/CleanLinux). *English [below](#english).*

## С чего начать

- Что входит в базу, а что ставится сверху — [SCOPE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/SCOPE.md); как всё устроено — [ARCHITECTURE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ARCHITECTURE.md).
- Что дальше — [ROADMAP.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md), issues с меткой `roadmap` и общая доска [CleanLinux roadmap](https://github.com/users/vovawees/projects/1).
- Крупную правку сначала обсудите в issue, чтобы не делать работу дважды.

## Проверки

Хостингового CI нет: проверки запускаются локально, одной командой (нужен shellcheck 0.11 или новее).

`tools/check.sh` — сборка с `WERROR=1` и `make check` (каждый рецепт ещё и против bash) на gcc и clang, тесты под ASan + UBSan и сверка с bash-прототипом (если рядом есть `../CleanLinux-prototype`); `--btrfs` — снимки на настоящей btrfs (root).

Быстрые проверки можно запускать автоматически перед каждым `git push`:

```sh
git config core.hooksPath tools/hooks   # перед push: tools/check.sh --quick
```

## Правила

- C17, только libc + libzstd (libcurl — опция `CURL=1`); стиль — как в соседнем коде, табы ([.editorconfig](.editorconfig)).
- Новое поведение — вместе с тестом (`tests/unit.c` или `tests/cli.sh`) и описанием в README и [docs/DESIGN.md](docs/DESIGN.md).
- Формат рецептов, базы пакетов и `.pkg.tar.zst` меняется только совместимо: старые базы и пакеты продолжают читаться.
- Релиз: `BURN_VERSION` в `src/util.h`, коммит `burn X.Y.Z`, тег `vX.Y.Z`, `make dist` → `burn-X.Y.Z.tar.gz` в GitHub Release; затем рецепт `burn` в CleanLinux-Recipes (`burn checksum --write burn`).

## Коммиты и pull request'ы

- Одна логическая правка — один коммит. Сообщение — на английском, в стиле истории: `область: что изменилось`
  (`bootstrap: …`, `docs: …`), подробности — в теле письма.
- Ветка от `main`, pull request в `main`; `Closes #N` (или `Closes vovawees/CleanLinux#N`) закрывает issue при слиянии.
- Документация меняется в том же pull request'е, что и поведение.

## English

burn is part of [CleanLinux](https://github.com/vovawees/CleanLinux). There is no hosted CI: run `tools/check.sh` (shellcheck 0.11+)
before every push (`git config core.hooksPath tools/hooks` runs the quick checks automatically). C17, libc + libzstd only; every behaviour change comes with a test and docs; on-disk formats change compatibly only; releases are tags vX.Y.Z with the `make dist` tarball attached, then the `burn` recipe is bumped.
Commit messages are in English (`area: what changed`); pull requests target `main`, and `Closes #N` closes the issue.
See the [roadmap](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md) and the [project board](https://github.com/users/vovawees/projects/1).
