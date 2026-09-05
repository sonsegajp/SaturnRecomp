"""Native Qt Widgets library; no browser, HTML or HTTP server."""
from __future__ import annotations
import json
import math
from pathlib import Path
from PySide6.QtCore import Qt, QSize, QRectF, QPointF, QTimer, QThread, Signal, QVariantAnimation, QEasingCurve
from PySide6.QtGui import QColor, QFont, QIcon, QKeySequence, QLinearGradient, QRadialGradient, QConicalGradient, QPainter, QPainterPath, QPen, QPixmap, QShortcut
from PySide6.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QComboBox, QDialog, QFileDialog, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QListView, QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QProgressBar, QPushButton, QScrollArea, QSizePolicy, QStackedWidget, QStyledItemDelegate, QStyle, QTabWidget, QVBoxLayout, QWidget)

from theme import STYLE, PALETTE, SPACE, METRICS, color, font


def label(text='', name=''):
    item=QLabel(text)
    if name: item.setObjectName(name)
    return item

def rule():
    item=QFrame(); item.setObjectName('rule'); item.setFixedHeight(1); return item

def title(game): return game.get('metadata',{}).get('name') or game['title']

def region(game):
    for code,name in [('U','North America'),('J','Japan'),('E','Europe')]:
        if code in game.get('areas',''): return name
    return game.get('areas') or 'Sega Saturn'

def glyph(kind, icon_color=None):
    icon_color = icon_color or PALETTE['secondary']
    pix=QPixmap(40,40); pix.fill(Qt.GlobalColor.transparent)
    p=QPainter(pix); p.setRenderHint(QPainter.RenderHint.Antialiasing); p.scale(2,2)
    p.setPen(QPen(QColor(icon_color),1.4,Qt.PenStyle.SolidLine,Qt.PenCapStyle.RoundCap,Qt.PenJoinStyle.RoundJoin))
    if kind=='library':
        for x in (3,11):
            for y in (3,11): p.drawRoundedRect(QRectF(x,y,6,6),1,1)
    elif kind=='plus':
        p.drawLine(QPointF(10,4),QPointF(10,16)); p.drawLine(QPointF(4,10),QPointF(16,10))
    elif kind=='play':
        path=QPainterPath(QPointF(6,3)); path.lineTo(16,10); path.lineTo(6,17); path.closeSubpath(); p.fillPath(path,QColor(icon_color))
    elif kind=='disc':
        p.drawEllipse(QRectF(2,2,16,16)); p.drawEllipse(QRectF(8,8,4,4)); p.drawArc(QRectF(5,5,10,10),25*16,60*16)
    elif kind=='folder':
        path=QPainterPath(QPointF(2,5)); path.lineTo(8,5); path.lineTo(10,7); path.lineTo(18,7); path.lineTo(18,17); path.lineTo(2,17); path.closeSubpath(); p.drawPath(path)
    elif kind=='settings':
        for y,x in ((5,7),(10,13),(15,8)):
            p.drawLine(QPointF(3,y),QPointF(17,y)); p.setBrush(color('header')); p.drawEllipse(QRectF(x-2,y-2,4,4))
    elif kind=='import':
        p.drawLine(QPointF(10,2),QPointF(10,12)); p.drawLine(QPointF(6,8),QPointF(10,12)); p.drawLine(QPointF(14,8),QPointF(10,12))
        path=QPainterPath(QPointF(3,13)); path.lineTo(3,17); path.lineTo(17,17); path.lineTo(17,13); p.drawPath(path)
    elif kind=='search':
        p.drawEllipse(QRectF(3,3,10,10)); p.drawLine(QPointF(12,12),QPointF(17,17))
    p.end(); return QIcon(pix)

def button(text,name='',symbol='',color=None):
    item=QPushButton(text); item.setCursor(Qt.CursorShape.PointingHandCursor); item.setMinimumHeight(38)
    if name: item.setObjectName(name)
    if symbol: item.setIcon(glyph(symbol,color)); item.setIconSize(QSize(18,18))
    return item

class Task(QThread):
    succeeded=Signal(object)
    failed=Signal(str)
    def __init__(self,fn,parent=None): super().__init__(parent); self.fn=fn
    def run(self):
        try: self.succeeded.emit(self.fn())
        except Exception as error: self.failed.emit(str(error))

class Artwork:
    def __init__(self): self.cache={}
    def get(self,game):
        path=game.get('cover_path') or str(Path(game.get('folder',''))/'cover.jpg')
        try: stamp=Path(path).stat().st_mtime_ns
        except OSError: return QPixmap()
        key=path,stamp
        if key not in self.cache: self.cache[key]=QPixmap(path)
        return self.cache[key]

