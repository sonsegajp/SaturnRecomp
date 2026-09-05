"""Native console preferences, control reference and game information."""
from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QCheckBox, QDialog, QFrame, QGridLayout, QHBoxLayout, QLayout,
    QScrollArea, QSizePolicy, QTabWidget, QVBoxLayout, QWidget,
)

from native_ui import CoverWidget, button, label, region, rule, title
from theme import PALETTE, SPACE


def copy_label(text, name='muted'):
    item = label(text, name)
    item.setTextFormat(Qt.TextFormat.PlainText)
    item.setWordWrap(True)
    return item


def section(text):
    return label(text, 'subsectionTitle')


def keycap(text):
    item = label(text, 'keycap')
    item.setAlignment(Qt.AlignmentFlag.AlignCenter)
    return item


class NativeDialog(QDialog):
    def __init__(self, parent):
        super().__init__(parent)
        self.host = parent
        self.setModal(True)
        self.setWindowFlag(Qt.WindowType.WindowContextHelpButtonHint, False)

    def add_feedback(self, layout):
        """Reserve inline feedback when an action reports back to this dialog."""
        self.feedback = QWidget()
        wrapper = QVBoxLayout(self.feedback)
        wrapper.setContentsMargins(0, SPACE['lg'], 0, 0)
        notice = QFrame()
        notice.setObjectName('notice')
        contents = QVBoxLayout(notice)
        contents.setContentsMargins(SPACE['lg'], SPACE['sm'], SPACE['lg'], SPACE['sm'])
        self.feedback_text = copy_label('', 'subtitle')
        self.feedback_text.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.feedback_text.setAccessibleName('Action feedback')
        contents.addWidget(self.feedback_text)
        wrapper.addWidget(notice)
        self.feedback.hide()
        layout.addWidget(self.feedback)

    def show_message(self, message):
        """Keep action feedback visible while this modal owns the interaction."""
        self.feedback_text.setText(str(message))
        self.feedback.show()

    def showEvent(self, event):
        super().showEvent(event)
        dark_titlebar = getattr(self.host, 'dark_titlebar', None)
        if dark_titlebar:
            dark_titlebar(self)


