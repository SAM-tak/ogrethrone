# Animated characters

Six skinned, animated FBX characters from Quaternius's **RPG Characters** pack (CC0),
each a Model component with an Animator playing one of its clips: the Warrior's sword
attack, the Wizard holding his staff, the Rogue running, the Ranger walking, the Cleric
idling and the Monk attacking. It exercises skinned FBX import, skeletal animation and
skinned motion vectors.

The asset is not part of this repository. Download
[`rpg_characters_-_nov_2020.zip`](https://opengameart.org/sites/default/files/rpg_characters_-_nov_2020.zip)
from [OpenGameArt](https://opengameart.org/content/lowpoly-rpg-characters) (12.9 MB) and
extract it into `assets/`, so that it contains `RPG Characters - Nov 2020/` with its `FBX/`
and `Textures/` folders. The folder is ignored by Git except for the `.meta` files that keep
the files' IDs, which the scene refers to.

```powershell
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe samples/characters
```

Outside Play the characters hold their Animators' times; drag **Time** in the Inspector to
scrub a clip, or press **F5** to play them (with **View → Motion Blur (Play)**, fast limbs
blur). The Console lists each file's clips when it is read: every character has Idle, Walk,
Run, Roll, Punch, PickUp, RecieveHit and Death, and its own attacks or spells.

The files name their texture only in the material's name (`Warrior_Texture`), so each Model
has material changes giving the materials their **Diffuse Map** from `Textures/`.

## License

The characters are by [Quaternius](https://quaternius.com), released under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) (public domain).
