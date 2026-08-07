# lrmame driver subset

Which `SOURCES=` files a shippable lrmame (MAME 0.289) build contains, and why
everything else was left out. The list itself is `tools/lrmame-subset.sources`,
one path per line, ready to be comma-joined into `SOURCES=`.

Regenerate — the two device-side inputs are the reason this is a committed
artefact rather than a build step:

```sh
# MRA setnames (2,954 distinct, from 6,622 MRAs) and the 0.37b5 driver list
ssh root@192.168.20.81 'find /media/fat/_Arcade -name "*.mra" -print0 \
    | xargs -0 cat | grep -oE "<setname>[^<]+</setname>|zip=\"[^\"]+\""' > mra-raw.txt
ssh root@192.168.20.81 'cd /media/fat/games/mame && \
    SDL_VIDEODRIVER=dummy ./mame "*" -sourcefile' > mame4all-sourcefile.txt

tools/lrmame-driver-index.py --out lrmame-index.json
tools/lrmame-subset.py --index lrmame-index.json --mra mra-setnames.txt \
    --mame4all mame4all-sourcefile.txt --extra pacman/pacman.cpp \
    --sources tools/lrmame-subset.sources --report docs/lrmame-subset.md
```

`pacman` is in the list via `--extra` and carries no coverage: it is the bench
control every lrmame measurement so far has used, and dropping it would end the
comparability of the Stage 10 numbers.

Each number below is a count of driver *files* except where it says parents.

- **540 driver files** = 526 carrying coverage + 14 pulled in only to close clone→parent links
- **1333 parent romsets** of coverage value
- of the 526 coverage files, **7** need `drcbearm32` (SH-2/SH-3)

## Dropped

| reason | files | parents |
|---|---:|---:|
| not working | 647 | 0 |
| MiSTer core (MRA) | 233 | 1215 |
| mame4all already has it | 183 | 598 |
| gambling (manufacturer) | 63 | 1203 |
| mahjong | 30 | 154 |
| DRC CPU drcbearm32 has not been validated on | 21 | 114 |
| gambling (titles) | 18 | 34 |
| PS1-class 3D | 11 | 136 |
| out of class — PS1-class (R3000 + GPU) | 6 | 90 |
| out of class — Dreamcast/Naomi-class (SH-4) | 3 | 61 |
| console-core false gap | 3 | 111 |

### Dropped — gambling (manufacturer) (63 files, 1203 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `igt/peplus.cpp` | 237 | Player's Edge Plus (BE0013) Blackjack |
| `barcrest/mpu4mod2sw.cpp` | 136 | Twenty One (Barcrest) (Dutch) (MPU4) (DTO 2.0) |
| `barcrest/mpu4mod4oki.cpp` | 64 | American Highway (Barcrest) (Dutch) (MPU4) (DAH 2.0) |
| `igs/goldstar.cpp` | 56 | 3 Cards Poker 96 (V1.6) |
| `acorn/aristmk5.cpp` | 42 | Bad Dog Poker (0200428V, NSW/ACT) |
| `misc/calomega.cpp` | 41 | Casino 21 UCMC/IWC (ver 30.08) |
| `misc/goldnpkr.cpp` | 41 | Amstar Draw Poker |
| `amcoe/sfbonus.cpp` | 38 | Animal Bonus Nudge (Version 2.1 Dual) |
| `barcrest/mpu4unsorted.cpp` | 37 | Against All Odds (Eurotek) (MPU4) |
| `funworld/funworld.cpp` | 33 | Bonus Card (German) |
| `misc/norautp.cpp` | 33 | Draw Poker (Joker Poker V.01) |
| `dynax/ddenlovr.cpp` | 32 | Panel & Variety Akamaru Q Joushou Dont-R |
| `aristocrat/aristmk4.cpp` | 28 | 3 Bags Full (5VXFC790, Victoria) |
| `igs/igs_m027.cpp` | 27 | Chaoji Dou Dizhu (V219CN) |
| `dynax/dynax.cpp` | 26 | 7jigen no Youseitachi - Mahjong 7 Dimensions (Japan) |
| `barcrest/mpu4crystal.cpp` | 24 | Aladdin's Cave (Crystal) (MPU4) (set 1) |
| `barcrest/mpu4empire.cpp` | 22 | Apache Gold (Empire) (MPU4, set 1) |
| `barcrest/mpu4mdm.cpp` | 20 | 2p Unlimited (Mdm) (MPU4) |
| `misc/gei.cpp` | 19 | Amuse (Version 50.08 IBA) |
| `barcrest/mpu4union.cpp` | 18 | Crazy Bingo (Union) (MPU4) |
| `merit/merit.cpp` | 18 | The Big Apple (2131-13, U5-0) |
| `merit/meritm.cpp` | 18 | Americana (9131-01) |
| `misc/multfish.cpp` | 18 | Crazy Monkey (100311 World) |
| `misc/highvdeo.cpp` | 14 | Bra$il (Version 3) |
| `barcrest/mpu4mod4yam.cpp` | 13 | Adders & Ladders (Barcrest) (Dutch) (MPU4) (DAL 1.2) |
| `misc/statriv2.cpp` | 13 | Big Casino |
| `barcrest/mpu4vid.cpp` | 11 | Adders and Ladders (v2.1) (MPU4 Video) |
| `bfm/bfm_sc2.cpp` | 9 | Golden Crown (Dutch, Game Card 95-752-011) |
| `astrocorp/astrcorp.cpp` | 8 | Stone Age (Astro, Ver. EN.03.A, 2005/02/21) |
| `bordun/skylncr.cpp` | 8 | Butterfly Video Game (version A00) |
| `igs/igspoker.cpp` | 8 | Champion League (v220I, Poker) |
| `subsino/subsino.cpp` | 8 | Poker Carnival |
| `subsino/subsino2.cpp` | 8 | Bishou Jan (Japan, Ver. 203) |
| `bfm/bfcobra.cpp` | 6 | Brain Box II (Set 114) |
| `jpm/jpmimpct.cpp` | 6 | Cluedo (prod. 2D) |
| `barcrest/mpu4misc.cpp` | 5 | Bangin' Away (Global) (MPU4, set 1) |
| `adp/adp.cpp` | 4 | Fashion Gambler (set 1) |
| `barcrest/mpu4plasma.cpp` | 4 | Apache (Barcrest) (MPU4 w/ Plasma DMD?) |
| `bmc/bmcpokr.cpp` | 4 | Dongfang Shenlong |
| `dynax/realbrk.cpp` | 4 | Dai-Dai-Kakumei (Japan) |
| `misc/coinmstr.cpp` | 4 | Joker Poker (Coinmaster set 1) |
| `misc/magicard.cpp` | 4 | Lucky 7 (Impera, V04/91a, set 1) |
| `misc/umipoker.cpp` | 4 | Baccarat Special |
| `funworld/snookr10.cpp` | 3 | Apple 10 (Ver 1.21) |
| `bfm/rastersp.cpp` | 2 | Football Crazy (Video Quiz) |
| `bmc/koftball.cpp` | 2 | Kaimen Hu (BMC) |
| `bordun/att.cpp` | 2 | Lian Huan Pao - ATT II |
| `dynax/hnayayoi.cpp` | 2 | Hana Yayoi (Japan) |
| `igt/drw80pkr.cpp` | 2 | Draw 80 Poker (Minn) |
| `jpm/guab.cpp` | 2 | Give us a Break |
| `jpm/jpmsys5.cpp` | 2 | Monopoly Deluxe (JPM) (Version 6) (SYSTEM5 VIDEO) |
| `wing/luckgrln.cpp` | 2 | 7 Smash (set 1) |
| `aristocrat/caswin.cpp` | 1 | Royal Casino (D-2608208A1-2) |
| `barcrest/mpu4dealem.cpp` | 1 | Deal 'Em (MPU4 Conversion Kit, v7.0) |
| `barcrest/mpu4redpoint.cpp` | 1 | Cherry Bingo (Redpoint Systems) (MPU4) |
| `bmc/bmcbowl.cpp` | 1 | Konkyuu no Hoshi |
| `bmc/popobear.cpp` | 1 | PoPo Bear |
| `igt/fortune1.cpp` | 1 | Fortune I (PK485-S) Draw Poker |
| `merit/mgames.cpp` | 1 | Match Games |
| `pc/pcat_dyn.cpp` | 1 | Tournament Solitaire (V1.06, 08/03/95) |
| `seta/vsmjtria.cpp` | 1 | VS Mahjong Triangle |
| `sunwise/jankenmn.cpp` | 1 | Janken Man Kattara Ageru |
| `wing/lucky74.cpp` | 1 | Lucky 74 (bootleg, set 1) |

