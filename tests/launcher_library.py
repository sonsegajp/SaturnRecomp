"""Disc preparation integration tests using a synthetic ISO, without firmware."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
from library import Library

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

if __name__ == '__main__':
    unittest.main()