def draw_cover(painter, rect, game, art):
    """Keep original cover proportions; missing artwork gets a quiet title plate."""
    radius = METRICS['radius']
    path = QPainterPath()
    path.addRoundedRect(rect, radius, radius)
    painter.save()
    painter.setClipPath(path)
    painter.fillRect(rect, color('header'))
    pixmap = art.get(game)
    if not pixmap.isNull():
        size = pixmap.size().scaled(rect.size().toSize(), Qt.AspectRatioMode.KeepAspectRatio)
        target = QRectF(rect.center().x() - size.width() / 2,
                        rect.center().y() - size.height() / 2,
                        size.width(), size.height())
        painter.drawPixmap(target, pixmap, QRectF(pixmap.rect()))
    else:
        painter.fillRect(rect, color('raised'))
        painter.setPen(color('muted'))
        painter.setFont(font('caption'))
        painter.drawText(rect.adjusted(16, 16, -16, -16),
                         Qt.AlignmentFlag.AlignTop, 'SEGA SATURN')
        painter.setPen(color('text'))
        painter.setFont(font('section'))
        painter.drawText(rect.adjusted(16, 42, -16, -20),
                         Qt.AlignmentFlag.AlignVCenter | Qt.TextFlag.TextWordWrap,
                         title(game))
    painter.restore()
    painter.setPen(QPen(color('text', 30), 1))
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRoundedRect(rect, radius, radius)


class CoverWidget(QWidget):
    def __init__(self, art, game=None):
        super().__init__()
        self.art, self.game = art, game
        self.setFixedSize(174, 226)

    def paintEvent(self, event):
        if not self.game:
            return
        painter = QPainter(self)
        painter.setRenderHints(QPainter.RenderHint.Antialiasing |
                               QPainter.RenderHint.SmoothPixmapTransform)
        draw_cover(painter, QRectF(1, 1, self.width() - 2, self.height() - 2),
                   self.game, self.art)