### Dropped — mahjong (30 files, 154 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `nichibutsu/nbmj8688.cpp` | 25 | Apparel Night (Japan 860929) |
| `nichibutsu/nbmj9195.cpp` | 25 | Mahjong Circuit no Mehyou (Japan) |
| `nichibutsu/nbmj8891.cpp` | 21 | Abunai Houkago - Mou Matenai (Japan 890325) |
| `misc/homedata.cpp` | 16 | Battlecry (Version E) |
| `nichibutsu/nbmj8991.cpp` | 13 | Mahjong Gal no Kaika (Japan) |
| `vsystem/fromance.cpp` | 7 | Mahjong Daiyogen (Japan) |
| `jaleco/jalmah.cpp` | 6 | Mahjong Daireikai (Japan) |
| `seta/srmp2.cpp` | 5 | Mahjong Yuugi (Japan set 1) |
| `vsystem/ojankohs.cpp` | 5 | Chinese Casino (Japan) |
| `nichibutsu/nyanpai.cpp` | 4 | Mahjong 4P Shimasho (Japan) |
| `seta/rmhaihai.cpp` | 4 | Real Mahjong Haihai (Japan, newer) |
| `vsystem/fromanc2.cpp` | 3 | Taisen Idol-Mahjong Final Romance 2 (Japan, newer) |
| `nichibutsu/nbmj8900.cpp` | 2 | Oh! Paipee (Japan 890227) |
| `seibu/goodejan.cpp` | 2 | Good E Jong -Kachinuki Mahjong Syoukin Oh!!- (set 1) |
| `dataeast/mirage.cpp` | 1 | Mirage Youjuu Mahjongden (Japan) |
| `igs/dunhuang.cpp` | 1 | Mahjong Dunhuang |
| `irem/m14.cpp` | 1 | PT Reach Mahjong (Japan) |
| `jaleco/bnstars.cpp` | 1 | Vs. Janshi Brandnew Stars |
| `metro/tmmjprd.cpp` | 1 | Tokimeki Mahjong Paradise - Dear My Love |
| `misc/mjsenpu.cpp` | 1 | Mahjong Senpu (Japan) |
| `nmk/cultures.cpp` | 1 | Jibun wo Migaku Culture School Mahjong Hen |
| `sanritsu/jantotsu.cpp` | 1 | 4nin-uchi Mahjong Jantotsu |
| `sanritsu/jongkyo.cpp` | 1 | Jongkyo |
| `sanritsu/mjkjidai.cpp` | 1 | Mahjong Kyou Jidai (Japan) |
| `sanritsu/ron.cpp` | 1 | Futari Mahjong Ron II (set 1) |
| `seibu/sengokmj.cpp` | 1 | Sengoku Mahjong (Japan) |
| `seta/kiwame.cpp` | 1 | Pro Mahjong Kiwame |
| `seta/srmp5.cpp` | 1 | Super Real Mahjong P5 |
| `seta/srmp6.cpp` | 1 | Super Real Mahjong P6 (Japan) |
| `toaplan/mjsister.cpp` | 1 | Mahjong Sisters (Japan) |

### Dropped — PS1-class 3D (11 files, 136 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `sony/zn.cpp` | 38 | 1 on 1 Government (Japan) |
| `namco/namcos12.cpp` | 26 | Attack Pla Rail (Japan, AP1/VER.A) |
| `namco/namcos22.cpp` | 17 | Ace Driver (World, AD2) |
| `williams/seattle.cpp` | 12 | Bio F.R.E.A.K.S (prototype, boot ROM 0.1i) |
| `sega/model2.cpp` | 11 | Daytona USA (Revision A) |
| `williams/vegas.cpp` | 11 | CART Fury Championship Racing (ver 1.00) |
| `namco/namcos11.cpp` | 10 | Dancing Eyes (World, DC2/VER.B) |
| `konami/gticlub.cpp` | 3 | GTI Club: Rally Cote D'Azur (ver EAA) |
| `konami/zr107.cpp` | 3 | Jet Wave (EAB, Euro v1.04) |
| `namco/namcos21.cpp` | 3 | Winning Run (World) (89/06/06, Ver.09) |
| `konami/hornet.cpp` | 2 | Gradius IV: Fukkatsu (ver JAC) |

### Dropped — DRC CPU drcbearm32 has not been validated on (21 files, 114 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `konami/firebeat.cpp` | 19 | Beatmania III |
| `misc/vamphalf.cpp` | 18 | Age Of Heroes - Silkroad 2 (v0.63 - 2001/02/07) |
| `eolith/eolith.cpp` | 13 | Candy Candy |
| `itech/iteagle.cpp` | 12 | Big Buck Hunter (v1.00.14) |
| `namco/namcos23.cpp` | 12 | 500 GP (US, 5GP3 Ver. C) |
| `snk/hng64.cpp` | 7 | Beast Busters: Second Nightmare |
| `atari/jaguar.cpp` | 6 | Area 51 (R3000) |
| `misc/dgpix.cpp` | 5 | Elfin |
| `misc/limenko.cpp` | 5 | Dynamite Bomber (Korea, Rev 1.5) |
| `konami/konamim2.cpp` | 3 | Evil Night (ver UBA) |
| `misc/policetr.cpp` | 2 | Police Trainer (Rev 1.3B, Rev 0.3 PCB) |
| `promat/gstream.cpp` | 2 | G-Stream G2020 |
| `rare/kinst.cpp` | 2 | Killer Instinct |
| `eolith/eolith16.cpp` | 1 | KlonDike+ |
| `eolith/vegaeo.cpp` | 1 | Crazy War |
| `f32/f-32.cpp` | 1 | Mosaic (F2 System) |
| `konami/ultrsprt.cpp` | 1 | Five a Side Soccer (ver UAA) |
| `misc/pasha2.cpp` | 1 | Pasha Pasha 2 |
| `namco/dkmb.cpp` | 1 | Donkey Kong / Donkey Kong Jr / Mario Bros |
| `namco/turrett.cpp` | 1 | Turret Tower - The Enemy Has Arrived |
| `seta/speglsht.cpp` | 1 | Super Eagle Shot |

### Dropped — console-core false gap (3 files, 111 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `nintendo/playch10.cpp` | 54 | 1942 (PlayChoice-10) |
| `nintendo/vsnes.cpp` | 35 | Vs. Balloon Fight (set BF4 A-3) |
| `sega/segac2.cpp` | 22 | Borench (set 1) |

### Dropped — out of class — PS1-class (R3000 + GPU) (6 files, 90 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `sony/taitogn.cpp` | 31 | Aero Fighters Special (VER 1.00G) |
| `konami/ksys573.cpp` | 25 | Anime Champ (GCA07 VER. JAA) |
| `namco/namcos10.cpp` | 15 | Uchuu Daisakusen: Chocovader Contactee (Japan, CVC1 Ver.A) |
| `konami/konamigv.cpp` | 9 | Hyper Athlete (GV021 Japan 1.00) |
| `konami/twinkle.cpp` | 9 | beatmania IIDX 2nd style (GC985 JAA) |
| `konami/konamigq.cpp` | 1 | Crypt Killer (GQ420 UAA) |

### Dropped — out of class — Dreamcast/Naomi-class (SH-4) (3 files, 61 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `sega/segasp.cpp` | 30 | Aminosan (satellite) |
| `sega/dc_atomiswave.cpp` | 28 | Animal Basket / Hustle Tamaire Kyousou (24 Jan 2005) |
| `konami/konamigs.cpp` | 3 | Dance Dance Revolution Kids (GQAN4 JAA) |

### Dropped — gambling (titles) (18 files, 34 parents)

Printed in full: this is a judgement call, and `--keep <file>` overrides it.

