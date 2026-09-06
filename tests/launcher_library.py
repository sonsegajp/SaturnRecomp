"""Disc preparation integration tests using a synthetic ISO, without firmware."""
import importlib.util
import base64
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
from library import Library, RUNTIME_FILES

# A 0.12-second, 440 Hz stereo test tone encoded as MP3; no game audio is used.
MP3_TONE = base64.b64decode(
    '//sQZAAAAHkG04UwAAoAAA0goAABBAgzShmhAAAAADSDAAAAEsSzOM4EAEAaEx+7b4eHl5hDwlAVpWAKMNYBhoPEXRthdcaGr/98KA+Ag1wqCv2KcAbgAGOMZmNGP6OMVh2VU1NlKB7/'
    '+xJkCgPwjwbVp2AACAAADSDgAAECRB1YhOGCYAAANIAAAATwAPBGFJWCJpx2APktYdr+aWnQciEGNE8DQwRlpKSQBR0gmAKSNygw2RItgAhZkatNKXSEQOaD7n8Oy4jREBJZKBrwAPD/'
    '+xBkGoPwiAdQAZswmAAADSAAAAEBvBtXAGDAsAAANIAAAARMASsGJVHYDZGoXRqMDzgAZ51IYfQROU/9TD7tnoAAEtAwGAwGAoAAAAAEP0aqRAi0iQn5+n8NlvxuL2/z2cz/yAEd+v/'
    '7EmQtA/CHB1ADeTEIAAANIAAAAQIYG1qDYSJgAAA0gAAABFN2CCdNd/w3KfMelX+37uW8P8UBdHygPhab8SyY0G6r+OjzES+n8gYPokxBTUUzLjEwMKqqqqqqqqqqqqqqqqqqqqqqqv/'
    '7EGQ+gACJB1clYAAIAAANIKAAAQT4cW2404AQAAA0gwAAAKqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq'
    '//sSZEQAAUUgVIZg4AAAAA0gwAAAAAABpBwAACAAADSDgAAEqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq'
)

DISC_AUDIO_PROBE = r'''
#include "disc.h"
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv) {
    disc d;
    int audio = 0, decoded = 0, peak = 0;
    uint64_t nonzero = 0;
    if (argc != 2) return 2;
    if (disc_open(&d, argv[1])) { fprintf(stderr, "%s\n", d.err); return 1; }
    for (int i = 0; i < d.ntracks; ++i) {
        const disc_track *track = &d.tracks[i];
        if (track->mode != TRACK_AUDIO) continue;
        const disc_file *file = &d.files[track->file_index];
        uint32_t sectors = (uint32_t)(file->size / 2352u);
        uint32_t offset = sectors / 2u < 750u ? sectors / 2u : 750u;
        uint64_t signal = 0;
        ++audio;
        if (file->is_mp3 && file->audio_decoder && file->pcm_frames) ++decoded;
        for (uint32_t j = 0; j < 20u && offset + j < sectors; ++j) {
            int16_t pcm[1176];
            if (disc_read_raw(&d, track->start_lba + offset + j, pcm)) break;
            for (int k = 0; k < 1176; ++k) {
                int sample = pcm[k] < 0 ? -(int)pcm[k] : pcm[k];
                if (sample) ++signal;
                if (sample > peak) peak = sample;
            }
        }
        nonzero += signal;
        printf("track=%d frames=%llu nonzero=%llu\n", track->num,
            (unsigned long long)file->pcm_frames, (unsigned long long)signal);
    }
    printf("tracks=%d audio=%d decoded=%d nonzero=%llu peak=%d\n", d.ntracks,
        audio, decoded, (unsigned long long)nonzero, peak);
    disc_close(&d);
    return decoded && nonzero && peak > 64 ? 0 : 1;
}
'''

def directory_entry(name, lba, size, directory=False):
    name = name if isinstance(name, bytes) else name.encode('ascii')
    entry = bytearray(33 + len(name) + (len(name) % 2 == 0))
    entry[0] = len(entry)
    entry[2:10] = struct.pack('<I', lba) + struct.pack('>I', lba)
    entry[10:18] = struct.pack('<I', size) + struct.pack('>I', size)
    entry[25] = 2 if directory else 0
    entry[28:32] = b'\x01\x00\x00\x01'
    entry[32] = len(name)
    entry[33:33 + len(name)] = name
    return entry