class StageCover(CoverWidget):
    """An original cover sleeve and a reflective Saturn disc, drawn in Qt."""
    def __init__(self, art):
        super().__init__(art)
        self.setFixedSize(320, 220)

    def paintEvent(self, event):
        if not self.game:
            return
        painter = QPainter(self)
        painter.setRenderHints(QPainter.RenderHint.Antialiasing |
                               QPainter.RenderHint.SmoothPixmapTransform)
        painter.scale(self.width() / 320, self.height() / 220)
        center = QPointF(211, 112)
        sheen = QConicalGradient(center, 38)
        for position, role in ((0, 'metal'), (.12, 'raised'), (.24, 'cyan'),
                               (.38, 'text'), (.52, 'metal'), (.63, 'header'),
                               (.8, 'orange'), (.9, 'text'), (1, 'metal')):
            sheen.setColorAt(position, color(role))
        disc = QPainterPath()
        disc.addEllipse(center, 100, 100)
        disc.addEllipse(center, 13, 13)
        painter.fillPath(disc, sheen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        for radius in (96, 93, 90, 38, 35, 15):
            painter.setPen(QPen(color('header', 60), .7))
            painter.drawEllipse(center, radius, radius)
        painter.save()
        painter.translate(36, 13)
        painter.rotate(-7)
        shadow = QRectF(6, 9, 157, 203)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(color('header', 130))
        painter.drawRoundedRect(shadow, 5, 5)
        draw_cover(painter, QRectF(0, 0, 157, 203), self.game, self.art)
        painter.restore()


class Spotlight(QFrame):
    """Native selected-game stage. Only artwork transitions; controls stay still."""
    play = Signal()
    details = Signal()

    def __init__(self, art):
        super().__init__()
        self.setObjectName('inspector')
        self.setFixedHeight(252)
        self.compact = False
        self.art = art
        self.game = None
        self.tint = color('cyan')
        self.previous_tint = self.tint
        self.transition = 1.0
        self.animation = QVariantAnimation(self)
        self.animation.setDuration(240)
        self.animation.setStartValue(0.0)
        self.animation.setEndValue(1.0)
        self.animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.animation.valueChanged.connect(self.animate)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(30, 16, 24, 16)
        layout.setSpacing(32)
        copy = QVBoxLayout()
        copy.setSpacing(0)
        self.kicker = label('SELECTED GAME', 'heroKicker')
        copy.addWidget(self.kicker)
        copy.addSpacing(SPACE['md'])
        self.name = label('', 'heroTitle')
        self.name.setWordWrap(True)
        self.name.setTextFormat(Qt.TextFormat.PlainText)
        copy.addWidget(self.name)
        copy.addSpacing(SPACE['xs'])
        self.meta = label('', 'subtitle')
        copy.addWidget(self.meta)
        copy.addSpacing(SPACE['md'])
        self.description = label('', 'inspectorDescription')
        self.description.setTextFormat(Qt.TextFormat.PlainText)
        self.description.setWordWrap(True)
        self.description.setMaximumWidth(610)
        self.description.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Ignored)
        copy.addWidget(self.description, 1)
        copy.addSpacing(SPACE['lg'])
        actions = QHBoxLayout()
        actions.setSpacing(SPACE['md'])
        self.play_button = button('Play game', 'heroPlay', 'play', PALETTE['accent_text'])
        self.play_button.setMinimumSize(170, 42)
        self.play_button.clicked.connect(self.play)
        actions.addWidget(self.play_button)
        self.detail_button = button('Game details', 'ghost')
        self.detail_button.clicked.connect(self.details)
        actions.addWidget(self.detail_button)
        actions.addStretch(1)
        copy.addLayout(actions)
        layout.addLayout(copy, 1)
        self.cover = StageCover(art)
        layout.addWidget(self.cover)

    def animate(self, progress):
        self.transition = progress
        self.update()

    def cover_tint(self, game):
        pixmap = self.art.get(game)
        if pixmap.isNull():
            return color('cyan')
        sample = pixmap.toImage().scaled(24, 24)
        bins = {}
        for y in range(sample.height()):
            for x in range(sample.width()):
                pixel = sample.pixelColor(x, y)
                hue, saturation, lightness, _ = pixel.getHslF()
                if saturation > .35 and .18 < lightness < .78:
                    bucket = int(hue * 12)
                    bins[bucket] = bins.get(bucket, 0) + saturation
        hue = (max(bins, key=bins.get) + .5) / 12 if bins else .52
        return QColor.fromHslF(hue, .76, .58)

    def set_game(self, game):
        changed = not self.game or self.game['id'] != game['id']
        self.game = game
        self.name.setText(title(game))
        self.name.setToolTip(title(game))
        self.meta.setText('   \u00b7   '.join(str(value) for value in
                         ('SEGA SATURN', game.get('metadata', {}).get('year'), region(game)) if value))
        self.summary = game.get('metadata', {}).get('summary') or 'Open game details for disc information and compatibility notes.'
        self.kicker.setText('RECENTLY PLAYED' if game.get('last_played') else 'SELECTED GAME')
        self.cover.game = game
        self.cover.update()
        if changed:
            self.animation.stop()
            self.previous_tint = self.tint
            self.tint = self.cover_tint(game)
            self.animation.start()
        self.update_description()
        QTimer.singleShot(0, self.update_description)

    def update_description(self):
        if not self.game:
            return
        metrics = self.description.fontMetrics()
        bounds = self.description.contentsRect()
        text = self.summary
        while text and metrics.boundingRect(bounds, Qt.TextFlag.TextWordWrap,
                                             text).height() > bounds.height():
            words = text.rstrip('\u2026').rsplit(' ', 1)
            text = words[0] + '\u2026' if len(words) > 1 else ''
        self.description.setText(text)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        QTimer.singleShot(0, self.update_description)

    def set_compact(self, compact):
        if self.compact == compact:
            return
        self.compact = compact
        self.setFixedHeight(188 if compact else 252)
        self.cover.setFixedSize(225, 155) if compact else self.cover.setFixedSize(320, 220)
        self.name.setObjectName('compactHeroTitle' if compact else 'heroTitle')
        self.name.style().unpolish(self.name)
        self.name.style().polish(self.name)
        self.layout().setContentsMargins(30, 12 if compact else 16, 24, 12 if compact else 16)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(.5, .5, -.5, -.5)
        path = QPainterPath()
        path.addRoundedRect(rect, 8, 8)
        painter.setClipPath(path)
        painter.fillRect(rect, color('header'))
        tint = QColor.fromRgbF(*[
            old + (new - old) * self.transition for old, new in
            zip(self.previous_tint.getRgbF(), self.tint.getRgbF())])
        glow = QRadialGradient(QPointF(self.width() - 165, 105), self.width() * .7)
        tint.setAlpha(118)
        glow.setColorAt(0, tint)
        tint.setAlpha(34)
        glow.setColorAt(.6, tint)
        glow.setColorAt(1, color('header', 0))
        painter.fillRect(rect, glow)
        # Orbital paths frame the original cover without obscuring the controls.
        painter.save()
        painter.translate(self.width() - 168, 124)
        painter.rotate(-24)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        for offset, opacity in ((0, 65), (12, 24), (50, 18)):
            painter.setPen(QPen(color('metal', opacity), 1))
            painter.drawEllipse(QRectF(-226-offset, -85-offset*.4,
                                      452+offset*2, 170+offset*.8))
        painter.setPen(QPen(color('orange', 200), 2))
        painter.drawArc(QRectF(-226, -85, 452, 170), 194*16, 26*16)
        painter.restore()
        # A small hardware-style index mark gives the stage a clear leading edge.
        painter.fillRect(QRectF(0, 29, 3, 25), color('orange'))
        painter.setPen(QPen(color('metal', 62), 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawPath(path)


class GameDelegate(QStyledItemDelegate):
    def __init__(self, art, parent=None):
        super().__init__(parent)
        self.art = art

    def sizeHint(self, option, index):
        return self.parent().gridSize()

    def paint(self, painter, option, index):
        game = index.data(Qt.ItemDataRole.UserRole)
        if not game:
            return
        painter.save()
        painter.setRenderHints(QPainter.RenderHint.Antialiasing |
                               QPainter.RenderHint.SmoothPixmapTransform)
        gap = METRICS['cover_gap']
        rect = QRectF(option.rect).adjusted(4, 4, -(gap - 4), -12)
        cover = QRectF(rect.x(), rect.y(), rect.width(), rect.width() * 1.28)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        focused = bool(option.state & QStyle.StateFlag.State_HasFocus)
        draw_cover(painter, cover, game, self.art)
        if selected or hovered:
            painter.setPen(QPen(color('accent' if selected else 'border_hover'), 2))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(cover.adjusted(-1, -1, 1, 1), 6, 6)
        if focused:
            painter.setPen(QPen(color('text'), 1, Qt.PenStyle.DotLine))
            painter.drawRoundedRect(cover.adjusted(-3, -3, 3, 3), 7, 7)
        painter.setFont(font('card_title'))
        painter.setPen(color('accent' if selected else 'text'))
        text = painter.fontMetrics().elidedText(title(game), Qt.TextElideMode.ElideRight, int(rect.width()))
        painter.drawText(QRectF(rect.x(), cover.bottom() + 10, rect.width(), 21), text)
        painter.setFont(font('caption'))
        painter.setPen(color('secondary'))
        meta = '  \u00b7  '.join(str(value) for value in
                             (game.get('metadata', {}).get('year'), region(game)) if value)
        meta = painter.fontMetrics().elidedText(meta, Qt.TextElideMode.ElideRight, int(rect.width()))
        painter.drawText(QRectF(rect.x(), cover.bottom() + 32, rect.width(), 18), meta)
        painter.restore()


class GameGrid(QListWidget):
    def __init__(self, art):
        super().__init__()
        self.setViewMode(QListView.ViewMode.IconMode)
        self.setMovement(QListView.Movement.Static)
        self.setResizeMode(QListView.ResizeMode.Adjust)
        self.setWrapping(True)
        self.setUniformItemSizes(True)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setItemDelegate(GameDelegate(art, self))
        self.setAccessibleName('Game library')
        self.setMinimumHeight(240)

    def fit(self):
        width = max(300, self.viewport().width())
        columns = max(2, width // (METRICS['cover_min_width'] + METRICS['cover_gap']))
        ancestor = self.parentWidget()
        while ancestor and not isinstance(ancestor, QScrollArea):
            ancestor = ancestor.parentWidget()
        if ancestor and self.window().height() <= 720:
            available = max(180, ancestor.viewport().height() - 8)
            fitting_width = max(118, int((available - 73) / 1.28) + METRICS['cover_gap'])
            columns = max(columns, math.ceil(width / fitting_width))
        cell = width // columns
        height = int((cell - METRICS['cover_gap']) * 1.28 + 73)
        self.setGridSize(QSize(cell, height))
        self.setFixedHeight(max(1, math.ceil(self.count() / columns)) * height + 6)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.fit()


class SortCombo(QComboBox):
    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(color('secondary'), 1.4, Qt.PenStyle.SolidLine,
                            Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        x, y = self.width() - 16, self.height() / 2
        painter.drawLine(QPointF(x - 3, y - 1.5), QPointF(x, y + 1.5))
        painter.drawLine(QPointF(x, y + 1.5), QPointF(x + 3, y - 1.5))

class MainWindow(QMainWindow):
    def __init__(self, library):
        super().__init__()
        self.library = library
        self.art = Artwork()
        self.state = {}
        self.workers = set()
        self.dialogs = []
        self.view = 'library'
        self.signature = ''
        self.job_signature = ''
        self.selected = None
        self.launching = False
        self.setWindowTitle('SaturnRecomp')
        self.resize(1280, 820)
        self.setMinimumSize(900, 650)

        root = QWidget()
        self.setCentralWidget(root)
        shell = QVBoxLayout(root)
        shell.setContentsMargins(0, 0, 0, 0)
        shell.setSpacing(0)

        from visuals import SaturnHeader
        header = SaturnHeader()
        header.setObjectName('header')
        header.setFixedHeight(METRICS['header_height'])
        bar = QHBoxLayout(header)
        bar.setContentsMargins(SPACE['xl'], 0, SPACE['xl'], 0)
        bar.setSpacing(20)
        from library import ROOT
        logo = QPixmap(str(ROOT / 'assets/saturnrecomp-logo.png'))
        branding = label()
        branding.setPixmap(logo.scaledToWidth(180, Qt.TransformationMode.SmoothTransformation))
        branding.setFixedWidth(190)
        bar.addWidget(branding)
        bar.addSpacing(SPACE['xl'])
        self.library_nav = button('Library', 'nav')
        self.library_nav.setCheckable(True)
        self.library_nav.setChecked(True)
        self.library_nav.clicked.connect(lambda: self.set_view('library'))
        self.imports_nav = button('Imports', 'nav')
        self.imports_nav.setCheckable(True)
        self.imports_nav.clicked.connect(lambda: self.set_view('imports'))
        bar.addWidget(self.library_nav)
        bar.addWidget(self.imports_nav)
        bar.addStretch(1)
        self.settings_button = button('Console settings', 'utility', 'settings', PALETTE['secondary'])
        self.settings_button.clicked.connect(self.open_settings)
        bar.addWidget(self.settings_button)
        folder = button('', 'utility', 'folder', PALETTE['secondary'])
        folder.setToolTip('Open library folder')
        folder.setAccessibleName('Open library folder')
        folder.clicked.connect(lambda: self.open_folder())
        bar.addWidget(folder)
        shell.addWidget(header)

        from visuals import SaturnHeader, OrbitalBackdrop
        workspace = OrbitalBackdrop()
        workspace.setObjectName('workspace')
        columns = QVBoxLayout(workspace)
        columns.setContentsMargins(SPACE['xl'], SPACE['xl'], SPACE['xl'], SPACE['lg'])
        columns.setSpacing(SPACE['lg'])
        self.spotlight = Spotlight(self.art)
        self.spotlight.play.connect(lambda: self.play_game(self.selected_game()))
        self.spotlight.details.connect(self.open_details)
        columns.addWidget(self.spotlight)
        gallery = QWidget()
        gallery_layout = QVBoxLayout(gallery)
        gallery_layout.setContentsMargins(0, 0, 0, 0)
        gallery_layout.setSpacing(0)
        heading = QHBoxLayout()
        text = QVBoxLayout()
        text.setSpacing(6)
        self.page_title = label('Library', 'pageTitle')
        self.subtitle = label('', 'subtitle')
        text.addWidget(self.page_title)
        text.addWidget(self.subtitle)
        heading.addLayout(text)
        heading.addStretch(1)
        self.add_button = button('Add game', '', 'plus', PALETTE['secondary'])
        self.add_button.clicked.connect(self.add_games)
        heading.addWidget(self.add_button)
        gallery_layout.addLayout(heading)
        gallery_layout.addSpacing(SPACE['lg'])

        toolbar = QHBoxLayout()
        toolbar.setSpacing(12)
        self.search = QLineEdit()
        self.search.setPlaceholderText('Search games')
        self.search.setClearButtonEnabled(True)
        self.search.addAction(glyph('search', PALETTE['muted']), QLineEdit.ActionPosition.LeadingPosition)
        self.search.setAccessibleName('Search your library')
        self.search.textChanged.connect(lambda: self.render_games())
        toolbar.addWidget(self.search, 1)
        self.sort = SortCombo()
        self.sort.addItems(['Title A–Z', 'Recently added', 'Last played'])
        self.sort.setAccessibleName('Sort games')
        self.sort.setToolTip('Sort games')
        self.sort.setMinimumWidth(152)
        self.sort.currentIndexChanged.connect(lambda: self.render_games())
        toolbar.addWidget(self.sort)
        self.count = label('0', 'count')
        toolbar.addWidget(self.count)
        gallery_layout.addLayout(toolbar)
        gallery_layout.addSpacing(SPACE['lg'])

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.content = QWidget()
        body = QVBoxLayout(self.content)
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(14)
        self.notice = QFrame()
        self.notice.setObjectName('notice')
        notice = QVBoxLayout(self.notice)
        notice.setContentsMargins(20, 19, 20, 19)
        notice.addWidget(label('Set up your console', 'sectionTitle'))
        notice.addWidget(label('Choose your Saturn BIOS to start adding games.', 'muted'))
        bios_button = button('Choose BIOS', 'primary', 'disc', PALETTE['accent_text'])
        bios_button.clicked.connect(self.choose_bios)
        notice.addWidget(bios_button, 0, Qt.AlignmentFlag.AlignLeft)
        body.addWidget(self.notice)

        self.imports = QWidget()
        self.imports_layout = QVBoxLayout(self.imports)
        self.imports_layout.setContentsMargins(0, 0, 0, 0)
        self.imports_layout.setSpacing(13)
        body.addWidget(self.imports)
        self.grid = GameGrid(self.art)
        self.grid.currentItemChanged.connect(self.selection_changed)
        self.grid.itemActivated.connect(lambda _: self.open_details())
        body.addWidget(self.grid)
        self.empty = QFrame()
        empty = QVBoxLayout(self.empty)
        empty.setContentsMargins(20, 60, 20, 50)
        empty.setSpacing(14)
        disc = label()
        disc.setPixmap(glyph('disc', PALETTE['muted']).pixmap(48, 48))
        disc.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty.addWidget(disc)
        self.empty_title = label('Build your library')
        self.empty_title.setObjectName('dialogTitle')
        self.empty_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty.addWidget(self.empty_title)
        self.empty_text = label('Choose a Saturn disc to get started.', 'muted')
        self.empty_text.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty.addWidget(self.empty_text)
        self.empty_action = button('Add your first game', 'primary', 'plus', PALETTE['accent_text'])
        self.empty_action.clicked.connect(self.empty_clicked)
        empty.addWidget(self.empty_action, 0, Qt.AlignmentFlag.AlignCenter)
        body.addWidget(self.empty)
        body.addStretch(1)
        self.scroll.setWidget(self.content)
        gallery_layout.addWidget(self.scroll, 1)
        columns.addWidget(gallery, 1)
        shell.addWidget(workspace, 1)

        footer = QHBoxLayout()
        footer.setContentsMargins(SPACE['xl'], 0, SPACE['xl'], SPACE['lg'])
        self.console_status = label('', 'muted')
        self.presentation_status = label('', 'muted')
        self.art_status = label('', 'muted')
        footer.addWidget(self.console_status)
        footer.addSpacing(20)
        footer.addWidget(self.presentation_status)
        footer.addStretch(1)
        footer.addWidget(self.art_status)
        shell.addLayout(footer)
        self.breadcrumb = label()
        self.section_title = label()
        self.toast = QFrame()
        self.toast.setObjectName('notice')
        toast_layout = QHBoxLayout(self.toast)
        toast_layout.setContentsMargins(20, 8, 15, 8)
        self.toast_label = label()
        self.toast_label.setWordWrap(True)
        toast_layout.addWidget(self.toast_label, 1)
        close = button('Dismiss', 'ghost')
        close.clicked.connect(self.toast.hide)
        toast_layout.addWidget(close)
        shell.addWidget(self.toast)
        self.toast.hide()
        self.search_shortcut = QShortcut(QKeySequence('Ctrl+K'), self)
        self.search_shortcut.activated.connect(self.focus_search)
        self.add_shortcut = QShortcut(QKeySequence('Ctrl+O'), self)
        self.add_shortcut.activated.connect(self.add_games)
        self.refresh()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(1500)
        QTimer.singleShot(0, lambda: self.dark_titlebar(self))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, 'spotlight'):
            self.spotlight.set_compact(self.height() <= 720)
            QTimer.singleShot(0, self.grid.fit)

    @staticmethod
    def dark_titlebar(window):
        import sys
        if sys.platform=='win32':
            import ctypes
            enabled=ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(int(window.winId()),20,ctypes.byref(enabled),ctypes.sizeof(enabled))

    def notify(self,message):
        modal = QApplication.activeModalWidget()
        if modal is not None and hasattr(modal, 'show_message'):
            modal.show_message(message)
            return
        self.toast_label.setText(message); self.toast.show(); QTimer.singleShot(7500,self.toast.hide)

    def run_task(self,fn,success=None,controls=None):
        controls=controls or []
        for control in controls: control.setEnabled(False)
        task=Task(fn,self); self.workers.add(task)
        if success: task.succeeded.connect(success)
        task.failed.connect(self.notify)
        def finished():
            for control in controls:
                try: control.setEnabled(True)
                except RuntimeError: pass
            self.workers.discard(task); task.deleteLater(); self.refresh()
        task.finished.connect(finished); task.start()

    def refresh(self):
        try:
            state=self.library.state(include_art=False)
        except Exception as error:
            self.notify(str(error)); return
        self.state=state; games=state['games']; settings=state['settings']
        if self.selected not in {g['id'] for g in games}:
            saved=settings.get('selected_game')
            self.selected=saved if saved in {g['id'] for g in games} else max(games,key=lambda g:(g.get('last_played',0),g.get('created',0)))['id'] if games else None
        self.console_status.setText('●  Console ready' if settings.get('bios') else '○  Console setup needed')
        self.presentation_status.setText('  120 Hz presentation' if settings.get('interpolation') else '  Native presentation')
        self.art_status.setText('Cover artwork by IGDB' if state['igdb'] else 'Cover artwork optional')
        self.subtitle.setText(f"{len(games)} games \u00b7 Sega Saturn" if self.view=='library' and games else 'Your Saturn game collection.' if self.view=='library' else 'Disc preparation, all in one place.')
        self.notice.setVisible(not settings.get('bios') and self.view=='library')
        signature=json.dumps(games,sort_keys=True)
        if signature!=self.signature:
            self.signature=signature; self.render_games()
        self.render_jobs()

    def selected_game(self):
        return next((g for g in self.state.get('games',[]) if g['id']==self.selected),None)

    def render_games(self):
        games=self.state.get('games',[]); query=self.search.text().strip().casefold()
        visible=[g for g in games if query in (g['title']+' '+title(g)).casefold()]
        sort=self.sort.currentIndex()
        visible.sort(key=lambda g: (-g.get('created',0),title(g).casefold()) if sort==1 else (-g.get('last_played',0),title(g).casefold()) if sort==2 else (title(g).casefold(),))
        self.grid.blockSignals(True); self.grid.clear()
        for game in visible:
            item=QListWidgetItem(title(game)); item.setData(Qt.ItemDataRole.UserRole,game); item.setToolTip(title(game)+'\nDouble-click for game details'); self.grid.addItem(item)
            if game['id']==self.selected: self.grid.setCurrentItem(item)
        self.grid.blockSignals(False); self.grid.fit()
        self.grid.setVisible(self.view=='library' and bool(visible)); self.empty.setVisible(self.view=='library' and not visible)
        self.empty_title.setText('No games found' if games else 'Build your library')
        self.empty_text.setText('Try a different title, or clear your search.' if games else 'Add a Saturn disc. We’ll prepare it for your library.')
        self.empty_action.setText('Clear search' if games else 'Add your first game')
        self.count.setText(f'{len(visible)} / {len(games)}' if query else str(len(games)))
        game=self.selected_game(); self.spotlight.setVisible(self.view=='library' and bool(game))
        if game: self.spotlight.set_game(game)

    def selection_changed(self,item,previous):
        if item is None: return
        self.selected=item.data(Qt.ItemDataRole.UserRole)['id']; game=self.selected_game()
        if game: self.spotlight.set_game(game)
        try: self.library.set_selected_game(self.selected)
        except Exception as error: self.notify(str(error))

    def set_view(self,view):
        self.view=view; self.library_nav.setChecked(view=='library'); self.imports_nav.setChecked(view=='imports')
        self.page_title.setText('Library' if view=='library' else 'Imports'); self.breadcrumb.setText('Your collection  /  '+('Library' if view=='library' else 'Imports')); self.section_title.setText('All games' if view=='library' else 'Import activity')
        self.search.setVisible(view=='library'); self.sort.setVisible(view=='library'); self.count.setVisible(view=='library')
        self.render_games(); self.job_signature=''; self.refresh(); self.scroll.verticalScrollBar().setValue(0)

    def focus_search(self): self.set_view('library'); self.search.setFocus(); self.search.selectAll()
    def empty_clicked(self): self.search.clear() if self.state.get('games') else self.add_games()

    def render_jobs(self):
        jobs=[job for job in self.state.get('jobs',[]) if self.view=='imports' or job['status']!='complete']
        signature=json.dumps([self.view,jobs],sort_keys=True)
        if signature==self.job_signature: return
        self.job_signature=signature
        while self.imports_layout.count():
            item=self.imports_layout.takeAt(0)
            if item.widget(): item.widget().deleteLater()
        self.imports.setVisible(bool(jobs) or self.view=='imports')
        active=sum(job['status']=='running' for job in jobs)
        self.imports_nav.setText('Imports'+(f'  {active}' if active else ''))
        if not jobs and self.view=='imports':
            empty=QFrame(); layout=QVBoxLayout(empty); layout.setContentsMargins(20,64,20,64); layout.setSpacing(13)
            heading=label('You’re all caught up.'); heading.setAlignment(Qt.AlignmentFlag.AlignCenter); heading.setObjectName('dialogTitle'); layout.addWidget(heading)
            subtitle=label('Add a disc to start your next import.','muted'); subtitle.setAlignment(Qt.AlignmentFlag.AlignCenter); layout.addWidget(subtitle)
            add=button('Add a game','primary','plus',PALETTE['accent_text']); add.clicked.connect(self.add_games); layout.addWidget(add,0,Qt.AlignmentFlag.AlignCenter); self.imports_layout.addWidget(empty)
        for job in jobs:
            card=QFrame(); card.setObjectName('errorJob' if job['status']=='error' else 'job'); layout=QVBoxLayout(card); layout.setContentsMargins(19,17,19,17); layout.setSpacing(11)
            top=QHBoxLayout(); top.addWidget(label(job['title'],'sectionTitle')); top.addStretch(1)
            status='Import failed' if job['status']=='error' else 'Ready to launch' if job['status']=='complete' else f"{job['progress']}%"
            top.addWidget(label(status,'muted')); layout.addLayout(top)
            phase=label(job['phase'],'muted'); phase.setWordWrap(True); layout.addWidget(phase)
            if job['status']=='running':
                progress=QProgressBar(); progress.setRange(0,100); progress.setValue(job['progress']); progress.setTextVisible(False); progress.setFixedHeight(4); layout.addWidget(progress)
            else:
                dismiss=button('Dismiss','ghost'); dismiss.clicked.connect(lambda checked=False,key=job['id']:self.dismiss_job(key)); layout.addWidget(dismiss,0,Qt.AlignmentFlag.AlignLeft)
            self.imports_layout.addWidget(card)

    def dismiss_job(self,key):
        self.library.dismiss_job(key); self.job_signature=''; self.refresh()

    def choose_bios(self):
        path,_=QFileDialog.getOpenFileName(self,'Choose your Saturn BIOS','','Saturn BIOS (*.bin *.rom);;All files (*)')
        if not path: return
        self.run_task(lambda:self.library.set_bios(path),lambda _:self.notify('BIOS selected. Your console is ready.'),[self.add_button])

    def add_games(self):
        if not self.state.get('settings',{}).get('bios'):
            self.choose_bios(); return
        paths,_=QFileDialog.getOpenFileNames(self,'Add Saturn games — select the CUE for mixed-mode discs','','Saturn discs (*.cue *.iso *.bin *.img)')
        if not paths: return
        def added(results):
            duplicates=[result['existing'] for result in results if result.get('existing')]
            if duplicates: self.selected=duplicates[0]; self.notify('This disc is already in your library.')
            if any(result.get('job') for result in results): self.set_view('imports')
        self.run_task(lambda:[self.library.start_import(path) for path in paths],added,[self.add_button])

    def play_game(self,game=None):
        game=game or self.selected_game()
        if not game or self.launching: return
        self.launching=True
        self.notify('Starting '+title(game)+'…')
        def launched(result): self.notify(result.get('warning') or title(game)+' is opening in its game window.'); self.launching=False
        task=Task(lambda:self.library.launch(game['id']),self); self.workers.add(task)
        self.spotlight.play_button.setEnabled(False)
        task.succeeded.connect(launched); task.failed.connect(self.notify)
        def done():
            self.launching=False; self.spotlight.play_button.setEnabled(True); self.workers.discard(task); task.deleteLater(); self.refresh()
        task.finished.connect(done); task.start()

    def open_folder(self,key=''):
        try: self.library.open_folder(key)
        except Exception as error: self.notify(str(error))

    def open_settings(self):
        from native_dialogs import SettingsDialog
        dialog=SettingsDialog(self); self.dialogs.append(dialog); dialog.open(); self.dark_titlebar(dialog)

    def open_details(self):
        game=self.selected_game()
        if not game: return
        from native_dialogs import GameDetailsDialog
        dialog=GameDetailsDialog(self,game); self.dialogs.append(dialog); dialog.open(); self.dark_titlebar(dialog)

    def closeEvent(self,event):
        if self.workers:
            self.notify('Finishing the current action. Please close the launcher again in a moment.'); event.ignore(); return
        super().closeEvent(event)