| file | parents | example |
|---|---:|---|
| `gametron/gatron.cpp` | 3 | Bingo |
| `midcoin/wallc.cpp` | 3 | unknown Sidam poker (vertical) |
| `misc/ampoker2.cpp` | 3 | American Poker II |
| `misc/ltcasino.cpp` | 3 | Little Casino II (v30.0) |
| `misc/meyc8080.cpp` | 3 | Casino Black Jack (color, Standard 00-05) |
| `funtech/funtech_z80.cpp` | 2 | Super Two In One |
| `igs/spoker.cpp` | 2 | Jingle Bell (v200US, alternative hardware) |
| `misc/amusco.cpp` | 2 | American Music Poker (V1.4) |
| `misc/dwarfd.cpp` | 2 | Draw Poker III / Dwarfs Den (Dwarf Gfx) |
| `misc/vpoker.cpp` | 2 | 5-Aces Poker |
| `nichibutsu/pastelg.cpp` | 2 | Pastel Gal (Japan 851224) |
| `misc/clpoker.cpp` | 1 | Poker Genius |
| `misc/jubilee.cpp` | 1 | Double-Up Poker (Jubilee) |
| `misc/majorpkr.cpp` | 1 | Major Poker (set 1, v2.2) |
| `misc/pokerout.cpp` | 1 | Potten's Poker stealth with Breakout front game |
| `misc/roul.cpp` | 1 | Super Lucky Roulette |
| `misc/vroulet.cpp` | 1 | Vegas Roulette |
| `stern/supdrapo.cpp` | 1 | Super Draw Poker (set 1) |

## Kept — 526 files, 1333 parents