class SettingsDialog(NativeDialog):
    def __init__(self, parent):
        super().__init__(parent)
        self.setWindowTitle('Console settings · SaturnRecomp')
        self.resize(590, 580)
        self.setMinimumSize(560, 540)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(*([SPACE['xl']] * 4))
        layout.setSpacing(0)
        heading = label('Console settings', 'dialogTitle')
        layout.addWidget(heading)
        layout.addSpacing(SPACE['sm'])
        layout.addWidget(label('Set up your Saturn. Make it yours.', 'subtitle'))
        layout.addSpacing(SPACE['lg'])

        self.tabs = QTabWidget()
        self.tabs.setAccessibleName('Console preferences')
        self.tabs.addTab(self.scroll_page(self.console_page(), 'Console settings'), 'Console')
        self.tabs.addTab(self.scroll_page(self.controls_page(), 'Controls reference'), 'Controls')
        layout.addWidget(self.tabs, 1)
        self.add_feedback(layout)
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])
        footer = QHBoxLayout()
        footer.setSpacing(SPACE['lg'])
        footer.addWidget(label('Changes are saved automatically.', 'muted'))
        footer.addStretch(1)
        done = button('Done', 'primary')
        done.setMinimumWidth(96)
        done.clicked.connect(self.accept)
        footer.addWidget(done)
        layout.addLayout(footer)

        self.sync_settings()
        self.refresh_timer = QTimer(self)
        self.refresh_timer.setInterval(500)
        self.refresh_timer.timeout.connect(self.sync_settings)
        self.finished.connect(self.refresh_timer.stop)
        self.refresh_timer.start()

    @staticmethod
    def scroll_page(page, accessible_name):
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setAccessibleName(accessible_name)
        page.layout().setSizeConstraint(QLayout.SizeConstraint.SetMinAndMaxSize)
        scroll.setWidget(page)
        return scroll

    def console_page(self):
        page = QWidget()
        page.setObjectName('panelBody')
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, SPACE['lg'], 0, 0)
        layout.setSpacing(0)

        bios_row = QHBoxLayout()
        bios_row.setSpacing(SPACE['lg'])
        bios_copy = QVBoxLayout()
        bios_copy.setSpacing(SPACE['sm'])
        bios_copy.addWidget(section('Saturn BIOS'))
        self.bios_name = label('', 'muted')
        self.bios_name.setTextFormat(Qt.TextFormat.PlainText)
        self.bios_name.setWordWrap(True)
        self.bios_name.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Minimum)
        bios_copy.addWidget(self.bios_name)
        bios_copy.addWidget(label('Use your own 512 KB BIOS dump.', 'muted'))
        bios_row.addLayout(bios_copy, 1)
        choose = button('Choose BIOS', '', 'folder')
        choose.clicked.connect(lambda: self.host.choose_bios())
        bios_row.addWidget(choose)
        layout.addLayout(bios_row)
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])

        video_heading = QHBoxLayout()
        video_heading.setSpacing(SPACE['sm'])
        video_heading.addWidget(section('Presentation'))
        experimental = label('EXPERIMENTAL', 'badge')
        video_heading.addWidget(experimental)
        video_heading.addStretch(1)
        layout.addLayout(video_heading)
        layout.addSpacing(SPACE['sm'])
        self.interpolation = QCheckBox('Enable 120 Hz presentation')
        self.interpolation.setAccessibleName('Enable experimental 120 Hz presentation')
        self.interpolation.toggled.connect(self.change_interpolation)
        layout.addWidget(self.interpolation)
        layout.addSpacing(SPACE['sm'])
        layout.addWidget(copy_label('Adds intermediate frames at the original game speed.\nApplies to your next launch. Press F2 to toggle during play.'))
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])

        art_heading = QHBoxLayout()
        art_heading.setSpacing(SPACE['sm'])
        art_heading.addWidget(section('Cover artwork'))
        art_heading.addStretch(1)
        self.art_status = label('', 'badge')
        art_heading.addWidget(self.art_status)
        layout.addLayout(art_heading)
        layout.addSpacing(SPACE['sm'])
        self.art_description = copy_label('')
        layout.addWidget(self.art_description)
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])

        layout.addWidget(label('LIBRARY LOCATION', 'eyebrow'))
        layout.addSpacing(SPACE['sm'])
        self.library_path = copy_label(str(self.host.library.root))
        self.library_path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.library_path.setToolTip(str(self.host.library.root))
        layout.addWidget(self.library_path)
        layout.addStretch(1)
        return page

    def controls_page(self):
        page = QWidget()
        page.setObjectName('panelBody')
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, SPACE['lg'], 0, 0)
        layout.setSpacing(0)
        layout.addWidget(copy_label('Click the game window to play. Compatible controllers connect automatically.'))
        layout.addSpacing(SPACE['lg'])

        mappings = QGridLayout()
        mappings.setHorizontalSpacing(SPACE['lg'])
        mappings.setVerticalSpacing(SPACE['sm'])
        for column, text in enumerate(('SATURN', 'KEYBOARD', 'XBOX-STYLE PAD')):
            mappings.addWidget(label(text, 'eyebrow'), 0, column)
        for row, (action, keyboard, controller) in enumerate((
            ('Move', 'Arrows / WASD', 'D-pad / left stick'),
            ('Start', 'Enter', 'Start'),
            ('A / B / C', 'Z / X / C', 'A / B / RB'),
            ('X / Y / Z', 'R / T / Y', 'X / Y / LB'),
            ('L / R', 'Q / E', 'LT / RT'),
        ), 1):
            mappings.addWidget(label(action), row, 0)
            mappings.addWidget(keycap(keyboard), row, 1)
            mappings.addWidget(keycap(controller), row, 2)
        mappings.setColumnStretch(1, 1)
        mappings.setColumnStretch(2, 1)
        layout.addLayout(mappings)
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(section('During play'))
        layout.addSpacing(SPACE['sm'])
        shortcuts = QGridLayout()
        shortcuts.setHorizontalSpacing(SPACE['lg'])
        shortcuts.setVerticalSpacing(SPACE['sm'])
        for row, (key, action, second_key, second_action) in enumerate((
            ('Esc', 'Close game', 'Space', 'Pause / resume'),
            ('F2', '120 Hz on / off', 'F', 'Step while paused'),
        )):
            shortcuts.addWidget(keycap(key), row, 0)
            shortcuts.addWidget(label(action, 'muted'), row, 1)
            shortcuts.addWidget(keycap(second_key), row, 2)
            shortcuts.addWidget(label(second_action, 'muted'), row, 3)
        shortcuts.setColumnStretch(1, 1)
        shortcuts.setColumnStretch(3, 1)
        layout.addLayout(shortcuts)
        layout.addStretch(1)
        return page

    def sync_settings(self):
        state = self.host.state
        settings = state.get('settings', {})
        bios = settings.get('bios', '')
        name = settings.get('bios_name') or (Path(bios).name if bios else 'No BIOS selected')
        self.bios_name.setText(name)
        self.bios_name.setToolTip(bios or 'Choose your Saturn BIOS to import and play games.')
        if self.interpolation.isEnabled():
            self.interpolation.blockSignals(True)
            self.interpolation.setChecked(bool(settings.get('interpolation')))
            self.interpolation.blockSignals(False)
        connected = bool(state.get('igdb'))
        self.art_status.setText('IGDB connected' if connected else 'Optional')
        self.art_description.setText(
            'Box artwork is fetched during import using your existing IGDB connection.' if connected else
            'Connect IGDB in Odyssey to add box artwork during import. Games also work without artwork.'
        )

    def change_interpolation(self, enabled):
        def saved(_result):
            self.host.refresh()
            self.sync_settings()
        self.host.run_task(
            lambda: self.host.library.set_interpolation(enabled),
            success=saved, controls=[self.interpolation],
        )


