"""Native launcher interaction tests; no firmware, discs, network, or runtime needed.

Run with ``python tests/launcher_ui.py`` after installing launcher requirements.
Qt uses its offscreen platform unless QT_QPA_PLATFORM is explicitly supplied.
"""
from __future__ import annotations

from copy import deepcopy
import os
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))

from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QSignalSpy, QTest
from PySide6.QtWidgets import QApplication, QProgressBar, QPushButton

from native_dialogs import GameDetailsDialog, SettingsDialog
from native_ui import MainWindow, STYLE


def game(key, name, created, played=0, disc_title=None):
    return {
        'id': key, 'title': disc_title or name, 'product': 'TEST-' + key,
        'areas': 'U', 'version': 'V1.000', 'created': created,
        'last_played': played, 'metadata': {'name': name, 'year': '1996'},
        'folder': str(Path('in-memory-library') / 'games' / key),
        'cover_path': '', 'verification': 'Not yet verified',
    }


class MemoryLibrary:
    """Only state and recorded actions; never launches or opens real files."""

    def __init__(self, games=None, configured=True):
        self.root = Path('in-memory-library')
        self.games = deepcopy(games if games is not None else [
            game('z', 'Zodiac Circuit', 20, 40),
            game('a', 'Amber Skies', 10, 20, 'AMBER DISC'),
            game('m', 'Midnight Garden', 30),
        ])
        self.settings = {'bios': 'test-bios.bin' if configured else '',
                         'interpolation': False}
        self.jobs = []
        self.calls = []
        self.state_art_requests = []
        self.launch_release = threading.Event()
        self.launch_release.set()
        self.launch_error = None

    def state(self, include_art=False):
        self.state_art_requests.append(include_art)
        return deepcopy({'settings': self.settings, 'games': self.games,
                         'jobs': self.jobs, 'library': str(self.root),
                         'igdb': False})

    def set_selected_game(self, key):
        self.calls.append(('select', key))
        self.settings['selected_game'] = key

    def start_import(self, path):
        self.calls.append(('import', path))
        key = 'job-' + str(len(self.jobs))
        self.jobs.append({'id': key, 'title': Path(path).stem, 'phase': 'Queued',
                          'progress': 0, 'status': 'running'})
        return {'job': key}

    def set_bios(self, path):
        self.calls.append(('bios', path))
        self.settings.update(bios=path, bios_name=Path(path).name)
        return self.state()

    def set_interpolation(self, enabled):
        self.calls.append(('interpolation', enabled))
        self.settings['interpolation'] = bool(enabled)
        return self.state()

    def launch(self, key):
        self.calls.append(('launch', key))
        self.launch_thread = threading.get_ident()
        if not self.launch_release.wait(3):
            raise RuntimeError('Test did not release the launch worker')
        if self.launch_error:
            raise RuntimeError(self.launch_error)
        return {'launched': True, 'pid': 123}

    def open_folder(self, key=''):
        self.calls.append(('folder', key))
        return True

    def dismiss_job(self, key):
        self.calls.append(('dismiss', key))
        self.jobs = [job for job in self.jobs
                     if job['id'] != key or job['status'] == 'running']
        return True


class LauncherUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication(['launcher-ui-tests'])
        cls.app.setStyle('Fusion')
        cls.app.setStyleSheet(STYLE)
        cls.app.setQuitOnLastWindowClosed(False)

    def setUp(self):
        self.titlebar_patch = patch.object(MainWindow, 'dark_titlebar')
        self.titlebar_patch.start()
        self.addCleanup(self.titlebar_patch.stop)
        self.windows = []
        self.lib = MemoryLibrary()
        self.window = self.open_window(self.lib)

    def open_window(self, library):
        window = MainWindow(library)
        window.timer.stop()  # Each case advances library state explicitly.
        self.windows.append(window)
        window.resize(1240, 1000)
        window.show()
        window.activateWindow()
        QTest.qWait(20)
        return window

    def tearDown(self):
        for window in self.windows:
            window.library.launch_release.set()
        for window in reversed(self.windows):
            self.wait_until(lambda: not window.workers)
            for dialog in window.dialogs:
                dialog.close()
            window.close()
            window.deleteLater()
        self.app.processEvents()

    def wait_until(self, predicate, message='UI action did not finish', timeout=3):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            QTest.qWait(10)
        self.assertTrue(predicate(), message)

    def idle(self):
        self.wait_until(lambda: not self.window.workers)

    def visible_ids(self, window=None):
        grid = (window or self.window).grid
        return [grid.item(row).data(Qt.ItemDataRole.UserRole)['id']
                for row in range(grid.count())]

    def click_game(self, key):
        row = self.visible_ids().index(key)
        item = self.window.grid.item(row)
        self.window.scroll.ensureWidgetVisible(self.window.grid)
        QTest.mouseClick(self.window.grid.viewport(), Qt.MouseButton.LeftButton,
                         pos=self.window.grid.visualItemRect(item).center())
        return item

    def action(self, widget, text):
        matches = [item for item in widget.findChildren(QPushButton)
                   if item.text().strip() == text]
        self.assertEqual(len(matches), 1, 'Action must have one unambiguous target')
        QTest.mouseClick(matches[0], Qt.MouseButton.LeftButton)

    def test_search_matches_display_and_disc_titles_and_sort_orders(self):
        self.assertEqual(self.visible_ids(), ['a', 'm', 'z'])
        self.window.sort.setCurrentIndex(1)
        self.assertEqual(self.visible_ids(), ['m', 'z', 'a'])
        self.window.sort.setCurrentIndex(2)
        self.assertEqual(self.visible_ids(), ['z', 'a', 'm'])
        self.window.search.setText('  aMbEr dIsC  ')
        self.assertEqual(self.visible_ids(), ['a'])
        self.window.search.setText('skies')
        self.assertEqual(self.visible_ids(), ['a'])
        self.window.search.clear()
        self.assertEqual(self.visible_ids(), ['z', 'a', 'm'])
        self.assertFalse(any(self.lib.state_art_requests))

    def test_click_selection_updates_spotlight_and_survives_reopening(self):
        changed = QSignalSpy(self.window.grid.currentItemChanged)
        self.click_game('m')
        self.assertGreater(changed.count(), 0)
        self.assertEqual(self.window.selected_game()['id'], 'm')
        self.assertEqual(self.window.spotlight.game['id'], 'm')
        self.assertEqual(self.lib.settings['selected_game'], 'm')
        self.assertIn(('select', 'm'), self.lib.calls)
        reopened = self.open_window(self.lib)
        self.assertEqual(reopened.selected_game()['id'], 'm')
        self.assertEqual(reopened.grid.currentItem().data(Qt.ItemDataRole.UserRole)['id'], 'm')

    def test_no_results_recovers_without_losing_selection_or_launching(self):
        self.click_game('m')
        self.window.search.setText('There is no such game')
        self.assertEqual(self.visible_ids(), [])
        self.assertTrue(self.window.empty.isVisible())
        self.assertFalse(self.window.grid.isVisible())
        QTest.mouseClick(self.window.empty_action, Qt.MouseButton.LeftButton)
        self.assertEqual(self.window.search.text(), '')
        self.assertEqual(self.visible_ids(), ['a', 'm', 'z'])
        self.assertEqual(self.window.selected, 'm')
        self.assertTrue(self.window.grid.isVisible())
        self.assertFalse(self.window.empty.isVisible())
        self.assertFalse(any(call[0] == 'launch' for call in self.lib.calls))

    def test_import_progress_completion_and_dismissal(self):
        self.lib.jobs = [{'id': 'job-1', 'title': 'Synthetic Disc',
                          'phase': 'Preparing files', 'progress': 24, 'status': 'running'}]
        self.window.refresh()
        QTest.mouseClick(self.window.imports_nav, Qt.MouseButton.LeftButton)
        self.assertEqual(self.window.view, 'imports')
        self.assertFalse(self.window.grid.isVisible())
        card = self.window.imports_layout.itemAt(0).widget()
        self.assertEqual(card.findChild(QProgressBar).value(), 24)
        self.lib.jobs[0].update(progress=81)
        self.window.refresh()
        card = self.window.imports_layout.itemAt(0).widget()
        self.assertEqual(card.findChild(QProgressBar).value(), 81)
        self.lib.jobs[0].update(status='complete', progress=100, phase='Ready to launch')
        self.lib.games.append(game('n', 'Newly Imported', 50))
        self.window.refresh()
        card = self.window.imports_layout.itemAt(0).widget()
        self.assertIsNone(card.findChild(QProgressBar))
        self.action(card, 'Dismiss')
        self.assertEqual(self.lib.jobs, [])
        QTest.mouseClick(self.window.library_nav, Qt.MouseButton.LeftButton)
        self.assertIn('n', self.visible_ids())

    def test_failed_import_can_be_dismissed_without_removing_games(self):
        original = deepcopy(self.lib.games)
        self.lib.jobs = [{'id': 'failed', 'title': 'Unreadable disc', 'phase': 'Missing track',
                          'progress': 17, 'status': 'error'}]
        self.window.refresh()
        self.assertTrue(self.window.imports.isVisible())
        card = self.window.imports_layout.itemAt(0).widget()
        self.assertIsNone(card.findChild(QProgressBar))
        self.action(card, 'Dismiss')
        self.assertEqual(self.lib.jobs, [])
        self.assertEqual(self.lib.games, original)

    def test_first_run_bios_then_native_multiple_disc_picker(self):
        self.lib = MemoryLibrary(games=[], configured=False)
        self.window = self.open_window(self.lib)
        self.assertTrue(self.window.empty.isVisible())
        self.assertTrue(self.window.notice.isVisible())
        self.assertFalse(self.window.spotlight.isVisible())
        bios_path = str(Path('synthetic') / 'console.bin')
        with patch('native_ui.QFileDialog.getOpenFileName', return_value=(bios_path, '')) as picker:
            QTest.mouseClick(self.window.empty_action, Qt.MouseButton.LeftButton)
            self.idle()
        picker.assert_called_once()
        self.assertEqual(self.lib.settings['bios'], bios_path)
        self.assertFalse(self.window.notice.isVisible())
        discs = [str(Path('synthetic') / 'First.cue'), str(Path('synthetic') / 'Second.iso')]
        with patch('native_ui.QFileDialog.getOpenFileNames', return_value=(discs, '')) as picker:
            QTest.mouseClick(self.window.add_button, Qt.MouseButton.LeftButton)
            self.idle()
        picker.assert_called_once()
        self.assertEqual([call[1] for call in self.lib.calls if call[0] == 'import'], discs)
        self.assertEqual(self.window.view, 'imports')
        self.assertEqual(len(self.lib.jobs), 2)
        self.assertTrue(self.window.add_button.isEnabled())

    def test_cancelled_picker_keeps_first_run_unchanged(self):
        self.lib = MemoryLibrary(games=[], configured=False)
        self.window = self.open_window(self.lib)
        with patch('native_ui.QFileDialog.getOpenFileName', return_value=('', '')):
            QTest.mouseClick(self.window.empty_action, Qt.MouseButton.LeftButton)
        self.assertEqual(self.lib.calls, [])
        self.assertTrue(self.window.notice.isVisible())
        self.assertFalse(self.window.workers)

    def test_keyboard_search_shortcut_and_arrow_selection(self):
        self.window.search.setText('amber')
        QTest.mouseClick(self.window.imports_nav, Qt.MouseButton.LeftButton)
        QTest.keyClick(self.window, Qt.Key.Key_K, Qt.KeyboardModifier.ControlModifier)
        self.wait_until(self.window.search.hasFocus, 'Ctrl+K did not focus search')
        self.assertEqual(self.window.view, 'library')
        self.assertEqual(self.window.search.selectedText(), 'amber')
        QTest.keyClick(self.window.search, Qt.Key.Key_Backspace)
        self.click_game('a')
        self.window.grid.setFocus()
        QTest.keyClick(self.window.grid, Qt.Key.Key_Right)
        self.assertEqual(self.window.selected, 'm')
        self.assertEqual(self.lib.settings['selected_game'], 'm')
        self.assertEqual(self.window.spotlight.game['id'], 'm')

    def test_spotlight_play_runs_once_off_ui_thread_and_recovers_controls(self):
        self.click_game('m')
        self.lib.launch_release.clear()
        QTest.mouseClick(self.window.spotlight.play_button, Qt.MouseButton.LeftButton)
        self.wait_until(lambda: ('launch', 'm') in self.lib.calls)
        self.assertTrue(self.window.launching)
        self.assertFalse(self.window.spotlight.play_button.isEnabled())
        self.assertNotEqual(self.lib.launch_thread, threading.get_ident())
        QTest.mouseClick(self.window.spotlight.play_button, Qt.MouseButton.LeftButton)
        self.assertEqual(self.lib.calls.count(('launch', 'm')), 1)
        self.lib.launch_release.set()
        self.idle()
        self.assertFalse(self.window.launching)
        self.assertTrue(self.window.spotlight.play_button.isEnabled())

    def test_launch_failure_is_visible_and_allows_retry(self):
        self.lib.launch_error = 'Synthetic runtime unavailable'
        QTest.mouseClick(self.window.spotlight.play_button, Qt.MouseButton.LeftButton)
        self.idle()
        self.assertTrue(self.window.toast.isVisible())
        self.assertIn('Synthetic runtime unavailable', self.window.toast_label.text())
        self.assertFalse(self.window.launching)
        self.assertTrue(self.window.spotlight.play_button.isEnabled())
        self.lib.launch_error = None
        QTest.mouseClick(self.window.spotlight.play_button, Qt.MouseButton.LeftButton)
        self.idle()
        self.assertEqual(len([call for call in self.lib.calls if call[0] == 'launch']), 2)

    def test_settings_toggle_persists_and_reads_external_preference_changes(self):
        QTest.mouseClick(self.window.settings_button, Qt.MouseButton.LeftButton)
        dialog = self.window.dialogs[-1]
        self.assertIsInstance(dialog, SettingsDialog)
        self.assertTrue(dialog.isVisible())
        QTest.mouseClick(dialog.interpolation, Qt.MouseButton.LeftButton,
                         pos=QPoint(8, dialog.interpolation.height() // 2))
        self.idle()
        self.assertTrue(self.lib.settings['interpolation'])
        self.assertTrue(dialog.interpolation.isChecked())
        self.assertTrue(dialog.interpolation.isEnabled())
        self.lib.settings['interpolation'] = False
        self.window.refresh()
        dialog.sync_settings()
        self.assertFalse(dialog.interpolation.isChecked())
        dialog.tabs.setCurrentIndex(1)
        self.assertTrue(dialog.tabs.currentWidget().isVisible())
        QTest.keyClick(dialog, Qt.Key.Key_Escape)
        self.assertFalse(dialog.isVisible())

    def test_details_actions_use_selected_game(self):
        self.click_game('m')
        QTest.mouseClick(self.window.spotlight.detail_button, Qt.MouseButton.LeftButton)
        dialog = self.window.dialogs[-1]
        self.assertIsInstance(dialog, GameDetailsDialog)
        self.assertEqual(dialog.game['id'], 'm')
        self.assertTrue(dialog.isVisible())
        self.action(dialog, 'Open folder')
        self.assertIn(('folder', 'm'), self.lib.calls)
        self.action(dialog, 'Play game')
        self.idle()
        self.assertFalse(dialog.isVisible())
        self.assertIn(('launch', 'm'), self.lib.calls)

    def test_double_click_and_enter_each_open_one_details_dialog(self):
        item = self.click_game('m')
        QTest.mouseDClick(self.window.grid.viewport(), Qt.MouseButton.LeftButton,
                         pos=self.window.grid.visualItemRect(item).center())
        self.app.processEvents()
        visible = [dialog for dialog in self.window.dialogs if dialog.isVisible()]
        self.assertEqual(len(visible), 1, 'One double-click opened multiple dialogs')
        self.assertEqual(visible[0].game['id'], 'm')
        visible[0].close()
        self.window.activateWindow()
        self.window.grid.setFocus()
        QTest.keyClick(self.window.grid, Qt.Key.Key_Return)
        self.app.processEvents()
        visible = [dialog for dialog in self.window.dialogs if dialog.isVisible()]
        self.assertEqual(len(visible), 1, 'Enter must activate the focused game once')
        self.assertEqual(visible[0].game['id'], 'm')


if __name__ == '__main__':
    unittest.main(verbosity=2)
