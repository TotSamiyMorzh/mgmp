# Mewgenics PvP — v1.1

An unofficial two-player PvP mod for **Mewgenics 1.1.21239**, on **Windows x64**. Each player selects cats from their own save and fights the other player in a configurable arena.

**[Download v1.1](https://github.com/TotSamiyMorzh/mgmp/releases/tag/v1.1)** · [Player guide / Инструкция](package/README.txt) · [Changelog](package/CHANGELOG.txt)
**[Community&Bug reports](https://t.me/+mPtOHrfdNFljMjVi)**
## English

### Features

- Connect through Steam invitations or the friends list, with direct TCP as a fallback.
- Pick your own cats in a lobby with Steam names and avatars. The host can open the lobby before their opponent connects and use **Invite** to open the Steam invitation dialog.
- Configure team size, equipment (none, random or from home), cover, loot, monsters and bird waves.
- Play synchronized turn-based battles, then rematch or return home.
- Russian and English UI, following the game's language.

### Install and play

1. Download `mewgenics_pvp_1.1.zip` from the release page and unpack it on **both computers**. Place the mod on the same drive as the Steam game.
2. Keep Steam running. Run `setup.bat` and enter your Steam Mewgenics folder. It creates a local `game` folder using hard links and leaves the Steam installation unchanged.
3. The host runs `host.bat`. The other player runs `client.bat`; leave the IP empty to connect through Steam.
4. Both players load their **own save** and enter the house. The host opens **ARENA** in the pause menu (Esc), then clicks **Invite**, or the friend uses **Join Game** in Steam.
5. Select cats, let the host set the rules, and press **START**. After the fight, use **REMATCH** or **HOME**.

For a direct TCP connection, enter the host's IP in `client.bat`. The default port is **27600**; this requires a reachable host, a shared network or a suitable VPN.

### Updating from v1.0.0

1. Close the game on both computers.
2. Download and unpack `mewgenics_pvp_1.1.zip`.
3. Copy the contents of the new `mewgenics_pvp` folder into your existing mod folder, **replacing the old files**.
4. Keep your existing `game` folder. If the game itself was updated, rerun `setup.bat` against the supported Steam installation.
5. Start the mod as usual with `host.bat` or `client.bat`.

There is no need to move your saves. **Both players must update:** protocol **29** is incompatible with v1.0.0. The supported game version remains **1.1.21239**.

### What's new in v1.1

- Steam avatars, names and an invitation button in the lobby, including while waiting for a player.
- Amber tactical markers for neutral monsters and birds.
- Normal opening cinematic speed by default.
- Synchronized stationary cat turns, with facing included in PvP state checks.
- Random-X synchronization fixes for items including **Money Bag**.
- Player-controlled summons belong to the player who summoned them.

### Saves, diagnostics and testing

Back up `%APPDATA%\Glaiel Games\Mewgenics` before playing. The arena uses fighter clones and restores the pre-arena state when returning home. No game files or saves are distributed with the mod; required arena data is generated from your own installation.

If **HALTED** appears, close both games and provide logs from **both players**: `mgmp_host_*.log` / `mgmp_client_*.log` and `pvp_arena.log`. F1 toggles the diagnostic panel.

Validation: 9 automated tests pass. The facing test matched 54 paired state hashes with 24 test turns per copy and no HALTED. The preceding synchronization fixes passed a 30-minute run with 395 matching paired hashes. These checks do not guarantee every skill/item combination is covered. The player release has autobattle disabled and the synthetic-cat stress harness compiled out.

## Русский

Неофициальный PvP-мод для двух игроков: каждый выбирает котов из **своего сохранения** и сражается с соперником на настраиваемой арене. Требуются **Windows x64**, Steam и **Mewgenics 1.1.21239**.

### Возможности

- Подключение через приглашения и список друзей Steam, либо напрямую по TCP.
- Лобби со Steam-аватарками и никами. Хост может открыть его заранее и нажать **«Пригласить»**, пока ждёт соперника.
- Настройки размера команд, вещей (без вещей, рандом или из дома), укрытий, лута, монстров и волн птиц.
- Синхронизированные пошаговые бои, реванш и возвращение домой.
- Русский и английский интерфейс в зависимости от языка игры.

### Установка и запуск

1. Скачайте `mewgenics_pvp_1.1.zip` со страницы релиза и распакуйте на **обоих компьютерах**. Положите мод на тот же диск, где находится игра в Steam.
2. Запустите Steam. Откройте `setup.bat` и укажите папку Mewgenics в Steam. Скрипт создаст локальную папку `game` с жёсткими ссылками, не меняя установку Steam.
3. Хост запускает `host.bat`, второй игрок — `client.bat`. Для Steam оставьте IP пустым.
4. Оба загружают **свои сохранения** и входят в дом. Хост открывает **«АРЕНА»** через меню паузы (Esc) и нажимает **«Пригласить»**. Также можно выбрать **«Присоединиться к игре»** в Steam.
5. Выберите котов; хост настраивает правила и нажимает **«СТАРТ»**. После боя доступны **«РЕВАНШ»** и **«ДОМОЙ»**.

Для подключения по TCP введите IP хоста в `client.bat`. Порт по умолчанию — **27600**; нужны доступный хост, общая сеть или подходящий VPN.

### Обновление с v1.0.0

1. Закройте игру на обоих компьютерах.
2. Скачайте и распакуйте `mewgenics_pvp_1.1.zip`.
3. Скопируйте содержимое новой папки `mewgenics_pvp` в старую папку мода **с заменой файлов**.
4. Оставьте существующую папку `game`. Если обновлялась сама игра, повторно запустите `setup.bat`, указав поддерживаемую установку Steam.
5. Запускайте мод как раньше: `host.bat` или `client.bat`.

Переносить сохранения не нужно. **Обновиться должны оба игрока:** протокол **29** несовместим с v1.0.0. Поддерживаемая версия игры — **1.1.21239**.

### Новое в v1.1

- Steam-аватарки, ники и кнопка приглашения в лобби, доступная до подключения соперника.
- Янтарные тактические маркеры у нейтральных монстров и птиц.
- Обычная скорость вступительной заставки по умолчанию.
- Синхронизация поворотов котов на месте; направление учитывается при проверке состояния PvP.
- Исправление синхронизации случайного X у предметов, включая **Money Bag**.
- Управляемые призывы принадлежат игроку, который их призвал.

### Сохранения, диагностика и проверки

Перед игрой сохраните резервную копию `%APPDATA%\Glaiel Games\Mewgenics`. Арена использует клонов бойцов и восстанавливает состояние до арены при возвращении домой. Файлы игры и сохранения не распространяются: нужные данные арены создаются из вашей установки.

При **HALTED** закройте обе игры и пришлите логи **обоих игроков**: `mgmp_host_*.log` / `mgmp_client_*.log` и `pvp_arena.log`. F1 скрывает и показывает диагностическую панель.

Проверки: прошли 9 автоматических тестов. Проверка поворотов дала 54 совпавших парных хеша и по 24 тестовых поворота на копию без HALTED. Предыдущие исправления синхронизации прошли 30-минутный прогон с 395 совпавшими парными хешами. Эти проверки не охватывают все сочетания навыков и предметов. В пользовательской сборке автобой выключен, а генератор тестовых котов не включён в сборку.

## Building from source

Use Windows x64 and Visual Studio Build Tools with C++ and CMake:

```powershell
cmake -S . -B build -A x64 -DMGMP_SYNCTEST=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\tools\make_release.ps1 -Version 1.1
```

For the optional development stress harness, see [SYNCTEST.md](SYNCTEST.md). It is separate from the player release. The PvP implementation is in `src/pvp`; it uses the lockstep core of the original mgmp co-op project.

## Credits and license

Based on [mgmp by SanTertrust](https://github.com/SanTertrust/mgmp), under the MIT License. Uses MinHook, Dear ImGui, nlohmann/json, stb_image and the game's Steam API. Developed with Claude Code (Anthropic) and Codex (OpenAI). Third-party license notices are included in the release archive.

Not affiliated with Glaiel Games or Edmund McMillen. Mewgenics belongs to its creators.
