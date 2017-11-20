# gctools

gctools is a set of tools for reading video game files.

## Building

Run `make` in the root directory. Executables will be generated for each tool. These tools all build and run on Mac OS X, but are untested on other platforms.

## The tools

**afsdump** - extracts all files in an AFS archive to the current directory. Works with AFS archives found in several Sega games.
- Example: `mkdir out && cd out && afsdump ../archive.afs`

**gcmdump** - extracts all files in a GCM file (GameCube disc image) or TGC file (embedded GameCube disc image) to the current directory. You can force formats with the --gcm or --tgc options (by default gcmdump will try to figure out the file format itself).
- Example: `mkdir out && cd out && gcmdump ../image.gcm`

**gsldump** - extracts all files in a GSL archive to the current directory. This format was used in multiple versions of Phantasy Star Online for various game parameters.
- Example: `mkdir out && cd out && gsldump ../archive.gsl`

**gvmdump** - extracts all files in a GVM archive to the current directory. Note: not thoroughly tested; may fail for some archives.
- Example: `mkdir out && cd out && gvmdump ../archive.gvm`

**pae2gvm** - extracts the embedded GVM from a PAE file. The decompressed PAE data is saved as <filename>.dec; the output GVM is saved as <filename>.gvm.
- Example: `pae2gvm file.pae`

**prs/prs** - decompresses data in PRS, Yay0, and Yaz0 formats, or compresses data in PRS format.
- Example (decompress PRS): `prs -d < file.prs > file.bin`
- Example (compress PRS): `prs < file.bin > file.prs`
- Example (decompress Yay0): `prs --yay0 -d < file.yay0 > file.bin`
- Example (decompress Yaz0): `prs --yaz0 -d < file.yaz0 > file.bin`

**sms/smsdumpbanks** - extracts the contents of Super Mario Sunshine instrument and waveform banks. Produces text files describing the instruments, uncompressed .wav files containing the sounds, and .bms files containing the music sequences. Before running this program, extract msound.aaf from nintendo.szs (which is a Yaz0-compressed RARC archive) and put it in the AudioRes directory.
- Example: `mkdir sms_decoded_data && smsdumpbanks sms_extracted_data/AudioRes sms_decoded_data`

**sms/smsrenderbms** - converts a Super Mario Sunshine sequence into a .wav file, or plays it in realtime. It doesn't implement everything that Nintendo's engine implements, so the output sounds a bit different from the game's output. Some tracks sound almost perfect; a few are noticeably broken and sound terrible. Note that the game uses track 15 for Yoshi's drums, which you'll have to manually disable if you don't want them. For sequences that loop, this program will run forever unless you cancel it or give a time limit. Like smsdumpbanks, this program also requires msound.aaf to be extracted manually.
- Example (convert to 4-minute WAV, no Yoshi drums): `smsrenderbms --disable-track=15 --audiores-directory=sms_extracted_data/AudioRes --sample-rate=48000 k_bianco.com --output-filename=k_bianco.com.wav --time-limit=240`
- Example (play in realtime, with Yoshi drums): `smsrenderbms --audiores-directory=sms_extracted_data/AudioRes --sample-rate=48000 k_bianco.com --linear --play`
