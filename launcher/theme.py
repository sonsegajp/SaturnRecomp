"""Shared visual language for the native desktop launcher.

Change palette, type and spacing here. Widgets use semantic roles rather than
embedding their own colors or font declarations.
"""
from string import Template

from PySide6.QtGui import QColor, QFont, QPalette


PALETTE = {
    'bg': '#10151b',
    'header': '#151d25',
    'panel': '#19232d',
    'raised': '#25333f',
    'border': '#344753',
    'border_hover': '#6b8997',
    'text': '#eef3f5',
    'secondary': '#adbdc7',
    'muted': '#839aa8',
    'accent': '#8de5ed',
    'accent_hover': '#c1f8fa',
    'accent_pressed': '#5bbfc9',
    'accent_text': '#09242d',
    'selection': '#234450',
    'error': '#dfa5aa',
    'error_bg': '#30242c',
    'success': '#92b8a4',
    'cyan': '#70dfeb',
    'orange': '#f6a46c',
    'metal': '#bac9d2',
}

SPACE = {'xs': 4, 'sm': 8, 'md': 12, 'lg': 16, 'xl': 24, 'xxl': 32}
METRICS = {
    'header_height': 64,
    'inspector_width': 288,
    'cover_min_width': 156,
    'cover_max_width': 208,
    'cover_gap': 20,
    'control_height': 36,
    'radius': 5,
}
TYPE = {
    'body': (12, QFont.Weight.Normal),
    'caption': (11, QFont.Weight.Normal),
    'card_title': (13, QFont.Weight.DemiBold),
    'section': (14, QFont.Weight.DemiBold),
    'title': (30, QFont.Weight.DemiBold),
    'detail_title': (36, QFont.Weight.DemiBold),
}


def color(role, alpha=None):
    value = QColor(PALETTE[role])
    if alpha is not None:
        value.setAlpha(alpha)
    return value


def font(role='body'):
    size, weight = TYPE[role]
    value = QFont('Segoe UI')
    value.setPixelSize(size)
    value.setWeight(weight)
    return value


def native_palette():
    palette = QPalette()
    for role, name in (
        (QPalette.ColorRole.Window, 'bg'),
        (QPalette.ColorRole.WindowText, 'text'),
        (QPalette.ColorRole.Base, 'panel'),
        (QPalette.ColorRole.AlternateBase, 'raised'),
        (QPalette.ColorRole.Text, 'text'),
        (QPalette.ColorRole.Button, 'raised'),
        (QPalette.ColorRole.ButtonText, 'text'),
        (QPalette.ColorRole.Highlight, 'selection'),
        (QPalette.ColorRole.HighlightedText, 'text'),
        (QPalette.ColorRole.PlaceholderText, 'muted'),
        (QPalette.ColorRole.ToolTipBase, 'raised'),
        (QPalette.ColorRole.ToolTipText, 'text'),
    ):
        palette.setColor(role, color(name))
    palette.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.Text, color('muted'))
    palette.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.ButtonText, color('muted'))
    return palette


