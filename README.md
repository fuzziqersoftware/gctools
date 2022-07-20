# gctools

gctools is a set of tools for reading and translating video game files. These tools can understand:
- AFS archives from various Sega games (afsdump)
- GCM and TGC GameCube disc images (gcmdump)
- GSL files from Phantasy Star Online (gsldump)
- GVM files from Phantasy Star Online (gvmdump)
- RCF files from The Simpsons: Hit and Run (rcfdump)
- PAE files from Phantasy Star Online Episode III (pae2gvm)
- PRS files from Phantasy Star Online (prsd)
- Yay0 and Yaz0 files from various Nintendo games (prsd)
- AAF, BX, and AW files from Super Mario Sunshine, Luigi's Mansion, Pikmin, and other games (smsdumpbanks, smssynth)
- MIDI, INST, SONG, and related resources from classic Macintosh games (smssynth)
- Protracker/Soundtracker modules (modsynth)

## Building

- Install CMake and OpenAL if you don't have them already.
- Build and install phosg (https://github.com/fuzziqersoftware/phosg) and phosg-audio (https://github.com/fuzziqersoftware/phosg-audio).
- Install libsamplerate (http://www.mega-nerd.com/SRC/).
- Run `cmake . && make` in the root directory of gctools. Executables will be generated for each tool. These tools all build and run on macOS and Ubuntu, but are untested on other platforms.

## The tools

**afsdump** - extracts all files in an AFS archive to the current directory. Works with AFS archives found in several Sega games.
- Example: `mkdir out && cd out && afsdump ../archive.afs`

**gcmdump** - extracts all files in a GCM file (GameCube disc image) or TGC file (embedded GameCube disc image) to the current directory. You can force formats with the --gcm or --tgc options (by default gcmdump will try to figure out the file format itself).
- Example: `mkdir out && cd out && gcmdump ../image.gcm`

**gsldump** - extracts all files in a GSL archive to the current directory. This format was used in multiple versions of Phantasy Star Online for various game parameters.
- Example: `mkdir out && cd out && gsldump ../archive.gsl`

**gvmdump** - extracts all files in a GVM archive to the current directory, and converts the GVR textures to Windows BMP files if they use pixel formats that gvmdump understands (which is not all of them). Also can decode individual GVR files outside of a GVM archive.
- Example: `mkdir out && cd out && gvmdump ../archive.gvm`

**rcfdump** - extracts all files in a RCF archive to the current directory.
- Example: `mkdir out && cd out && rcfdump ../archive.rcf`

**pae2gvm** - extracts the embedded GVM from a PAE file. The decompressed PAE data is saved as <filename>.dec; the output GVM is saved as <filename>.gvm.
- Example: `pae2gvm file.pae`

**prsd** - decompresses data in PRS, Yay0, and Yaz0 formats, or compresses data in PRS format.
- Example (decompress PRS): `prsd -d < file.prs > file.bin`
- Example (compress PRS): `prsd < file.bin > file.prs`
- Example (decompress Yay0): `prsd --yay0 -d < file.yay0 > file.bin`
- Example (decompress Yaz0): `prsd --yaz0 -d < file.yaz0 > file.bin`

**smsdumpbanks** - extracts the contents of instrument and waveform banks in AAF, BX, or BAA format. Games using this format include Luigi's Mansion, Pikmin, and Super Mario Sunshine. Produces text files describing the instruments, uncompressed .wav files containing the sounds, and .bms files containing the music sequences. Before running this program, do the steps in the "Getting auxiliary files" section below.
- Example: `mkdir sms_decoded_data && smsdumpbanks sms_extracted_data/AudioRes sms_decoded_data`

**smssynth** - synthesizes and debugs music sequences in BMS or MIDI format. There are many ways to use smssynth; see the next section.

**modsynth** - synthesizes and debugs music sequences in Protracker/Soundtracker MOD format. Run it with no arguments for usage information.

### Using smssynth

**smssynth** deals with BMS and MIDI music sequence programs. It can disassemble them, convert them into .wav files, or play them in realtime. The implementation is based on reverse-engineering multiple games and not on any official source code, so sometimes the output sounds a bit different from the actual in-game music.

#### Usage for GameCube and Wii games

Before running smssynth, you may need to do the steps in the "Getting auxiliary files" section below. Also, for sequences that loop, smssynth will run forever unless you hit Ctrl+C or give a time limit.

Once you have the necessary files, you can find out what the available sequences are with the `--list` option, play sequences with the `--play` option, or produce WAV files from the sequences with the `--output-filename` option.

Here are some usage examples for GameCube games:
- List all the sequences in Luigi's Mansion: `smssynth --audiores-directory=luigis_mansion_extracted_data/AudioRes --list`
- Convert Bianco Hills (from Super Mario Sunshine) to 4-minute WAV, no Yoshi drums: `smssynth --audiores-directory=sms_extracted_data/AudioRes k_bianco.com --disable-track=15 --output-filename=k_bianco.com.wav --time-limit=240`
- Play Bianco Hills (from Super Mario Sunshine) in realtime, with Yoshi drums: `smssynth --audiores-directory=sms_extracted_data/AudioRes k_bianco.com --play`
- Play The Forest Navel (from Pikmin) in realtime: `smssynth --audiores-directory=pikmin_extracted_data/dataDir/SndData --play cave.jam`

#### Usage for Classic Mac OS games

smssynth can also disassemble and play MIDI files. This was implemented to synthesize the Classic Mac OS version of SimCity 2000's music using the original instruments, which wouldn't play on any MIDI player I tried. Some other Classic Mac OS games use the same library (SoundMusicSys), and most of these games' music resources now work with smssynth as well. To play these sequences, provide a JSON environment file produced by [resource_dasm](http://www.github.com/fuzziqersoftware/resource_dasm). Make sure not to move or rename any of the other files in the same directory as the JSON file, or it may not play properly.

SoundMusicSys environments have a lot of options, which resource_dasm packages up into a JSON file. You can produce an appropriate JSON file by running resource_dasm like `resource_dasm "Creep Night Demo Music" ./creep_night.out`. This will produce a JSON file for each SONG resource contained in the input file.

After doing this, you can play the songs with (for example) `smssynth --json-environment="./creep_night.out/Creep Night Demo Music_SONG_1000_smssynth_env.json" --play`. The `--disassemble` and `--output-filename` options also work when using JSON files (like for JAudio/BMS), but `--list` does not.

#### Compatibility

I've tested smssynth with the following GameCube games that use JAudio/BMS and assigned an approximate correctness value for each one:
- __Luigi's Mansion__: 60%. Most songs sound close to in-game audio, but a few instruments are clearly wrong and some effects are missing. I think this makes the staff roll sequence sound cooler, but I still intend to fix it.
- __Mario Kart: Double Dash!!__: 80%. All songs work; some volume effects appear to be missing so they sound a little different.
- __Pikmin__: 70%. The game uses track volume effects to change how songs sound based on what's happening in-game; smssynth doesn't do this, so the songs sound a little different from how they sound in-game but are easily recognizable.
- __Super Mario Sunshine__: 95%. Most songs sound perfect (exactly as they sound in-game); only a few are broken. Note that the game uses track 15 for Yoshi's drums; use `--disable-track=15` to silence them.
- __The Legend of Zelda: Twilight Princess__: <20%. Most songs don't play or sound terrible. Some are recognizable but don't sound like the in-game music.
- __Super Mario Galaxy__: <20%. Same as above.

Classic Mac OS games that use SoundMusicSys currently fare much better than JAudio games:
- __After Dark__: 90%. Mostly correct, but some songs (e.g. the _You Bet Your Head_ intro) seem to rely on SMOD effects that aren't implemented.
- __Castles - Siege and Conquest__: 100%
- __ClockWerx__: 100%
- __Creep Night Pinball__: 100%
- __DinoPark Tycoon__: 100%
- __Flashback__: 100%
- __Holiday Lemmings__: 100%
- __Lemmings__: 100%
- __Mario Teaches Typing__: 100%
- __Monopoly CD-ROM__: 100%, but the songs sound different than they sound in-game. This is because the original SoundMusicSys implementation drops some notes in e.g. _Free Parking_, but smssynth does not.
- __Odell Down Under__: 100%
- __Oh No! More Lemmings__: 100%
- __Prince of Persia__: 100%
- __Prince of Persia 2__: 100%. There are no SONG resources in this game; instead, use `resource_dasm --index-format=mohawk` to get the MIDI files from NISMIDI.dat and MIDISnd.dat and use the template JSON environment generated by resource_dasm from the game application. That is, provide both --json-environment and a MIDI file on the command line.
- __SimAnt__: 100%
- __SimCity 2000__: 100%
- __SimTown (demo)__: 100%. If the full version has more songs, they will probably work, but are not yet tested.
- __Snapdragon__: 100%
- __The Amazon Trail__: 100%
- __The Yukon Trail__: 100%
- __Troggle Trouble Math__: 100%
- __Ultimate Spin Doctor__: 100%
- __Widget Workshop__: 100%

### Getting auxiliary files from GameCube games

Luigi's Mansion should work without any modifications. Just point `--audiores-directory` at the directory extracted from the disc image.

#### Getting msound.aaf from Super Mario Sunshine

You'll have to copy msound.aaf into the AudioRes directory manually to use the Super Mario Sunshine tools. To do so:
- Get nintendo.szs from the disc image (use gcmdump or some other tool).
- Yaz0-decompress it (you can do this with `prsd -d --yaz0 < nintendo.szs > nintendo.szs.rarc`).
- Extract the contents of the archive (you can do this with rarcdump, which is part of [szstools](http://amnoid.de/gc/)).
- Copy msound.aaf into the AudioRes directory.

#### Getting sequence.barc from Pikmin

You'll have to manually extract the BARC data from default.dol (it's embedded somewhere in there). Open up default.dol in a hex editor and search for the ASCII string "BARC----". Starting at the location where you found "BARC----", copy at least 0x400 bytes out of default.dol and save it as sequence.barc in the SndData/Seqs/ directory. Now you should be able to run smsdumpbanks and smssynth using the Pikmin sound data. `--audiores-directory` should point to the SndData directory from the Pikmin disc (with sequence.barc manually added).

#### Getting Banks directory from Mario Kart: Double Dash

After extracting the AudioRes directory, rename the Waves subdirectory to Banks.

#### Getting files from The Legend of Zelda: Twilight Princess

The sequences are stored in a compressed RARC file, and don't appear to be listed in the environment index. (This means `--list` won't work and you'll have to specify a sequence file manually.) To get the sequences:
- Decompress the sequence file: `prsd -d --yaz0 < Seqs/Z2SoundSeqs.arc > .Seqs/Z2SoundSeqs.arc.dec`
- Extract the sequences: `rarcdump Seqs/Z2SoundSeqs.arc.dec`

#### Getting files from Super Mario Galaxy

Like Twilight Princess, the sequences are stored in a RARC archive, but this time each individual sequence is compressed, and the index is compressed too. Fortunately they're all Yaz0:

- Decompress the index file: `prsd -d --yaz0 < SMR.szs > SMR.baa`
- Extract the sequences: `rarcdump Seqs/JaiSeq.arc)`
- Decompress the sequences: `ls Seqs/JaiSeq.arc_dir/szs/*.szs | xargs -I {} bash -c "prsd -d --yaz0 < {} > {}.bms"`
