# Cataclysm: Signal

[![follows Keel](https://img.shields.io/badge/follows-Keel-1f6feb)](https://github.com/theomgdev/keel)

<sub>A fork of Cataclysm: Dark Days Ahead, under the same CC BY-SA 3.0 license.</sub>

Cataclysm: Signal is a turn-based survival game set in a post-apocalyptic world.
While some have described it as a "zombie game", there is far more to it than
that. Struggle to survive in a harsh, persistent, procedurally generated world.
Scavenge the remnants of a dead civilization for food, equipment, or, if you are
lucky, a vehicle with a full tank of gas to get you the hell out of Dodge. Fight
to defeat or escape from a wide variety of powerful monstrosities, from zombies
to giant insects to killer robots and things far stranger and deadlier, and
against the others like yourself, who want what you have.

<p align="center">
    <img src="./data/screenshots/ultica-showcase-sep-2021.png" alt="Tileset: Ultica">
</p>

## What makes this one different

LLM-assisted contributions are welcome here, under one rule that gives the
project its name: signal over noise. Add up everything you write for a change —
commit message, pull request, comments, markdown — and it has to come out
shorter than the code it describes. Two lines of code do not get twenty lines of
explanation. Read the same ratio along the time axis and it becomes value over
time, so an hour spent re-deriving something already known is noise as well.

[`AGENTS.md`](AGENTS.md) is the whole working agreement, and it is short. Read it
before you open a pull request, whether you are a person or a model.

Work so far has gone into performance — the crafting menu and zone auto-sort near
large item piles — with content and mechanics of its own to follow. The game is
single-threaded by design and runs on everything from a phone to a desktop, so
speed here means removing wasted work, never adding parallelism.

<details>
<summary><b>What Signal adds on top of Dark Days Ahead</b> (mild spoilers)</summary>

**Gearing up for a fight**

- *Gear up from the stores* — one order, and the character walks the camp's loot
  zones the way they walk them to sort or to build, working through the crates
  and equipping themselves from what is stored there: the best weapon they can
  use, something to swing if all they had was a gun, better armour and clothing,
  then ammunition matched to the weapon they actually chose — loaded, not merely
  carried — bandages, painkillers, a day of rations and a canteen. It takes real
  time and it says what it did along the way. Give it to a follower from the `C`
  menu or in conversation, or take it yourself from the `O` zone-activities menu.
- It reads the zones you already keep. Armour in `LOOT_ARMOR`, bandages in
  `LOOT_DRUGS`, rounds in `LOOT_AMMO`, all of it under whatever `CAMP_STORAGE`
  you threw over the top — and inside the boxes and kits, not just what is loose
  on the floor. Displaced gear goes back to the zone it belongs in, so gearing up
  leaves the camp sorted rather than strewn.
- Every piece is judged against the one piece it would replace, or against bare
  skin where there is nothing to replace: coverage-weighted protection,
  encumbrance with legs and eyes or mouth weighted extra since losing mobility or
  vision is how survivors die, weather, and carrying capacity up to what is
  actually useful. Nobody strips a gas mask for a slightly better hat, keeps
  stacking bags once there is nowhere left to put anything, or pulls a parka on
  over a shirt in August — but an uncovered back gets covered whatever the month,
  because plain clothing is worth more than nothing even when it is worth almost
  nothing as armour.
- Pockets come before plating. Somebody with nothing to carry things in looks for
  a bag or a pair of cargo pockets first, since without them a backup blade,
  spare rounds and bandages alike have nowhere to go.
- It knows what it cannot use. A rifle the camp has no round for stays on the
  shelf rather than replacing the knife that was working. Somebody already
  overheating puts nothing else on; somebody freezing is dressed first.
- Your explicit orders always win. Favourites, pickup whitelists and anything you
  handed over on purpose are never quietly taken or thrown away.

**Faster where it hurts**

- The crafting menu no longer re-asks your whole inventory for every recipe on
  screen, and remembers recipe names for the duration of a sort.
- Tool quality lookups are cached on the inventory itself.
- Zone auto-sort no longer copies the whole zone tile set on every lookup, which
  is what made large loot piles crawl.

**Kept current**

- Signal tracks upstream Cataclysm: Dark Days Ahead, so everything the DDA
  volunteers add keeps arriving here.

</details>

## Building

[COMPILING.md](doc/c++/COMPILING.md) covers general information and recipes for
Linux, OS X, Windows and BSD, and [COMPILER_SUPPORT.md](doc/c++/COMPILER_SUPPORT.md)
lists which compilers are supported. There are also focused guides for
[MSYS2](doc/c++/COMPILING-MSYS.md), [vcpkg](doc/c++/COMPILING-VS-VCPKG.md) and
[cmake](doc/c++/COMPILING-CMAKE.md), and more in [doc/](doc/).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to get a change in, and
[AGENTS.md](AGENTS.md) for what a good change looks like. Bugs and suggestions
go to [the issue tracker](https://github.com/theomgdev/Cataclysm-Signal/issues).

## License

The code and content are released under the Creative Commons Attribution
ShareAlike 3.0 license, free to use, modify and redistribute for any purpose;
see https://creativecommons.org/licenses/by-sa/3.0/ for details. Some code
distributed with the project is not part of it and carries its own license
notice. The game this one is built on is the work of over a thousand volunteers,
and that work is here under the same terms.