class GameDetailsDialog(NativeDialog):
    def __init__(self, parent, game):
        super().__init__(parent)
        self.game = game
        self.setWindowTitle(title(game) + ' · SaturnRecomp')
        self.resize(640, 620)
        self.setMinimumSize(600, 570)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(*([SPACE['xl']] * 4))
        layout.setSpacing(0)
        header = QHBoxLayout()
        header.setSpacing(SPACE['xl'])
        cover = CoverWidget(parent.art, game)
        cover.setFixedSize(174, 226)
        header.addWidget(cover, 0, Qt.AlignmentFlag.AlignTop)

        info = QVBoxLayout()
        info.setSpacing(0)
        info.addWidget(label('YOUR COLLECTION', 'eyebrow'))
        info.addSpacing(SPACE['lg'])
        game_title = copy_label(title(game), 'detailTitle')
        info.addWidget(game_title)
        info.addSpacing(SPACE['lg'])
        meta = game.get('metadata', {})
        info.addWidget(copy_label('  ·  '.join(str(value) for value in (meta.get('year'), region(game)) if value), 'subtitle'))
        info.addSpacing(SPACE['sm'])
        info.addWidget(label('Sega Saturn', 'muted'))
        info.addStretch(1)
        info.addWidget(label('DISC EDITION', 'eyebrow'))
        info.addSpacing(SPACE['sm'])
        edition = '  ·  '.join(str(value) for value in (game.get('product'), game.get('version')) if value)
        info.addWidget(copy_label(edition or 'Imported Saturn disc'))
        info.addSpacing(SPACE['sm'])
        header.addLayout(info, 1)
        layout.addLayout(header)
        layout.addSpacing(SPACE['xl'])
        layout.addWidget(section('About this game'))
        layout.addSpacing(SPACE['sm'])

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setMinimumHeight(80)
        scroll.setAccessibleName('Game description')
        summary = copy_label(meta.get('summary') or 'Your disc has been added to the library. Start a game, or open its folder to view the imported files.', 'subtitle')
        summary.setAlignment(Qt.AlignmentFlag.AlignTop)
        summary.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        summary.setContentsMargins(0, 0, SPACE['sm'], 0)
        scroll.setWidget(summary)
        layout.addWidget(scroll, 1)
        layout.addSpacing(SPACE['lg'])

        note = QFrame()
        note.setObjectName('notice')
        note_layout = QVBoxLayout(note)
        note_layout.setContentsMargins(SPACE['lg'], SPACE['sm'], SPACE['lg'], SPACE['sm'])
        note_layout.setSpacing(SPACE['sm'])
        status = game.get('verification') or 'Not yet verified'
        note_layout.addWidget(copy_label('Compatibility · ' + status, 'muted'))
        note_layout.addWidget(copy_label('Importing a disc does not verify its graphics, sound or gameplay.', 'muted'))
        layout.addWidget(note)
        self.add_feedback(layout)
        layout.addSpacing(SPACE['lg'])
        layout.addWidget(rule())
        layout.addSpacing(SPACE['lg'])
        actions = QHBoxLayout()
        actions.setSpacing(SPACE['sm'])
        open_folder = button('Open folder', '', 'folder')
        open_folder.clicked.connect(lambda: self.host.open_folder(game['id']))
        actions.addWidget(open_folder)
        actions.addStretch(1)
        close = button('Close', 'ghost')
        close.clicked.connect(self.reject)
        actions.addWidget(close)
        play = button('Play game', 'primary', 'play', PALETTE['accent_text'])
        play.setMinimumWidth(120)
        play.clicked.connect(self.play)
        actions.addWidget(play)
        layout.addLayout(actions)

    def play(self):
        self.accept()
        self.host.play_game(self.game)
