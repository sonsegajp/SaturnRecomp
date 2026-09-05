"""Quiet Saturn-inspired surfaces painted by native Qt widgets.

These are ordinary layout containers. They paint underneath their children and
do not add controls, intercept input, load artwork or run animation timers.
"""
from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QLinearGradient, QPainter, QPen, QRadialGradient
from PySide6.QtWidgets import QWidget

from theme import color


def _glow(painter, center, radius, role, opacity):
    gradient = QRadialGradient(center, radius)
    gradient.setColorAt(0, color(role, opacity))
    gradient.setColorAt(.42, color(role, round(opacity * .45)))
    gradient.setColorAt(1, color(role, 0))
    painter.fillRect(
        QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2),
        gradient,
    )


def _arc(painter, bounds, start, span, role, opacity, width=1):
    pen = QPen(color(role, opacity), width)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    painter.setPen(pen)
    painter.drawArc(bounds, round(start * 16), round(span * 16))


class SaturnHeader(QWidget):
    """Dark metal header with restrained, right-aligned orbital detailing."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

    def paintEvent(self, event):
        painter = QPainter(self)
        bounds = QRectF(self.rect())
        width, height = bounds.width(), bounds.height()
        painter.fillRect(bounds, color('header'))

        metal = QLinearGradient(0, 0, 0, height)
        metal.setColorAt(0, color('text', 12))
        metal.setColorAt(.48, color('text', 3))
        metal.setColorAt(1, color('text', 0))
        painter.fillRect(bounds, metal)

        # Fixed, very fine lines suggest brushed metal without noise or shimmer.
        for index, y in enumerate(range(2, self.height(), 3)):
            painter.setPen(QPen(color('text', 3 + index % 3), 1))
            painter.drawLine(QPointF(0, y + .5), QPointF(width, y + .5))

        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        _glow(painter, QPointF(width * .78, height * .20), 290, 'cyan', 10)

        # The motif stays on the right and below the contrast of the controls.
        painter.save()
        motif_left = max(width * .56, width - 480)
        painter.setClipRect(QRectF(motif_left, 0, width - motif_left, height))
        painter.translate(width - 185, height * .45)
        painter.rotate(-19)
        _arc(painter, QRectF(-202, -45, 404, 90), 10, 290, 'text', 18)
        _arc(painter, QRectF(-171, -36, 342, 72), -22, 245, 'cyan', 22)
        _arc(painter, QRectF(-139, -28, 278, 56), 65, 205, 'text', 11)
        _arc(painter, QRectF(-31, -31, 62, 62), 8, 280, 'text', 12)
        painter.restore()

        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        painter.setPen(QPen(color('text', 16), 1))
        painter.drawLine(QPointF(0, .5), QPointF(width, .5))
        painter.setPen(QPen(color('border', 220), 1))
        painter.drawLine(QPointF(0, height - .5), QPointF(width, height - .5))
        edge = QLinearGradient(0, 0, width, 0)
        edge.setColorAt(0, color('cyan', 0))
        edge.setColorAt(.32, color('cyan', 36))
        edge.setColorAt(.76, color('orange', 24))
        edge.setColorAt(1, color('orange', 0))
        painter.fillRect(QRectF(0, height - 1, width, 1), edge)


class OrbitalBackdrop(QWidget):
    """Charcoal workspace with soft atmosphere and precise Saturn ring arcs."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

    def paintEvent(self, event):
        painter = QPainter(self)
        bounds = QRectF(self.rect())
        width, height = bounds.width(), bounds.height()
        painter.fillRect(bounds, color('bg'))

        _glow(painter, QPointF(width * .90, 160), max(340, width * .49), 'cyan', 14)
        _glow(painter, QPointF(width * .22, height * .89), max(280, width * .41), 'orange', 8)

        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.save()
        painter.setClipRect(QRectF(width * .46, 0, width * .54, height))
        painter.translate(width * .92, 142)
        painter.rotate(-26)
        _arc(painter, QRectF(-250, -128, 500, 256), 8, 292, 'cyan', 17)
        _arc(painter, QRectF(-340, -177, 680, 354), 28, 276, 'text', 9)
        _arc(painter, QRectF(-465, -244, 930, 488), -8, 286, 'cyan', 12)
        _arc(painter, QRectF(-610, -326, 1220, 652), 42, 243, 'text', 7)
        painter.restore()