STYLE = Template('''
QWidget { color:$text; font-family:"Segoe UI"; font-size:12px; }
QMainWindow, QDialog { background:$bg; }

QLabel { background:transparent; }
QLabel#eyebrow { color:$muted; font-size:10px; font-weight:500; letter-spacing:1px; }
QLabel#pageTitle { color:$text; font-family:"Bahnschrift"; font-size:28px; font-weight:600; }
QLabel#heroTitle { color:$text; font-family:"Bahnschrift"; font-size:36px; font-weight:600; }
QLabel#compactHeroTitle { color:$text; font-family:"Bahnschrift"; font-size:28px; font-weight:600; }
QLabel#heroKicker { color:$cyan; font-size:10px; letter-spacing:2px; font-weight:600; }
QLabel#dialogTitle, QLabel#detailTitle { color:$text; font-size:23px; font-weight:600; }
QLabel#sectionTitle { color:$text; font-size:14px; font-weight:600; }
QLabel#subsectionTitle { color:$text; font-size:13px; font-weight:600; }
QLabel#subtitle { color:$secondary; font-size:12px; }
QLabel#muted, QLabel#count { color:$muted; font-size:11px; }
QLabel#inspectorDescription { color:$secondary; font-size:12px; }
QLabel#keycap { color:$secondary; background:$panel; border:1px solid $border;
    border-radius:4px; padding:5px 9px; font-size:11px; }
QLabel#badge { color:$secondary; background:$raised; border:1px solid $border;
    border-radius:3px; padding:3px 6px; font-size:9px; letter-spacing:.5px; }
QPushButton { color:$secondary; background:$panel; border:1px solid $border;
    border-radius:5px; padding:9px 14px; font-size:12px; font-weight:500; }
QPushButton:hover { color:$text; background:$raised; border-color:$border_hover; }
QPushButton:pressed { background:$selection; border-color:$accent; }
QPushButton:focus { border-color:$accent; }
QPushButton:disabled { color:$muted; background:$panel; border-color:$border; }
QPushButton#primary, QPushButton#heroPlay { color:$accent_text;
    background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 $accent_hover,stop:1 $accent);
    border-color:$accent; font-weight:600; }
QPushButton#primary:hover, QPushButton#heroPlay:hover { background:$accent_hover; border-color:$accent_hover; }
QPushButton#primary:pressed, QPushButton#heroPlay:pressed { background:$accent_pressed; border-color:$accent_pressed; }
QPushButton#primary:focus, QPushButton#heroPlay:focus { border:2px solid $text; padding:8px 13px; }
QPushButton#primary:disabled, QPushButton#heroPlay:disabled { background:$raised; color:$muted; border-color:$border; }
QPushButton#ghost { color:$secondary; background:transparent; border-color:transparent; }
QPushButton#ghost:hover { color:$text; background:$raised; }
QPushButton#ghost:focus { border-color:$accent; }
QPushButton#nav { color:$muted; background:transparent; border:0;
    border-bottom:2px solid transparent; border-radius:0; padding:16px 17px; }
QPushButton#nav:hover { color:$text; }
QPushButton#nav:checked { color:$text; border-bottom-color:$orange; }
QPushButton#nav:focus { background:$panel; }
QPushButton#utility { color:$secondary; background:transparent; border-color:$border; padding:7px 12px; font-size:11px; }
QPushButton#utility:hover { color:$text; background:$panel; border-color:$border_hover; }
QPushButton#utility:focus { border-color:$accent; }
QLineEdit { color:$text; background:$panel; border:1px solid $border; border-radius:5px;
    padding:9px 11px; selection-color:$text; selection-background-color:$selection; }
QLineEdit:hover { border-color:$border_hover; }
QLineEdit:focus { border-color:$accent; }
QComboBox { color:$secondary; background:$panel; border:1px solid $border; border-radius:5px;
    padding:9px 12px; padding-right:28px; font-size:11px; }
QComboBox:hover { border-color:$border_hover; }
QComboBox:focus { border-color:$accent; }
QComboBox::drop-down { width:23px; border:0; }
QComboBox QAbstractItemView { color:$text; background:$raised; border:1px solid $border_hover;
    selection-background-color:$selection; selection-color:$text; padding:4px; outline:none; }
QListWidget { background:transparent; border:0; outline:none; }
QScrollArea { background:transparent; border:0; }
QScrollArea > QWidget > QWidget { background:transparent; }
QScrollBar:vertical { border:0; background:transparent; width:7px; margin:3px 0; }
QScrollBar::handle:vertical { background:$border; min-height:32px; border-radius:3px; }
QScrollBar::handle:vertical:hover { background:$border_hover; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
QFrame#rule { background:$border; max-height:1px; border:0; }

QFrame#notice { background:$selection; border:1px solid $border_hover; border-radius:5px; }
QFrame#job { background:$panel; border:1px solid $border; border-radius:5px; }
QFrame#errorJob { background:$error_bg; border:1px solid $error; border-radius:5px; }
QProgressBar { background:$raised; border:0; border-radius:2px; height:4px; }
QProgressBar::chunk { background:$accent; border-radius:2px; }
QTabWidget::pane { border:0; background:$bg; }
QTabBar::tab { padding:12px 18px; color:$muted; background:transparent; border-bottom:2px solid $border; }
QTabBar::tab:selected { color:$text; border-bottom-color:$accent; }
QTabBar::tab:hover { color:$text; }
QCheckBox { spacing:10px; color:$text; }
QCheckBox::indicator { width:18px; height:18px; }
QCheckBox:focus { color:$accent; }
QToolTip { color:$text; background:$raised; border:1px solid $border_hover; padding:7px; }
QMessageBox { background:$bg; }
''').substitute(PALETTE)