def make_iso(path):
    raw = bytearray(30 * 2048)
    raw[:16] = b'SEGA SEGASATURN  '
    raw[0x20:0x2a] = b'TEST-00001'
    raw[0x2a:0x30] = b'V1.000'
    raw[0x40] = ord('U')
    title = b'LIBRARY & IMPORT TEST'
    raw[0x60:0x60 + len(title)] = title
    struct.pack_into('>II', raw, 0xf0, 0x06004000, 4)
    pvd = 16 * 2048
    raw[pvd:pvd + 7] = b'\x01CD001\x01'
    raw[pvd + 80:pvd + 88] = struct.pack('<I', 30) + struct.pack('>I', 30)
    raw[pvd + 156:pvd + 190] = directory_entry(b'\0', 20, 2048, True)
    # The first directory entry is the boot file even when another file has a lower LBA.
    entries = (directory_entry(b'\0', 20, 2048, True) + directory_entry(b'\1', 20, 2048, True)
               + directory_entry('0.BIN;1', 24, 4) + directory_entry('README.TXT;1', 22, 5))
    raw[20 * 2048:20 * 2048 + len(entries)] = entries
    raw[24 * 2048:24 * 2048 + 4] = b'BOOT'
    raw[22 * 2048:22 * 2048 + 5] = b'ASSET'
    path.write_bytes(raw)