| file | parents | DRC | example |
|---|---:|---|---|
| `taito/taito_f3.cpp` | 35 | — | Arabian Magic (Ver 1.0O 1992/07/06) |
| `seta/ssv.cpp` | 26 | — | Change Air Blade (Japan) |
| `metro/metro.cpp` | 24 | — | Sankokushi (Japan) |
| `sega/segas32.cpp` | 22 | — | Alien3: The Gun (World) |
| `dataeast/tumbleb.cpp` | 17 | — | B.C. Story (set 1) |
| `itech/itech32.cpp` | 17 | — | BloodStorm (v2.22) |
| `konami/djmain.cpp` | 17 | — | beatmania (ver JA-B) |
| `amiga/arsystems.cpp` | 16 | — | SportTime Table Hockey (Arcadia, set 1, V 2.1) |
| `misc/cvs.cpp` | 16 | — | Video Eight Ball |
| `sega/megadriv_acbl.cpp` | 16 | — | Gunstar Heroes / Snake Rattle n' Roll / Joe & Mac (Conny bootleg of Mega Drive versions) |
| `cave/cv1k.cpp` | 15 | ok | Akai Katana (Japan, 2010/ 8/13 MASTER VER.) |
| `kaneko/suprnova.cpp` | 15 | ok | Cyvern - The Dragon Weapons (US) |
| `konami/konamigx.cpp` | 15 | — | Crazy Cross (ver EAA) |
| `nintendo/snesb.cpp` | 14 | — | Ghost Chaser Densei (SNES bootleg, set 1) |
| `seta/seta2.cpp` | 14 | — | Deer Hunting USA V4.3 |
| `itech/itech8.cpp` | 13 | — | Arlington Horse Racing (v1.40-D) |
| `igs/igs017.cpp` | 12 | — | Champion Poker 2 (V100A) |
| `jaleco/ms32.cpp` | 12 | — | Mahjong Angel Kiss (ver 1.0, 92047-01 version) |
| `kaneko/expro02.cpp` | 12 | — | Fantasia (940429 PCB, set 1) |
| `namco/namcona1.cpp` | 11 | — | Bakuretsu Quiz Ma-Q Dai Bouken (Japan) |
| `sigma/sammymdl.cpp` | 11 | — | Animal Catch |
| `dooyong/dooyong.cpp` | 10 | — | Blue Hawk |
| `konami/konmedal.cpp` | 10 | — | Buttobi Striker |
| `namco/namconb1.cpp` | 10 | — | Great Sluggers '94 |
| `psikyo/psikyosh.cpp` | 10 | ok | The Fallen Angels (World) / Daraku Tenshi: The Fallen Angels (Japan) |
| `taito/taito_z.cpp` | 10 | — | Aqua Jack (World) |
| `gaelco/gaelco2.cpp` | 9 | — | Alligator Hunt (World, protected, checksum 2B34128B) |
| `sega/segaxbd.cpp` | 9 | — | A.B. Cop (World) (FD1094 317-0169b) |
| `sigma/sigmab98.cpp` | 9 | — | Burning Sanrinsya - Burning Tricycle |
| `efo/cedar_magnet.cpp` | 8 | — | Booby Kids (Magnet System) |
| `excellent/lastbank.cpp` | 7 | — | Fever 13 (Japan, v1.3) |
| `misc/4enraya.cpp` | 7 | — | 4 En Raya (set 1) |
| `misc/esd16.cpp` | 7 | — | Deluxe 5 (ver. 0107, 07/01/2000, set 1) |
| `psikyo/psikyo4.cpp` | 7 | ok | Taisen Hot Gimmick Kairakuten (Japan) |
| `seibu/seibuspi.cpp` | 7 | — | E Jong High School (Japan) |
| `taito/asuka.cpp` | 7 | — | Asuka & Asuka (World) |
| `williams/tmaster.cpp` | 7 | — | Touchmaster (v3.00 Euro) |
| `yunsung/nmg5.cpp` | 7 | — | 7 Ordi (Korea) |
| `yunsung/paradise.cpp` | 7 | — | Mad Ball (V2.0) |
| `atari/harddriv.cpp` | 6 | — | Hard Drivin' (cockpit, rev 7) |
| `jaleco/tetrisp2.cpp` | 6 | — | Rock'n Tread (Japan) |
| `konami/konmedal68k.cpp` | 6 | — | Ganbare Goemon |
| `konami/mystwarr.cpp` | 6 | — | Gaiapolis (ver EAF) |
| `misc/corona.cpp` | 6 | — | Le Grandchamps |
| `misc/tickee.cpp` | 6 | — | Ghost Hunter |
| `nichibutsu/jangou.cpp` | 6 | — | Country Girl (Japan set 1) |
| `nintendo/multigam.cpp` | 6 | — | Multi Game (set 1) |
| `sega/segas24.cpp` | 6 | — | Dynamic Country Club (World, ROM Based) |
| `sega/segaybd.cpp` | 6 | — | Galaxy Force 2 |
| `alpha/equites.cpp` | 5 | — | Bingo Time |
| `amiga/cubo.cpp` | 5 | — | Candy Puzzle (v1.0) |
| `dataeast/liberate.cpp` | 5 | — | Boomer Rang'r / Genesis (set 1) |
| `dataeast/rohga.cpp` | 5 | — | Hangzo (Japan, prototype) |
| `dataeast/simpl156.cpp` | 5 | — | Chain Reaction (World, Master Version 2.2, 1995.09.25) |
| `ice/lethalj.cpp` | 5 | — | Crazzy Clownz (Version 1.0) |
| `igs/igs011.cpp` | 5 | — | Dragon World (World, V040O) |
| `igs/pgm2.cpp` | 5 | — | DoDonPachi Dai-Ou-Jou Tamashii (V201, China) |
| `misc/dreamwld.cpp` | 5 | — | Baryon - Future Assault (set 1) |
| `misc/freekick.cpp` | 5 | — | Free Kick (NS6201-A 1987.10) |
| `misc/magic10.cpp` | 5 | — | Magic's 10 (ver. 16.55) |
| `misc/smsmcorp.cpp` | 5 | — | Second Chance |
| `namco/funcube.cpp` | 5 | — | Funcube (v1.5) |
| `playmark/playmark.cpp` | 5 | — | Big Twin |
| `seta/simple_st0016.cpp` | 5 | — | Kankoku Hanafuda Go-Stop |
| `taito/taitojc.cpp` | 5 | — | Densha de GO! (Ver 2.3 J) |
| `toaplan/sunwise.cpp` | 5 | — | Burger Kids (Japan) |
| `atari/maxaflex.cpp` | 4 | — | Astro Chase (Max-A-Flex) |
| `dataeast/deco_mlc.cpp` | 4 | ok | Janken Game Acchi Muite Hoi! (Japan 1.3) |
| `edevices/fantland.cpp` | 4 | — | Born To Fight (set 1) |
| `efo/cidelsa.cpp` | 4 | — | Altair |
| `f32/crospang.cpp` | 4 | — | Bestri (Korea, set 1) |
| `gaelco/gaelco3d.cpp` | 4 | — | Football Power (version 4.6) |
| `igs/igs_m027xa.cpp` | 4 | — | Crazy Bugs (V204US) |
| `misc/crystal.cpp` | 4 | — | The Crystal of Kings |
| `misc/dcheese.cpp` | 4 | — | ChuckECheese's Match Game |
| `misc/gumbo.cpp` | 4 | — | Double Point |
| `misc/micro3d.cpp` | 4 | — | Battle of the Solar System (rev. 1.1a 7/23/92) |
| `namco/namcos21_c67.cpp` | 4 | — | Air Combat (AC2, US) |
| `nichibutsu/nightgal.cpp` | 4 | — | Night Gal Summer (Japan 850702 NGS 0-01) |
| `pce/uapce.cpp` | 4 | — | Alien Crush (United Amusements PC Engine) |
| `philips/cdi.cpp` | 4 | — | Quizard (v1.8, German, i8751 DE 11 D3) |
| `playmark/sderby.cpp` | 4 | — | Lucky Boom |
| `sega/megadriv_ybox.cpp` | 4 | — | Juezhan Tianhuang |
| `suna/suna16.cpp` | 4 | — | Best Of Best |
| `taito/superqix.cpp` | 4 | — | Vs. Hot Smash |
| `williams/midvunit.cpp` | 4 | — | Cruis'n USA (v4.5) |
| `yunsung/yunsun16.cpp` | 4 | — | Bomb Kick (set 1) |
| `alpha/splendor.cpp` | 3 | — | High Voltage |
| `atari/atarig42.cpp` | 3 | — | Danger Express (prototype) |
| `ces/galgames.cpp` | 3 | — | Galaxy Games + StarPak 2 cartridge |
| `edevices/stlforce.cpp` | 3 | — | Mortal Race |
| `eolith/ghosteo.cpp` | 3 | — | BnB Arcade (V1.0005 World) |
| `igs/igs_fear.cpp` | 3 | — | Fearless Pinocchio (V101US) |
| `jaleco/homerun.cpp` | 3 | — | Dynamic Shoot Kyousou |
| `jaleco/scudhamm.cpp` | 3 | — | Captain Flag (Japan) |
| `misc/artmagic.cpp` | 3 | — | Cheese Chase |
| `misc/carrera.cpp` | 3 | — | Alantin - Aladdin's Magic Lamp |
| `misc/coolpool.cpp` | 3 | — | 9-Ball Shootout (set 1) |
| `misc/galaxi.cpp` | 3 | — | Galaxi (v2.0) |
| `misc/magicfly.cpp` | 3 | — | 7 e Mezzo |
| `misc/mil4000.cpp` | 3 | — | Cherry Wheel (Version 1.7) |
| `misc/miniboy7.cpp` | 3 | — | Bonanza's Joker Poker |
| `misc/photon.cpp` | 3 | — | Klad / Labyrinth (Photon System) |
| `misc/photon2.cpp` | 3 | — | Czernyj Korabl (Arcade bootleg of ZX Spectrum 'Blackbeard') |
| `misc/shangkid.cpp` | 3 | — | Chinese Hero |
| `misc/sshanghai.cpp` | 3 | — | Super Shanghai 2000 (set 1, green board) |
| `misc/tugboat.cpp` | 3 | — | The Berenstain Bears in Bigpaw's Cave |
| `misc/usgames.cpp` | 3 | — | Super Ten V8.3 |
| `namco/namcond1.cpp` | 3 | — | Abnormal Check |
| `nichibutsu/dacholer.cpp` | 3 | — | Dacholer |
| `nix/fitfight.cpp` | 3 | — | unknown fighting game 'BB' (prototype) |
| `nmk/quizdna.cpp` | 3 | — | Quiz Gakuen Paradise (Japan, ver. 1.04) |
| `promat/1945kiii.cpp` | 3 | — | 1945k III (newer, OPCX2 PCB) |
| `ramtek/hitme.cpp` | 3 | — | Barricade |
| `sega/model1.cpp` | 3 | — | Star Wars (Sega, US) |
| `sega/sg1000a.cpp` | 3 | — | Champion Boxing |
| `snk/neopcb.cpp` | 3 | — | The King of Fighters 2003 (Japan, JAMMA PCB) |
| `sunelectronics/dai3wksi.cpp` | 3 | — | Dai 3 Wakusei (Japan) |
| `sunelectronics/markham.cpp` | 3 | — | BanBam |
| `taito/taito_h.cpp` | 3 | — | Dynamite League (US) |
| `unico/unico.cpp` | 3 | — | Burglar X |
| `acorn/ertictac.cpp` | 2 | — | Erotictac/Tactic |
| `acorn/ssfindo.cpp` | 2 | — | Pang Pang Car |
| `alpha/shougi.cpp` | 2 | — | Shougi |
| `atari/atarigt.cpp` | 2 | — | Primal Rage (version 2.3, Jan 1995) |
| `atari/atarigx2.cpp` | 2 | — | Moto Frenzy |
| `atari/atarisy4.cpp` | 2 | — | Air Race (prototype) |
| `atari/runaway.cpp` | 2 | — | Qwak (prototype) |
| `comad/galspnbl.cpp` | 2 | — | Gals Pinball |
| `dataeast/bwing.cpp` | 2 | — | B-Wings (Japan new Ver.) |
| `dataeast/funkyjet.cpp` | 2 | — | Funky Jet (World, rev 1) |
| `dataeast/hvysmsh.cpp` | 2 | — | Heavy Smash (Europe version -2, 1993/06/30) |
| `edevices/twins.cpp` | 2 | — | Spider (Buena Vision, without nudity) |
| `excellent/gcpinbal.cpp` | 2 | — | Grand Cross (v1.02F) |
| `excellent/witch.cpp` | 2 | — | Keirin Ou |
| `f32/silvmil.cpp` | 2 | — | PuzzLove |
| `fuuki/fuukifg2.cpp` | 2 | — | Susume! Mile Smile / Go Go! Mile Smile (newer) |
| `fuuki/fuukifg3.cpp` | 2 | — | Asura Blade - Sword of Dynasty (Japan) |
| `gaelco/blmbycar.cpp` | 2 | — | Blomby Car (Version 1P0) |
| `igs/igs_m027_033vid.cpp` | 2 | — | Huahua Shijie 5 / Feixing Shijie (V107CN) |
| `igs/iqblock.cpp` | 2 | — | Grand Tour (V100U) |
| `igs/lordgun.cpp` | 2 | — | Alien Challenge (World) |
| `irem/m63.cpp` | 2 | — | Fighting Basketball |
| `kaneko/jchan.cpp` | 2 | — | Jackie Chan - The Kung-Fu Master (rev 4?) |
| `konami/bishi.cpp` | 2 | — | Bishi Bashi Champ Mini Game Senshuken (ver JAA, 3 Players) |
| `konami/dbz.cpp` | 2 | — | Dragon Ball Z (rev B) |
| `konami/moo.cpp` | 2 | — | Bucky O'Hare (ver EAB) |
| `konami/plygonet.cpp` | 2 | — | Polygonet Commanders (ver UAA) |
| `konami/qdrmfgp.cpp` | 2 | — | Quiz Do Re Mi Fa Grand Prix (Japan) |
| `konami/quickpick5.cpp` | 2 | — | Quick Pick 5 |
| `metro/hyprduel.cpp` | 2 | — | Hyper Duel (Japan set 1) |
| `misc/acefruit.cpp` | 2 | — | Sidewinder |
| `misc/banpresto_bpsc68000.cpp` | 2 | — | Crayon Shin-chan no Daruma Otoshi da Zo |
| `misc/dfruit.cpp` | 2 | — | Fruit Dream (Japan, Ver. 1.20) |
| `misc/efg8080.cpp` | 2 | — | Black Hole (EFG Sanremo) |
| `misc/ettrivia.cpp` | 2 | — | Progressive Music Trivia (Question set 1) |
| `misc/gluck2.cpp` | 2 | — | Good Luck II |
| `misc/kingpin.cpp` | 2 | — | Kingpin |
| `misc/ladyfrog.cpp` | 2 | — | Lady Frog |
| `misc/lependu.cpp` | 2 | — | Code Magik (Ver 5.5) / Super 7 (stealth game) |
| `misc/mcatadv.cpp` | 2 | — | Magical Cat Adventure |
| `misc/megaphx.cpp` | 2 | — | Hammer Boy |
| `misc/mosaic.cpp` | 2 | — | Golden Fire II |
| `misc/mpu12wbk.cpp` | 2 | — | Fruit Star Bonus (Ver 8.2.00ITL) |
| `misc/oneshot.cpp` | 2 | — | Mad Donna (Tuning, set 1) |
| `misc/re900.cpp` | 2 | — | Buena Suerte '94 |
| `misc/spool99.cpp` | 2 | — | Super Pool 99 (Version 0.36) |
| `misc/timelimt.cpp` | 2 | — | Progress |
| `msx/sangho.cpp` | 2 | — | Puzzle Star (Sang Ho Soft) |
| `namco/namcofl.cpp` | 2 | — | Final Lap R (Rev. B) |
| `nasco/crgolf.cpp` | 2 | — | Crowns Golf (834-5419-04) |
| `nasco/himesiki.cpp` | 2 | — | Android (prototype, later build) |
| `nichibutsu/clshroad.cpp` | 2 | — | Clash-Road |
| `nichibutsu/hyhoo.cpp` | 2 | — | Hayaoshi Taisen Quiz Hyhoo (Japan) |
| `nichibutsu/tubep.cpp` | 2 | — | Roller Jammer |
| `nix/pirates.cpp` | 2 | — | Genix Family |
| `nmk/macrossp.cpp` | 2 | — | Macross Plus |
| `olympia/lbeach.cpp` | 2 | — | Long Beach |
| `omori/battlex.cpp` | 2 | — | Battle Cross |
| `pc/pcat_nit.cpp` | 2 | — | Touchstar Bonanza (Revision 3) |
| `playmark/powerbal.cpp` | 2 | — | Magic Sticks |
| `promat/3x3puzzl.cpp` | 2 | — | 3X3 Puzzle (Enterprise) |
| `sanritsu/chinsan.cpp` | 2 | — | Ganbare Chinsan Ooshoubu (MC-8123A, 317-5012) |
| `sega/angelkds.cpp` | 2 | — | Angel Kids (Japan) |
| `sega/megadriv_sunmix.cpp` | 2 | — | Super Bubble Bobble (Sun Mixing, Mega Drive clone hardware) |
| `seibu/raiden2.cpp` | 2 | — | Raiden II (US, set 1) |
| `seta/albazg.cpp` | 2 | — | Hana Awase 6 Part II |
| `seta/macs.cpp` | 2 | — | Yu-Jan |
| `seta/speedatk.cpp` | 2 | — | Daifugo (Japan) |
| `snk/mainsnk.cpp` | 2 | — | Canvas Croquis |
| `snk/miconkit.cpp` | 2 | — | Micon-Kit Part II |
| `snk/midas.cpp` | 2 | — | Hammer |
| `stern/mazerbla.cpp` | 2 | — | Great Guns |
| `success/big10.cpp` | 2 | — | Big 10 (1985, Success) |
| `success/kurukuru.cpp` | 2 | — | Kuru Kuru Pyon Pyon (Japan) |
| `taito/40love.cpp` | 2 | — | Forty-Love (World) |
| `taito/cyclemb.cpp` | 2 | — | Cycle Maabou (Japan) |
| `taito/halleys.cpp` | 2 | — | Ben Bero Beh (Japan) |
| `taito/jollyjgr.cpp` | 2 | — | Frog & Spiders (bootleg?) |
| `taito/pitnrun.cpp` | 2 | — | Jump Kun (prototype) |
| `taito/slapshot.cpp` | 2 | — | Operation Wolf 3 (World) |
| `taito/taito_o.cpp` | 2 | — | Eibise (Japan) |
| `taito/undrfire.cpp` | 2 | — | Chase Bombers (World) |
| `tatsumi/tx1.cpp` | 2 | — | Buggy Boy/Speed Buggy (cockpit, rev. D) |
| `tch/kickgoal.cpp` | 2 | — | Action Hollywood |
| `tecfri/holeland.cpp` | 2 | — | Crazy Rally (set 1) |
| `tecmo/lvcards.cpp` | 2 | — | Lovely Cards |
| `tecmo/tecmosys.cpp` | 2 | — | Deroon DeroDero (earlier, set 1) |
| `unico/drgnmst.cpp` | 2 | — | Dragon Master (set 1) |
| `universal/sraider.cpp` | 2 | — | Mrs. Dynamite |
| `universal/zerohour.cpp` | 2 | — | Red Clash |
| `videogames/looping.cpp` | 2 | — | Looping (Europe) |
| `virtual/ldplayer.cpp` | 2 | — | Pioneer LDV-1000 Simulator |
| `vsystem/f1gp.cpp` | 2 | — | F-1 Grand Prix (set 1) |
| `vsystem/inufuku.cpp` | 2 | — | 3 On 3 Dunk Madness (US, prototype? 1997/02/04) |
| `vsystem/welltris.cpp` | 2 | — | Miyasu Nonki no Quiz 18-Kin |
| `williams/midzeus.cpp` | 2 | — | Invasion: The Abductors (version 5.0) |
| `zaccaria/galaxia.cpp` | 2 | — | Astro Wars (set 1) |
| `zaccaria/laserbat.cpp` | 2 | — | Cat and Mouse (type 02 program) |
| `zaccaria/zac1b1120.cpp` | 2 | — | Dodgem |
| `alliedleisure/ace.cpp` | 1 | — | Ace |
| `alliedleisure/clayshoo.cpp` | 1 | — | Clay Shoot |
| `alliedleisure/trvquest.cpp` | 1 | — | Trivia Quest |
| `alpha/meijinsn.cpp` | 1 | — | Meijinsen (set 1) |
| `amiga/upscope.cpp` | 1 | — | Up Scope |
| `apple/superga2.cpp` | 1 | — | Kuzmich-Egorych |
| `atari/akkaarrh.cpp` | 1 | — | Akka Arrh (prototype) |
| `atari/beathead.cpp` | 1 | — | BeatHead (prototype) |
| `atari/boxer.cpp` | 1 | — | Boxer (prototype) |
| `atari/cball.cpp` | 1 | — | Cannonball (Atari, prototype) |
| `atari/cops.cpp` | 1 | — | Revelations |
| `atari/cybstorm.cpp` | 1 | — | Cyberstorm (prototype) |
| `atari/destroyr.cpp` | 1 | — | Destroyer (Atari, version O2) |
| `atari/dragrace.cpp` | 1 | — | Drag Race |
| `atari/firefox.cpp` | 1 | — | Fire Fox (set 1) |
| `atari/flyball.cpp` | 1 | — | Flyball (rev 2) |
| `atari/marblmd2.cpp` | 1 | — | Marble Madness II (prototype) |
| `atari/mgolf.cpp` | 1 | — | Mini Golf (Atari, prototype) |
| `atari/orbit.cpp` | 1 | — | Orbit |
| `atari/poolshrk.cpp` | 1 | — | Poolshark |
| `atari/skyraid.cpp` | 1 | — | Sky Raider |
| `atari/sprint4.cpp` | 1 | — | Sprint 4 (Rev 03) |
| `atari/sprint8.cpp` | 1 | — | Sprint 8 |
| `atari/starshp1.cpp` | 1 | — | Starship 1 |
| `atari/tank8.cpp` | 1 | — | Tank 8 (set 1) |
| `atari/tomcat.cpp` | 1 | — | TomCat (prototype) |
| `atari/tourtabl.cpp` | 1 | — | Tournament Table (set 1) |
| `atari/triplhnt.cpp` | 1 | — | Triple Hunt |
| `atari/tunhunt.cpp` | 1 | — | Tunnel Hunt |
| `atari/wolfpack.cpp` | 1 | — | Wolf Pack (prototype) |
| `atlus/patapata.cpp` | 1 | — | Pata Pata Panic |
| `bally/gridlee.cpp` | 1 | — | Gridlee |
| `capcom/egghunt.cpp` | 1 | — | Egg Hunt |
| `capcom/kenseim.cpp` | 1 | — | Ken Sei Mogura: Street Fighter II (Japan 940418, Ver 1.00) |
| `capcom/supduck.cpp` | 1 | — | Super Duck |
| `cinematronics/dlair.cpp` | 1 | — | Dragon's Lair (US Rev. F2) |
| `cinematronics/embargo.cpp` | 1 | — | Embargo |
| `comad/funybubl.cpp` | 1 | — | Funny Bubble |
| `dataeast/backfire.cpp` | 1 | — | Backfire! (Data East, Japan, set 1) |
| `dataeast/chanbara.cpp` | 1 | — | Chanbara (Japan) |
| `dataeast/compgolf.cpp` | 1 | — | Competition Golf Final Round (World?, revision 3) |
| `dataeast/dassault.cpp` | 1 | — | Thunder Zone (World, Rev 1) |
| `dataeast/dblewing.cpp` | 1 | — | Double Wings (set 1) |
| `dataeast/dietgo.cpp` | 1 | — | Diet Go Go (Europe v1.1 1992.09.26, set 1) |
| `dataeast/dragngun.cpp` | 1 | — | Dragon Gun (US, LTEST-02 Version 0.00, 1992-12-08) |
| `dataeast/dreambal.cpp` | 1 | — | Dream Ball (Japan V2.4) |
| `dataeast/lemmings.cpp` | 1 | — | Lemmings (US prototype) |
| `dataeast/madalien.cpp` | 1 | — | Mad Alien (set 1) |
| `dataeast/metlclsh.cpp` | 1 | — | Metal Clash (Japan) |
| `dataeast/pktgaldx.cpp` | 1 | — | Pocket Gal Deluxe (Europe v3.00) |
| `dataeast/progolf.cpp` | 1 | — | 18 Holes Pro Golf (set 1) |
| `dataeast/sshangha.cpp` | 1 | — | Super Shanghai Dragon's Eye (World) |
| `dataeast/thedeep.cpp` | 1 | — | The Deep (Japan) |
| `dataeast/tryout.cpp` | 1 | — | Pro Baseball Skill Tryout (Japan) |
| `dgrm/blackt96.cpp` | 1 | — | Black Touch '96 |
| `dgrm/onetwo.cpp` | 1 | — | One + Two |
| `dgrm/pokechmp.cpp` | 1 | — | Poke Champ (set 1) |
| `edevices/diverboy.cpp` | 1 | — | Diver Boy |
| `edevices/mugsmash.cpp` | 1 | — | Mug Smashers |
| `edevices/mwarr.cpp` | 1 | — | Mighty Warriors (24/1) |
| `edevices/ppmast93.cpp` | 1 | — | Ping Pong Masters '93 |
| `edevices/pzletime.cpp` | 1 | — | Puzzle Time (prototype) |
| `efo/nightmare.cpp` | 1 | — | Night Mare (Spain) |
| `excellent/aquarium.cpp` | 1 | — | Aquarium (US) |
| `excellent/d9final.cpp` | 1 | — | Dream 9 Final (v2.24) |
| `excellent/dblcrown.cpp` | 1 | — | Double Crown (v1.0.3) |
| `excellent/es9606.cpp` | 1 | — | Keirin Derby II |
| `exidy/carpolo.cpp` | 1 | — | Car Polo |
| `exidy/vertigo.cpp` | 1 | — | Top Gunner (Exidy) |
| `gaelco/glass.cpp` | 1 | — | Glass (ver 1.1, Break Edition, checksum 49D5E66B, Version 1994, set 1) |
| `gaelco/mastboy.cpp` | 1 | — | Master Boy (Spain, set 1, checksum E49B) |
| `gaelco/mastboyo.cpp` | 1 | — | Master Boy (1987, Z80 hardware, Covielsa, set 1) |
| `gaelco/targeth.cpp` | 1 | — | Target Hits (ver 1.1, checksum 5152) |
| `gaelco/thoop2.cpp` | 1 | — | TH Strikes Back (non North America, version 1.0, checksum 020EB356) |
| `gaelco/wrally.cpp` | 1 | — | World Rally Championship (version 1.0, checksum DE0D, 08 Nov 1993) |
| `gaelco/xorworld.cpp` | 1 | — | Xor World (ver 1.2, checksum DB5D0F, prototype) |
| `gaelco/xorworld_ms.cpp` | 1 | — | Xor World (Modular System, prototype, set 1) |
| `galaxian/119.cpp` | 1 | — | 119 (bootleg?) |
| `galaxian/dambustr.cpp` | 1 | — | Dambusters (US, set 1) |
| `galaxian/galaxian_rockclim.cpp` | 1 | — | Rock Climber |
| `ice/skimaxx.cpp` | 1 | — | Skimaxx |
| `igs/5clown.cpp` | 1 | — | Five Clown (English, set 1) |
| `igs/igs009.cpp` | 1 | — | Grand Prix '98 (V100K, set 1) |
| `igs/igs_m027_023vid.cpp` | 1 | — | Mingxing San Que Yi (China, V201CN) |
| `igs/jackie.cpp` | 1 | — | Happy Jackie (v119U) |
| `irem/m58.cpp` | 1 | — | 10-Yard Fight (World, set 1) |
| `jaleco/dday.cpp` | 1 | — | D-Day (Jaleco, set 1) |
| `jaleco/fcombat.cpp` | 1 | — | Field Combat |
| `jaleco/pturn.cpp` | 1 | — | Parallel Turn |
| `kaneko/djboy.cpp` | 1 | — | DJ Boy (World) |
| `kaneko/galpani3.cpp` | 1 | — | Gals Panic 3 (Euro) |
| `kaneko/hvyunit.cpp` | 1 | — | Heavy Unit (World) |
| `kaneko/sandscrp.cpp` | 1 | — | Sand Scorpion |
| `konami/asterix.cpp` | 1 | — | Asterix (ver EAD) |
| `konami/chqflag.cpp` | 1 | — | Chequered Flag |
| `konami/divebomb.cpp` | 1 | — | Kyuukoukabakugekitai - Dive Bomber Squad (Japan, prototype) |
| `konami/gijoe.cpp` | 1 | — | G.I. Joe (World, EAB) |
| `konami/hexion.cpp` | 1 | — | Hexion (Japan ver JAB) |
| `konami/kontest.cpp` | 1 | — | Konami Test Board (GX800, Japan) |
| `konami/lethal.cpp` | 1 | — | Lethal Enforcers (ver UAE, 11/19/92 15:04) |
| `konami/mogura.cpp` | 1 | — | Mogura Desse (Japan) |
| `konami/piratesh.cpp` | 1 | — | Pirate Ship (ver UAA) |
| `konami/tgtpanic.cpp` | 1 | — | Target Panic |
| `konami/xexex.cpp` | 1 | — | Xexex (ver EAA) |
| `matic/barata.cpp` | 1 | — | Dona Barata (early prototype) |
| `meadows/warpsped.cpp` | 1 | — | Warp Speed (prototype) |
| `metro/rabbit.cpp` | 1 | — | Rabbit (Asia 3/6) |
| `midw8080/rotaryf.cpp` | 1 | — | Rotary Fighter |
| `misc/39in1.cpp` | 1 | — | 39 in 1 MAME bootleg (GNO-V000) |
| `misc/alinvade.cpp` | 1 | — | Alien Invaders |
| `misc/amspdwy.cpp` | 1 | — | American Speedway (set 1) |
| `misc/attckufo.cpp` | 1 | — | Attack UFO |
| `misc/babysuprem.cpp` | 1 | — | Baby Suprem |
| `misc/beaminv.cpp` | 1 | — | Beam Invader |
| `misc/beezer.cpp` | 1 | — | Beezer (version 9.0) |
| `misc/cardline.cpp` | 1 | — | Card Line |
| `misc/chance32.cpp` | 1 | — | Chance Thirty Two |
| `misc/chsuper.cpp` | 1 | — | Champion Super 3 (V0.35) |
| `misc/cocoloco.cpp` | 1 | — | Coco Loco (set 1) |
| `misc/cubeqst.cpp` | 1 | — | Cube Quest (01/04/84) |
| `misc/cybertnk.cpp` | 1 | — | Cyber Tank (v1.4) |
| `misc/discoboy.cpp` | 1 | — | Disco Boy |
| `misc/dominob.cpp` | 1 | — | Domino Block |
| `misc/dorachan.cpp` | 1 | — | Dora-chan (Japan) |
| `misc/dynadice.cpp` | 1 | — | Dynamic Dice |
| `misc/efdt.cpp` | 1 | — | El Fin Del Tiempo |
| `misc/enigma2.cpp` | 1 | — | Enigma II |
| `misc/esripsys.cpp` | 1 | — | Turbo Sub (prototype rev. TSCA) |
| `misc/flipjack.cpp` | 1 | — | Flipper Jack |
| `misc/flower.cpp` | 1 | — | Flower (US) |
| `misc/fortecar.cpp` | 1 | — | Forte Card (Ver 110, Spanish) |
| `misc/galgame.cpp` | 1 | — | Galaxy Game |
| `misc/gameace.cpp` | 1 | — | Hot Body I |
| `misc/good.cpp` | 1 | — | Good (Korea) |
| `misc/gotcha.cpp` | 1 | — | Got-cha Mini Game Festival |
| `misc/gunpey.cpp` | 1 | — | Gunpey (Japan) |
| `misc/hotblock.cpp` | 1 | — | Hot Blocks - Tetrix II (set 1) |
| `misc/hotchili.cpp` | 1 | — | Hot Chilli (95103, v0104) |
| `misc/imolagp.cpp` | 1 | — | Imola Grand Prix (set 1) |
| `misc/jackhouse.cpp` | 1 | — | Jack House |
| `misc/kas89.cpp` | 1 | — | Kasino '89 |
| `misc/laserbas.cpp` | 1 | — | Future Flash (set 1) |
| `misc/luckybal.cpp` | 1 | — | Lucky Ball 96 (Ver 4.01) |
| `misc/magtouch.cpp` | 1 | — | Magical Touch |
| `misc/meyc8088.cpp` | 1 | — | Golden Arrow (Standard G8-03) |
| `misc/mirax.cpp` | 1 | — | Mirax (set 1) |
| `misc/murogem.cpp` | 1 | — | Muroge Monaco (set 1) |
| `misc/murogmbl.cpp` | 1 | — | Slot (unknown bootleg?) |
| `misc/musclem.cpp` | 1 | — | Muscle Master |
| `misc/news.cpp` | 1 | — | News (set 1) |
| `misc/pachifev.cpp` | 1 | — | Pachifever |
| `misc/paracaidista.cpp` | 1 | — | Paracaidista |
| `misc/pass.cpp` | 1 | — | Pass |
| `misc/pipeline.cpp` | 1 | — | Pipeline |
| `misc/pkscram.cpp` | 1 | — | PK Scramble |
| `misc/quizo.cpp` | 1 | — | Quiz Olympic (set 1) |
| `misc/sanremo.cpp` | 1 | — | Number One |
| `misc/skyarmy.cpp` | 1 | — | Sky Army |
| `misc/sliver.cpp` | 1 | — | Sliver (set 1) |
| `misc/smotor.cpp` | 1 | — | Super Motor (prototype) |
| `misc/sttechno.cpp` | 1 | — | Shamisen Brothers Vol 1 (V1.01K) |
| `misc/stuntair.cpp` | 1 | — | Stunt Air |
| `misc/tapatune.cpp` | 1 | — | Tap a Tune |
| `misc/tattack.cpp` | 1 | — | Time Attacker |
| `misc/taxidriv.cpp` | 1 | — | Taxi Driver |
| `misc/thayers.cpp` | 1 | — | Thayer's Quest (set 1) |
| `misc/toratora.cpp` | 1 | — | Tora Tora (prototype?) |
| `misc/triviaquiz.cpp` | 1 | — | Professor Trivia (set 1) |
| `misc/trivrus.cpp` | 1 | — | Trivia R Us (v1.07) |
| `misc/truco.cpp` | 1 | — | Truco-Tron |
| `misc/trvmadns.cpp` | 1 | — | Trivia Madness - Series A Question set |
| `misc/ttchamp.cpp` | 1 | — | Table Tennis Champions |
| `misc/tvg01.cpp` | 1 | — | The Boat |
| `misc/vampire.cpp` | 1 | — | Vampire (prototype?) |
| `misc/videosaa.cpp` | 1 | — | Lady Gum |
| `misc/xyonix.cpp` | 1 | — | Xyonix |
| `modelracing/dribling.cpp` | 1 | — | Dribbling (set 1) |
| `modelracing/sshot.cpp` | 1 | — | Super Shot (set 1) |
| `msx/forte2.cpp` | 1 | — | Pesadelo (bootleg of Konami Knightmare) |
| `msx/pengadvb.cpp` | 1 | — | Penguin Adventure (bootleg of MSX version, encrypted) |
| `msx/sfkick.cpp` | 1 | — | Super Free Kick (set 1) |
| `namco/20pacgal.cpp` | 1 | — | Ms. Pac-Man/Galaga - 20th Anniversary Class of 1981 Reunion (V1.08) |
| `namco/30test.cpp` | 1 | — | 30 Test (remake) |
| `namco/kungfur.cpp` | 1 | — | Kung-Fu Roushi |
| `namco/tceptor.cpp` | 1 | — | Thunder Ceptor |
| `nasco/suprgolf.cpp` | 1 | — | Super Crowns Golf (World) |
| `nichibutsu/gomoku.cpp` | 1 | — | Gomoku Narabe Renju |
| `nichibutsu/magmax.cpp` | 1 | — | Mag Max (set 1) |
| `nichibutsu/shettle.cpp` | 1 | — | Alone Shettle Crew |
| `nintendo/blockfvr.cpp` | 1 | — | Block Fever |
| `nintendo/cothello.cpp` | 1 | — | Computer Othello |
| `nintendo/famibox.cpp` | 1 | — | FamicomBox |
| `nintendo/mmagic.cpp` | 1 | — | Monkey Magic |
| `nmk/ddealer.cpp` | 1 | — | Double Dealer |
| `nmk/quizpani.cpp` | 1 | — | Quiz Panicuru Fantasy |
| `omori/carjmbre.cpp` | 1 | — | Car Jamboree |
| `omori/popper.cpp` | 1 | — | Popper |
| `omori/spaceg.cpp` | 1 | — | Space Guerrilla |
| `omori/yakyuken.cpp` | 1 | — | The Yakyuken (bootleg) |
| `orca/akazukin.cpp` | 1 | — | Akazukin (Japan) |
| `orca/sub.cpp` | 1 | — | Submarine (Sigma) |
| `pacific/mrflea.cpp` | 1 | — | The Amazing Adventures of Mr. F. Lea |
| `pc/filetto.cpp` | 1 | — | Filetto (v1.05 901009) |
| `pc/tetriskr.cpp` | 1 | — | Tetris (Korean bootleg of Mirrorsoft PC-XT Tetris) |
| `playmark/drtomy.cpp` | 1 | — | Dr. Tomy |
| `playmark/sslam.cpp` | 1 | — | Super Slam (set 1) |
| `rare/btoads.cpp` | 1 | — | Battletoads |
| `rare/xtheball.cpp` | 1 | — | X the Ball |
| `sanritsu/drmicro.cpp` | 1 | — | Dr. Micro |
| `sanritsu/koikoi.cpp` | 1 | — | Koi Koi Part 2 |
| `sanritsu/sprcros2.cpp` | 1 | — | Super Cross II (Japan, set 1) |
| `sega/calcune.cpp` | 1 | — | Calcune (Japan, prototype) |
| `sega/coolridr.cpp` | 1 | ok | Cool Riders |
| `sega/flashbeats.cpp` | 1 | — | Flash Beats |
| `sega/segajw.cpp` | 1 | — | Joker's Wild (Rev. B) |
| `sega/timetrv.cpp` | 1 | — | Time Traveler |
| `seibu/airraid.cpp` | 1 | — | Fail Gate |
| `seibu/darkmist.cpp` | 1 | — | The Lost Castle In Darkmist |
| `seibu/deadang.cpp` | 1 | — | Dead Angle |
| `seibu/feversoc.cpp` | 1 | ok | Fever Soccer |
| `seibu/metlfrzr.cpp` | 1 | — | Metal Freezer (Japan) |
| `seibu/mustache.cpp` | 1 | — | Mustache Boy (Japan) |
| `seibu/r2dx_v33.cpp` | 1 | — | Raiden II New / Raiden DX (newer V33 PCB) (Raiden DX EEPROM) |
| `seta/albazc.cpp` | 1 | — | Hanaroku |
| `seta/champbwl.cpp` | 1 | — | Championship Bowling |
| `seta/jclub2.cpp` | 1 | — | Jockey Club II (v1.12X, older hardware) |
| `seta/thedealr.cpp` | 1 | — | The Dealer (Visco) |
| `sigma/nyny.cpp` | 1 | — | New York! New York! |
| `sigma/r2dtank.cpp` | 1 | — | R2D Tank |
| `sigma/sigmab52.cpp` | 1 | — | Joker's Wild (B52 system, BP55114-V1104, Ver.054NMV) |
| `skeleton/carnival37.cpp` | 1 | — | Carnival 37 |
| `snk/bbusters.cpp` | 1 | — | Beast Busters (World) |
| `snk/drderby.cpp` | 1 | — | Derby Derby |
| `snk/mechatt.cpp` | 1 | — | Mechanized Attack (World) |
| `stern/cliffhgr.cpp` | 1 | — | Cliff Hanger (set 1) |
| `success/othello.cpp` | 1 | — | Othello (version 3.0) |
| `success/sothello.cpp` | 1 | — | Super Othello |
| `success/tonton.cpp` | 1 | — | Waku Waku Doubutsu Land TonTon (Japan) |
| `suna/go2000.cpp` | 1 | — | Go 2000 |
| `sunelectronics/ikki.cpp` | 1 | — | Ikki (Japan) |
| `taito/ashnojoe.cpp` | 1 | — | Success Joe (World) |
| `taito/bigevglf.cpp` | 1 | — | Big Event Golf (US) |
| `taito/buggychl.cpp` | 1 | — | Buggy Challenge |
| `taito/cchance.cpp` | 1 | — | Cherry Chance |
| `taito/chaknpop.cpp` | 1 | — | Chack'n Pop |
| `taito/changela.cpp` | 1 | — | Change Lanes |
| `taito/exzisus.cpp` | 1 | — | Exzisus (Japan, dedicated) |
| `taito/fgoal.cpp` | 1 | — | Field Goal (set 1) |
| `taito/galastrm.cpp` | 1 | — | Galactic Storm (Japan) |
| `taito/grchamp.cpp` | 1 | — | Grand Champion (set 1) |
| `taito/groundfx.cpp` | 1 | — | Ground Effects / Super Ground Effects (Japan) |
| `taito/gunbustr.cpp` | 1 | — | Gunbuster (World) |
| `taito/invqix.cpp` | 1 | — | Space Invaders / Qix Silver Anniversary Edition (Ver. 2.03) |
| `taito/ksayakyu.cpp` | 1 | — | Kusayakyuu |
| `taito/missb2.cpp` | 1 | — | Miss Bubble II |
| `taito/mlanding.cpp` | 1 | — | Midnight Landing (Germany) |
| `taito/msisaac.cpp` | 1 | — | Metal Soldier Isaac II |
| `taito/othunder.cpp` | 1 | — | Operation Thunderbolt (World, rev 1) |
| `taito/rollrace.cpp` | 1 | — | Fighting Roller |
| `taito/sbowling.cpp` | 1 | — | Strike Bowling |
| `taito/scyclone.cpp` | 1 | — | Space Cyclone (Japan) |
| `taito/spdheat.cpp` | 1 | — | Super Dead Heat (World) |
| `taito/sspeedr.cpp` | 1 | — | Super Speed Race |
| `taito/ssrj.cpp` | 1 | — | Super Speed Race Junior (Japan) |
| `taito/superchs.cpp` | 1 | — | Super Chase - Criminal Termination (World) |
| `taito/suprridr.cpp` | 1 | — | Super Rider |
| `taito/taitoair.cpp` | 1 | — | Top Landing (World) |
| `taito/topspeed.cpp` | 1 | — | Top Speed (World, rev 1) |
| `taito/volfied.cpp` | 1 | — | Volfied (World, rev 1) |
| `taito/wyvernf0.cpp` | 1 | — | Wyvern F-0 (Rev 1) |
| `tatsumi/cyclwarr.cpp` | 1 | — | Big Fight - Big Trouble In The Atlantic Ocean |
| `tatsumi/kingdrby.cpp` | 1 | — | King Derby (1981) |
| `tatsumi/lockon.cpp` | 1 | — | Lock-On (rev. E) |
| `tch/littlerb.cpp` | 1 | — | Little Robin |
| `tch/rltennis.cpp` | 1 | — | Reality Tennis (set 1) |
| `tch/speedspn.cpp` | 1 | — | Speed Spin |
| `tch/topdrive.cpp` | 1 | — | Top Driving (version 1.1) |
| `technos/dogfgt.cpp` | 1 | — | Acrobatic Dog-Fight |
| `technos/shadfrce.cpp` | 1 | — | Shadow Force (World, Version 3) |
| `technos/spdodgeb.cpp` | 1 | — | Super Dodge Ball (US) |
| `tecmo/spbactn.cpp` | 1 | — | Super Pinball Action (US) |
| `tecmo/tbowl.cpp` | 1 | — | Tecmo Bowl (World, set 1) |
| `tecmo/wc90.cpp` | 1 | — | Tecmo World Cup '90 (World set 1) |
| `toaplan/enmadaio.cpp` | 1 | — | Enma Daio (Japan) |
| `toaplan/fixeight.cpp` | 1 | — | FixEight (Europe) |
| `unico/goori.cpp` | 1 | — | Goori Goori |
| `unico/silkroad.cpp` | 1 | — | The Legend of Silkroad |
| `universal/getaway.cpp` | 1 | — | Get A Way (upright) |
| `upl/mouser.cpp` | 1 | — | Mouser |
| `upl/xxmissio.cpp` | 1 | — | XX Mission |
| `ussr/istrebiteli.cpp` | 1 | — | Istrebiteli |
| `ussr/special_gambl.cpp` | 1 | — | Dice game |
| `valadon/tankbust.cpp` | 1 | — | Tank Busters |
| `videogames/supertnk.cpp` | 1 | — | Super Tank |
| `videogames/video21.cpp` | 1 | — | Video 21 |
| `vsystem/crshrace.cpp` | 1 | — | Lethal Crash Race / Bakuretsu Crash Race (set 1) |
| `vsystem/gstriker.cpp` | 1 | — | Tecmo World Cup '94 (set 1) |
| `vsystem/suprslam.cpp` | 1 | — | From TV Animation Slam Dunk - Super Slams |
| `vsystem/taotaido.cpp` | 1 | — | Tao Taido (2 button version) |
| `williams/midxunit.cpp` | 1 | — | Revolution X (revision 2.0 9/8/94) |
| `williams/predators.cpp` | 1 | — | The Predators (prototype) |
| `williams/wmg.cpp` | 1 | — | Williams Multigame |
| `yachiyo/ssingles.cpp` | 1 | — | Swinging Singles (US) |
| `yachiyo/sstrangr.cpp` | 1 | — | Space Stranger |
| `zaccaria/quasar.cpp` | 1 | — | Quasar (set 1) |
| `zaccaria/seabattl.cpp` | 1 | — | Sea Battle |

