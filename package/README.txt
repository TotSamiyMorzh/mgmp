Mewgenics PvP (mgmp) 1.0.0  --  English first, русский ниже
===========================================================

ENGLISH
-------

What both computers need
- Mewgenics on Steam, the same game version (mgmp checks the game data when the
  players connect and refuses a mismatch).
- This whole folder. Put it on the same drive as the Steam game, so the game
  copy is made of hard links and takes no space.
- A backup of %APPDATA%\Glaiel Games\Mewgenics never hurts. The arena puts
  everything back after a fight.

Once on each computer: setup.bat
- Run setup.bat and paste the Steam game folder, for example
  D:\SteamLibrary\steamapps\common\Mewgenics
- It makes game\ next to it (links to the game files + steam_appid.txt) and does
  not touch the Steam folder.
- Why: a game started straight from Steam is restarted by Steam, and the mod
  loses its settings.
- After every game update, run setup.bat again (game\ still holds the old
  version otherwise, and the two players would not match).

Host (creates the game)
1. Steam is running.
2. Run host.bat. The game opens; the intro flies by.
3. That is all: the host can be joined over Steam and over the network (TCP 27600).

Client (joins) -- over Steam, no ports, no IP
1. Steam is running and the host is in your Steam friends.
2. Run client.bat and just press Enter (empty IP = Steam).
3. Join any way you like:
   - in the Steam friends list, on the host: "Join Game";
   - or the host presses F1 in game and clicks "invite" next to you in the
     steam block, and you accept the Steam invite;
   - or in game press F1 and click "join" next to the host in the steam block.
   Steam punches through the connection itself (or relays it through its servers).

Client over IP (fallback)
- Run client.bat and type the host's IP. You need the same network, TCP port
  27600 forwarded to the host, or Radmin VPN / ZeroTier / Tailscale.

In the game
- Both: Play -> your own save -> you are in the house.
- The host opens the PvP arena (the "ARENA" button in the pause menu (ESC), or
  F9); the client's lobby opens by itself.
- Each player picks their own cats by clicking a row. With "Items: From home"
  each player has an "ITEMS FROM HOME" button and dresses their cats from their
  own storage.
- The host sets the rules (cover, loot, monsters, birds, waves) and presses START.
- Take turns with your cats. After a win: ESC -> menu (REMATCH / HOME).
- The lobby and the arena follow the game language (Russian or English).

The mgmp windows over the game
- "mgmp -- session" and "mgmp -- log": connection status and log. F1 hides and
  shows them; the steam block (invite / join) is there too.
- If the two games ever disagree, both stop with HALTED. That is not a crash: the
  mod will not let you play two different games. Close both games and send the
  logs (below).

Logs (if something goes wrong)
- In this folder: mgmp_host_*.log or mgmp_client_*.log, and pvp_arena.log.
- The files from both computers are needed.

No game files ship with this mod. The arena level and the PvP bird data are
made from YOUR game when the game starts (modpath\, rebuilt by itself after a
game update); the lobby's names, descriptions and icons are read from your
game's resources.gpak while it runs.

Uninstall: delete this folder. Nothing was installed anywhere else, and the
Steam game folder was never changed.

Credits
- Built on mgmp (Mewgenics multiplayer), MIT License, by SanTertrust.
- Uses MinHook, Dear ImGui, nlohmann/json, stb_image and the game's own Steam API (Steam networking).
- Written with the help of an AI coding agent (Claude Code, Anthropic).
- Not affiliated with Glaiel Games or Edmund McMillen. Mewgenics is theirs.


РУССКИЙ
-------

Что нужно на ОБОИХ компах
- Mewgenics в Steam, одна и та же версия игры (иначе mgmp откажется соединяться:
  он сверяет данные игры при подключении).
- Эта папка целиком. Положите её на тот же диск, где стоит игра в Steam
  (тогда копия игры делается жёсткими ссылками и не занимает места).
- На всякий случай сохраните папку с сейвами:
  %APPDATA%\Glaiel Games\Mewgenics
  Арена после боя возвращает всё как было, но бэкап не помешает.

Один раз на каждом компе: setup.bat
- Запустите setup.bat и вставьте путь к папке игры в Steam, например
  D:\SteamLibrary\steamapps\common\Mewgenics
- Он создаст рядом папку game\ (ссылки на файлы игры + steam_appid.txt).
  Папку игры в Steam он не трогает.
- Зачем: если запускать игру прямо из Steam, Steam перезапустит её сам и мод
  потеряет свои настройки.
- После каждого обновления игры запустите setup.bat ещё раз (иначе в game  останется старая версия и у игроков не совпадут данные).

Хост (кто создаёт игру)
1. Steam запущен.
2. Запустите host.bat. Игра откроется, интро пролетит быстро.
3. Всё: хост сразу доступен и через Steam, и по сети (порт 27600).

Клиент (кто подключается) — через Steam, без портов и IP
1. Steam запущен, хост — у вас в друзьях Steam.
2. Запустите client.bat и просто нажмите Enter (пустой IP = Steam).
3. Подключитесь любым способом:
   - в списке друзей Steam у хоста — «Присоединиться к игре»;
   - или хост в игре жмёт F1 и в блоке steam напротив вас «invite»,
     вы принимаете приглашение в Steam;
   - или в игре F1, блок steam, напротив хоста «join».
   Steam сам пробьёт соединение (или пустит его через свои серверы).

Клиент по IP (запасной вариант)
- client.bat и ввести IP хоста. Нужны одна сеть или проброс TCP-порта 27600
  у хоста, либо Radmin VPN / ZeroTier / Tailscale.

Дальше в игре
- Оба: Play -> свой сейв -> вы в доме.
- Хост открывает PvP-арену (кнопка «АРЕНА» в меню паузы (ESC) или F9) — у
  клиента лобби откроется само.
- Каждый выбирает своих котов кликом по строке. При «Вещи: Из дома» у каждого
  есть кнопка «ВЕЩИ ИЗ ДОМА» — одеваете своих котов из своего хранилища.
- Правила (укрытия, лут, монстры, птицы, волны) ставит хост. СТАРТ жмёт хост.
- Ходите своими котами по очереди. После победы: ESC -> меню (РЕВАНШ / ДОМОЙ).
- Лобби и арена говорят на языке игры (русский или английский).

Окна mgmp поверх игры
- «mgmp -- session» и «mgmp -- log» — статус соединения и журнал. F1 прячет
  и показывает их, там же блок steam (invite / join).
- Если игры разошлись, обе остановятся с надписью HALTED. Это не краш: так мод
  не даёт играть в разные игры. Закройте обе игры и пришлите логи (ниже).

Логи (если что-то пошло не так)
- В этой папке: mgmp_host_*.log или mgmp_client_*.log, pvp_arena.log.
- Нужны файлы с обоих компов.

Файлов игры в моде нет. Уровень арены и данные PvP-птиц делаются из ВАШЕЙ
игры при её запуске (папка modpath\, сама пересоздаётся после обновления
игры); имена, описания и иконки в лобби читаются из resources.gpak вашей игры
прямо во время работы.

Удаление: удалите эту папку. Больше нигде ничего не ставилось, папка игры в
Steam не менялась.

Авторы
- Основано на mgmp (мультиплеер для Mewgenics), лицензия MIT, автор SanTertrust.
- Использует MinHook, Dear ImGui, nlohmann/json, stb_image и Steam API самой игры (сеть Steam).
- Написано с помощью ИИ-агента (Claude Code, Anthropic).
- Не связано с Glaiel Games и Эдмундом Макмилленом. Mewgenics принадлежит им.