class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'out')
        self.folder = Path(self.temp.name)
        self.lib = Library(self.folder / 'library')
        self.disc = self.folder / 'Test Disc.iso'
        make_iso(self.disc)
        bios = self.folder / 'bios.bin'
        bios.write_bytes(b'SEGA' + bytes(512 * 1024 - 4))
        self.lib.set_bios(bios)

    def tearDown(self):
        self.temp.cleanup()

    def finish(self, result):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            job = self.lib.jobs[result['job']]
            if job['status'] != 'running':
                return job
            time.sleep(.02)
        self.fail('Import did not finish')

    def import_game(self):
        with patch.object(self.lib.art, 'fetch', return_value={'status': 'Offline'}):
            job = self.finish(self.lib.start_import(self.disc))
        self.assertEqual(job['status'], 'complete', job)
        return job['game']

    def test_disc_assets_manifest_and_executable(self):
        with patch.object(self.lib.art, 'fetch', return_value={'status': 'Offline'}):
            job = self.finish(self.lib.start_import(self.disc))
        self.assertEqual(job['status'], 'complete', job)
        folder = self.lib.root / 'games' / job['game']
        self.assertEqual((folder / 'assets/0.BIN').read_bytes(), b'BOOT')
        self.assertEqual((folder / 'assets/README.TXT').read_bytes(), b'ASSET')
        manifest = ET.parse(folder / 'manifest.xml').getroot()
        self.assertEqual(manifest.attrib['title'], 'LIBRARY & IMPORT TEST')
        self.assertEqual(manifest.attrib['boot-file'], '/0.BIN')
        self.assertEqual(len(manifest.find('files')), 2)
        self.assertEqual(len(manifest.find('tracks')), 1)
        self.assertEqual((folder / (job['game'] + '.exe')).read_bytes()[:2], b'MZ')
        config = (folder / 'game.toml').read_text()
        self.assertIn('../../bios/', config)
        self.assertIn('file = "/0.BIN"', config)
        self.assertEqual(self.lib.start_import(self.disc), {'existing': job['game']})
        self.assertNotIn('client_secret', (folder / 'game.json').read_text())

    def test_invalid_disc_does_not_publish(self):
        self.disc.write_bytes(bytes(30 * 2048))
        job = self.finish(self.lib.start_import(self.disc))
        self.assertEqual(job['status'], 'error')
        self.assertEqual(self.lib.state()['games'], [])
        self.assertTrue(Path(job['log']).is_file())

    def test_invalid_bios_does_not_replace_console(self):
        previous = dict(self.lib.settings)
        invalid = self.folder / 'invalid.bin'
        invalid.write_bytes(b'invalid')
        with self.assertRaises(ValueError):
            self.lib.set_bios(invalid)
        self.assertEqual(self.lib.settings, previous)

    def test_native_toggle_is_read_back(self):
        (self.lib.root / 'settings.ini').write_text('[Video]\nInterpolation=120\n')
        self.assertTrue(self.lib.state()['settings']['interpolation'])
        self.lib.dismiss_job('missing')

    def test_runtime_target_rates_survive_launcher_toggle(self):
        ini = self.lib.root / 'settings.ini'
        for rate in (60, 75, 120, 144, 165, 239, 240):
            ini.write_text(f'[Video]\nInterpolation={rate}\nTargetHz={rate}\nInternalScale=3\n'
                           '[Audio]\nVolume=37\nMuted=1\n[Plugin]\nFutureKey=100% custom\n')
            state = self.lib.state(False)['settings']
            self.assertTrue(state['interpolation'])
            self.assertEqual(state['target_hz'], rate)
            self.lib.set_interpolation(False)
            disabled = ini.read_text()
            self.assertIn('Interpolation=0\n', disabled)
            self.assertIn(f'TargetHz={rate}\n', disabled)
            self.assertIn('InternalScale=3\n', disabled)
            self.assertIn('[Audio]\nVolume=37\nMuted=1\n', disabled)
            self.assertIn('FutureKey=100% custom\n', disabled)
            self.lib.set_interpolation(True)
            self.assertIn(f'Interpolation={rate}\n', ini.read_text())

    def test_selecting_game_preserves_fresh_overlay_settings(self):
        key = self.import_game()
        ini = self.lib.root / 'settings.ini'
        # Simulate an F1 save after the launcher has already loaded old values.
        fresh = ('; runtime user settings\n[Video]\nInterpolation=0\nTargetHz=165\n'
                 'WindowWidth=2560\nWindowHeight=1440\nFullscreen=1\n'
                 'TextureFilter=1\nAntialiasing=1\nModelSmoothing=1\n'
                 '[Audio]\nVolume=29\nMuted=1\n[Future]\nKeep=opaque:value\n')
        ini.write_text(fresh)
        before = ini.read_bytes()
        self.lib.set_selected_game(key)
        self.assertEqual(ini.read_bytes(), before)
        self.assertEqual(self.lib.settings['target_hz'], 165)
        self.assertFalse(self.lib.settings['interpolation'])
        reopened = Library(self.lib.root)
        self.assertEqual(ini.read_bytes(), before)
        self.assertEqual(reopened.state(False)['settings']['target_hz'], 165)

    def test_legacy_rate_and_malformed_values_match_runtime_defaults(self):
        ini = self.lib.root / 'settings.ini'
        ini.write_text('[video]\nInterpolation=165 ; saved by older runtime\n'
                       'CustomOption=preserve\n[Audio]\nVolume=52\n')
        self.lib.set_interpolation(False)
        self.assertIn('TargetHz=165\n', ini.read_text())
        self.assertIn('CustomOption=preserve\n', ini.read_text())
        ini.write_text('[Video]\nInterpolation=garbage\nTargetHz=9999999999999999999999999\n')
        state = self.lib.state(False)['settings']
        self.assertFalse(state['interpolation'])
        self.assertEqual(state['target_hz'], 120)

    def test_native_selection_persists_without_changing_games(self):
        key = self.import_game()
        record = self.lib.root / 'games' / key / 'game.json'
        before = record.read_bytes()
        self.lib.settings['custom_setting'] = 'preserve me'
        self.lib.set_selected_game(key)
        reopened = Library(self.lib.root)
        self.assertEqual(reopened.state(False)['settings']['selected_game'], key)
        self.assertEqual(reopened.settings['custom_setting'], 'preserve me')
        self.assertEqual(record.read_bytes(), before)
        with self.assertRaises(ValueError):
            reopened.set_selected_game('../outside')
        self.assertEqual(reopened.settings['selected_game'], key)

    def test_native_state_uses_art_paths_without_reading_images(self):
        key = self.import_game()
        cover = self.lib.root / 'games' / key / 'cover.jpg'
        cover.write_bytes(b'cover image fixture')
        with patch.object(Path, 'read_bytes', side_effect=AssertionError('Unnecessary image read')):
            state = self.lib.state(include_art=False)
        self.assertEqual(state['games'][0]['cover_path'], str(cover))
        self.assertEqual(state['games'][0]['cover'], '')
        self.assertEqual(state['logo'], '')
        self.assertTrue(self.lib.state()['games'][0]['cover'].startswith('data:image/jpeg;base64,'))

    def test_launch_passes_clean_environment_and_saves_history(self):
        key = self.import_game()
        with patch('library.subprocess.Popen') as spawn, patch.dict('os.environ', {
                'SATURN_HANDOFF': '1', 'SATURN_TEST_MODE': '1', 'SDL_AUDIODRIVER': 'dummy'}):
            spawn.return_value.pid = 314
            result = self.lib.launch(key)
        self.assertEqual(result, {'launched': True, 'pid': 314})
        env = spawn.call_args.kwargs['env']
        self.assertNotIn('SATURN_HANDOFF', env)
        self.assertNotIn('SATURN_TEST_MODE', env)
        self.assertNotIn('SDL_AUDIODRIVER', env)
        self.assertEqual(env['SATURN_SMPCFILE'], str(self.lib.root / 'games' / key / 'console.bin'))
        self.assertGreater(self.lib.state(False)['games'][0]['last_played'], 0)

    def test_launch_failure_is_not_reported_as_success(self):
        key = self.import_game()
        with patch('library.subprocess.Popen', side_effect=OSError('Cannot start the game')):
            with self.assertRaisesRegex(OSError, 'Cannot start the game'):
                self.lib.launch(key)
        self.assertNotIn('last_played', self.lib.state(False)['games'][0])

    def test_started_game_remains_successful_when_history_cannot_be_saved(self):
        key = self.import_game()
        with patch('library.subprocess.Popen') as spawn, patch('library.atomic_json', side_effect=PermissionError):
            spawn.return_value.pid = 315
            result = self.lib.launch(key)
        self.assertTrue(result['launched'])
        self.assertEqual(result['pid'], 315)
        self.assertIn('last-played time could not be saved', result['warning'])

    def test_launch_reports_missing_disc_or_bios_before_starting(self):
        key = self.import_game()
        self.disc.unlink()
        with patch('library.subprocess.Popen') as spawn:
            with self.assertRaisesRegex(ValueError, 'original disc was moved'):
                self.lib.launch(key)
            spawn.assert_not_called()
        make_iso(self.disc)
        Path(self.lib.settings['bios']).unlink()
        with patch('library.subprocess.Popen') as spawn:
            with self.assertRaisesRegex(ValueError, 'Choose your Saturn BIOS'):
                self.lib.launch(key)
            spawn.assert_not_called()

    def test_packaged_mp3_decoder_reaches_importer_and_shared_runtime(self):
        mingw = Path(os.environ.get('SATURN_MINGW_BIN', 'C:/msys64/mingw64/bin'))
        compiler = mingw / 'gcc.exe'
        if not compiler.is_file():
            self.skipTest('MP3 integration probe requires the MinGW build toolchain')
        package = self.folder / 'package'
        runtime = package / 'runtime'
        runtime.mkdir(parents=True)
        for name in RUNTIME_FILES:
            shutil.copy2(self.lib.root / 'runtime' / name, runtime / name)
        shutil.copy2(self.lib.tool('saturn-import.exe'), runtime / 'saturn-import.exe')
        shutil.copy2(self.lib.tool('saturn-game.exe'), runtime / 'saturn-game.exe')
        (self.folder / 'tone.mp3').write_bytes(MP3_TONE)
        cue = self.folder / 'complete.cue'
        cue.write_text('FILE "Test Disc.iso" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n'
                       'FILE "tone.mp3" MP3\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n')
        environment = {'PATH': str(Path(os.environ['SystemRoot']) / 'System32'),
                       'SATURN_MINGW_BIN': str(self.folder / 'missing-toolchain')}
        # Simulate the packaged source directory and an end-user PATH. Decoder
        # discovery must succeed from the supplied DLLs, not the developer PC.
        with patch('library.ROOT', package), patch.dict(os.environ, environment):
            packaged = Library(self.folder / 'portable-library')
            packaged.set_bios(self.folder / 'bios.bin')
            with patch.object(packaged.art, 'fetch', return_value={'status': 'Offline'}):
                result = packaged.start_import(cue)
                deadline = time.monotonic() + 15
                while packaged.jobs[result['job']]['status'] == 'running' and time.monotonic() < deadline:
                    time.sleep(.02)
            job = packaged.jobs[result['job']]
            self.assertEqual(job['status'], 'complete', job)
            manifest = ET.parse(packaged.root / 'games' / job['game'] / 'manifest.xml').getroot()
            self.assertEqual(len(manifest.find('tracks')), 2)
            decoder = packaged.root / 'runtime/libmpg123-0.dll'
            self.assertEqual(decoder.read_bytes(), (runtime / decoder.name).read_bytes())
            probe_source = self.folder / 'disc-audio-probe.c'
            probe_source.write_text(DISC_AUDIO_PROBE)
            probe = packaged.root / 'runtime/disc-audio-probe.exe'
            build_environment = os.environ.copy()
            build_environment['PATH'] = str(mingw) + os.pathsep + build_environment['PATH']
            build = subprocess.run([str(compiler), '-O2', '-std=c11', '-I' + str(ROOT / 'recompiler/include'),
                                    str(probe_source), str(ROOT / 'recompiler/src/disc.c'), '-o', str(probe)],
                                   capture_output=True, text=True, env=build_environment)
            self.assertEqual(build.returncode, 0, build.stderr)
            decoded = subprocess.run([str(probe), str(cue)], cwd=self.folder,
                                     capture_output=True, text=True)
            self.assertEqual(decoded.returncode, 0, decoded.stdout + decoded.stderr)
            self.assertIn('tracks=2 audio=1 decoded=1', decoded.stdout)
if __name__ == '__main__':
    unittest.main()