## Pulled in by parent closure — 14 files

No coverage value on their own: they hold the parent of a clone in a kept file.

| file | parents | example |
|---|---:|---|
| `dataeast/supbtime.cpp` | 3 | China Town (Japan) |
| `igs/spoker.cpp` | 14 | 3 Super 8 (Italy) |
| `jaleco/bnstars.cpp` | 1 | Vs. Janshi Brandnew Stars |
| `midw8080/8080bw.cpp` | 28 | Astropal |
| `misc/epos.cpp` | 7 | Catapult |
| `nintendo/dkong.cpp` | 11 | Eight Ball Action (DK conversion) |
| `nintendo/mario.cpp` | 1 | Mario Bros. (US, revision E) |
| `nmk/nmk16.cpp` | 31 | Acrobat Mission |
| `pacman/jrpacman.cpp` | 1 | Jr. Pac-Man (11/9/83) |
| `pacman/pacman.cpp` | 27 | Ali Baba and 40 Thieves |
| `pacman/pengo.cpp` | 1 | Pengo (World, not encrypted, rev A) |
| `sega/segas16a.cpp` | 9 | Action Fighter (FD1089A 317-0018) |
| `sega/segas16b.cpp` | 35 | Ace Attacker (FD1094 317-0059) |
| `taito/taito_b.cpp` | 15 | Ashura Blaster (World) |
