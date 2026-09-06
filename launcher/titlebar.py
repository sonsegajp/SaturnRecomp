"""Integrated Qt window controls with native Windows sizing and Snap hit tests."""
from __future__ import annotations

import sys

from PySide6.QtCore import QEvent, QPoint, QRect, QRectF, Qt, QTimer
from PySide6.QtGui import QPainter, QPen
from PySide6.QtWidgets import QAbstractButton, QApplication, QHBoxLayout, QMainWindow, QWidget

from theme import METRICS, color

HTCLIENT, HTCAPTION, HTMAXBUTTON = 1, 2, 9
HTLEFT, HTRIGHT, HTTOP, HTTOPLEFT, HTTOPRIGHT = 10, 11, 12, 13, 14
HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT = 15, 16, 17


class CaptionButton(QAbstractButton):
    def __init__(self, action, host):
        super().__init__()
        self.action = action
        self.host = host
        self.native_hover = False
        self.setFixedSize(METRICS['caption_button_width'], METRICS['header_height'])
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.ArrowCursor)
        self.sync_state()

    def sync_state(self):
        name = {'minimize': 'Minimize', 'close': 'Close',
                'maximize': 'Restore' if self.host.isMaximized() else 'Maximize'}[self.action]
        self.setAccessibleName(name + ' window')
        self.setToolTip(name)
        self.update()

    def set_native_hover(self, hovered):
        if hovered != self.native_hover:
            self.native_hover = hovered
            self.update()

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.click()
            event.accept()
        else:
            super().keyPressEvent(event)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        hovered = self.underMouse() or self.native_hover
        if hovered or self.isDown():
            role = 'caption_close' if self.action == 'close' else 'selection' if self.isDown() else 'raised'
            painter.fillRect(self.rect(), color(role))
        if self.hasFocus():
            painter.setPen(QPen(color('accent'), 1))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(QRectF(self.rect()).adjusted(4.5, 10.5, -4.5, -10.5), 3, 3)
        painter.setPen(QPen(color('text' if hovered or self.host.isActiveWindow() else 'secondary'), 1.2))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        x, y = self.width() / 2, self.height() / 2
        if self.action == 'minimize':
            painter.drawLine(QPoint(round(x - 5), round(y)), QPoint(round(x + 5), round(y)))
        elif self.action == 'close':
            painter.drawLine(QPoint(round(x - 4), round(y - 4)), QPoint(round(x + 4), round(y + 4)))
            painter.drawLine(QPoint(round(x + 4), round(y - 4)), QPoint(round(x - 4), round(y + 4)))
        elif self.host.isMaximized():
            painter.drawRect(QRectF(x - 2.5, y - 5.5, 8, 8))
            painter.fillRect(QRectF(x - 5.5, y - 2.5, 8, 8),
                             color('raised' if hovered else 'header'))
            painter.drawRect(QRectF(x - 5.5, y - 2.5, 8, 8))
        else:
            painter.drawRect(QRectF(x - 4.5, y - 4.5, 9, 9))


class WindowControls(QWidget):
    def __init__(self, host):
        super().__init__()
        self.setAccessibleName('Window controls')
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.minimize_button = CaptionButton('minimize', host)
        self.maximize_button = CaptionButton('maximize', host)
        self.close_button = CaptionButton('close', host)
        self.minimize_button.clicked.connect(host.showMinimized)
        self.maximize_button.clicked.connect(host.toggle_maximized)
        self.close_button.clicked.connect(host.close)
        for control in (self.minimize_button, self.maximize_button, self.close_button):
            layout.addWidget(control)

    def sync_state(self):
        for control in (self.minimize_button, self.maximize_button, self.close_button):
            control.sync_state()


class _WindowsApi:
    """Pointer-safe Win32 bindings, loaded only with the Windows Qt platform."""
    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes, self.types = ctypes, wintypes
        self.user = ctypes.windll.user32
        self.user.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
        self.user.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        self.user.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
        self.user.SetWindowLongPtrW.restype = ctypes.c_ssize_t
        self.user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                          ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
        self.user.ScreenToClient.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
        self.user.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
        self.user.MonitorFromWindow.restype = wintypes.HMONITOR
        self.user.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.c_void_p]

        class MonitorInfo(ctypes.Structure):
            _fields_ = [('size', wintypes.DWORD), ('monitor', wintypes.RECT),
                        ('work', wintypes.RECT), ('flags', wintypes.DWORD)]

        class MinMaxInfo(ctypes.Structure):
            _fields_ = [('reserved', wintypes.POINT), ('max_size', wintypes.POINT),
                        ('max_position', wintypes.POINT), ('min_track', wintypes.POINT),
                        ('max_track', wintypes.POINT)]

        class TrackMouseEvent(ctypes.Structure):
            _fields_ = [('size', wintypes.DWORD), ('flags', wintypes.DWORD),
                        ('hwnd', wintypes.HWND), ('hover_time', wintypes.DWORD)]

        self.MonitorInfo, self.MinMaxInfo = MonitorInfo, MinMaxInfo
        self.TrackMouseEvent = TrackMouseEvent
        self.user.TrackMouseEvent.argtypes = [ctypes.POINTER(TrackMouseEvent)]

    def track_caption_leave(self, hwnd):
        tracking = self.TrackMouseEvent(self.ctypes.sizeof(self.TrackMouseEvent),
                                        0x12, hwnd, 0)  # TME_LEAVE | TME_NONCLIENT
        self.user.TrackMouseEvent(self.ctypes.byref(tracking))

    def configure(self, hwnd):
        # Keep Windows' sizing, animation and taskbar semantics. WM_NCCALCSIZE
        # makes the entire frame our client area, including the caption region.
        style = self.user.GetWindowLongPtrW(hwnd, -16)
        style |= 0x00C00000 | 0x00040000 | 0x00080000 | 0x00030000
        self.user.SetWindowLongPtrW(hwnd, -16, style)
        self.user.SetWindowPos(hwnd, None, 0, 0, 0, 0, 0x0037)

    def client_point(self, message, scale):
        point = self.types.POINT(self.ctypes.c_short(message.lParam & 0xffff).value,
                                 self.ctypes.c_short((message.lParam >> 16) & 0xffff).value)
        self.user.ScreenToClient(message.hWnd, self.ctypes.byref(point))
        return QPoint(round(point.x / scale), round(point.y / scale))

    def constrain_maximize(self, message, window):
        monitor = self.user.MonitorFromWindow(message.hWnd, 2)
        info = self.MonitorInfo()
        info.size = self.ctypes.sizeof(info)
        if not monitor or not self.user.GetMonitorInfoW(monitor, self.ctypes.byref(info)):
            return False
        limits = self.MinMaxInfo.from_address(message.lParam)
        limits.max_position.x = info.work.left - info.monitor.left
        limits.max_position.y = info.work.top - info.monitor.top
        limits.max_size.x = info.work.right - info.work.left
        limits.max_size.y = info.work.bottom - info.work.top
        scale = window.devicePixelRatioF()
        limits.min_track.x = round(window.minimumWidth() * scale)
        limits.min_track.y = round(window.minimumHeight() * scale)
        return True


class ChromeWindow(QMainWindow):
    """A frameless main window whose existing header doubles as its title bar."""
    def __init__(self):
        super().__init__()
        self.chrome_header = None
        self.window_controls = None
        self._native_max_pressed = False
        self._styled_hwnd = None
        self._windows_api = (_WindowsApi() if sys.platform == 'win32'
                             and QApplication.platformName() == 'windows' else None)
        self.setWindowFlags(Qt.WindowType.Window | Qt.WindowType.FramelessWindowHint |
                            Qt.WindowType.WindowSystemMenuHint |
                            Qt.WindowType.WindowMinimizeButtonHint |
                            Qt.WindowType.WindowMaximizeButtonHint |
                            Qt.WindowType.WindowCloseButtonHint)

    def install_titlebar(self, header, branding):
        self.chrome_header = header
        header.installEventFilter(self)
        branding.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.window_controls = WindowControls(self)
        return self.window_controls

    def toggle_maximized(self):
        self.showNormal() if self.isMaximized() else self.showMaximized()

    def start_system_move(self):
        handle = self.windowHandle()
        return bool(handle and handle.startSystemMove())

    def hit_test(self, point):
        """Classify Qt client coordinates without stealing widget interactions."""
        if not self.rect().contains(point):
            return HTCLIENT
        if not self.isMaximized() and not self.isFullScreen():
            grip = METRICS['resize_grip']
            left, right = point.x() < grip, point.x() >= self.width() - grip
            top, bottom = point.y() < grip, point.y() >= self.height() - grip
            if top: return HTTOPLEFT if left else HTTOPRIGHT if right else HTTOP
            if bottom: return HTBOTTOMLEFT if left else HTBOTTOMRIGHT if right else HTBOTTOM
            if left: return HTLEFT
            if right: return HTRIGHT
        if not self.chrome_header:
            return HTCLIENT
        header_point = self.chrome_header.mapFrom(self, point)
        if not self.chrome_header.rect().contains(header_point):
            return HTCLIENT
        if self.window_controls:
            maximize = self.window_controls.maximize_button
            if maximize.rect().contains(maximize.mapFrom(self, point)):
                return HTMAXBUTTON
        child = self.chrome_header.childAt(header_point)
        while child and child != self.chrome_header:
            if isinstance(child, QAbstractButton):
                return HTCLIENT
            child = child.parentWidget()
        return HTCAPTION

    def eventFilter(self, watched, event):
        if watched is self.chrome_header:
            if event.type() == QEvent.Type.MouseButtonDblClick and event.button() == Qt.MouseButton.LeftButton:
                self.toggle_maximized()
                return True
            if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
                return self.start_system_move()
        return super().eventFilter(watched, event)

    def showEvent(self, event):
        super().showEvent(event)
        if self._windows_api and self._styled_hwnd != int(self.winId()):
            self._styled_hwnd = int(self.winId())
            self._windows_api.configure(self._styled_hwnd)

    def changeEvent(self, event):
        super().changeEvent(event)
        if self.window_controls and event.type() in (QEvent.Type.WindowStateChange,
                                                     QEvent.Type.ActivationChange):
            self.window_controls.sync_state()
            if not self.isActiveWindow():
                self.window_controls.maximize_button.set_native_hover(False)
                self.window_controls.maximize_button.setDown(False)
                self._native_max_pressed = False

    def nativeEvent(self, event_type, pointer):
        api = getattr(self, '_windows_api', None)
        if not api or bytes(event_type) not in (b'windows_generic_MSG', b'windows_dispatcher_MSG'):
            return super().nativeEvent(event_type, pointer)
        message = api.types.MSG.from_address(int(pointer))
        if message.message == 0x0083:  # WM_NCCALCSIZE: remove the OS caption/frame.
            return True, 0
        if message.message == 0x0024 and api.constrain_maximize(message, self):
            return True, 0  # WM_GETMINMAXINFO: keep maximized controls above taskbar.
        if message.message == 0x0084:  # WM_NCHITTEST, physical screen -> Qt client.
            hit = self.hit_test(api.client_point(message, self.devicePixelRatioF()))
            if self.window_controls:
                if hit == HTMAXBUTTON and not self.window_controls.maximize_button.native_hover:
                    api.track_caption_leave(message.hWnd)
                self.window_controls.maximize_button.set_native_hover(hit == HTMAXBUTTON)
            return True, hit
        if self.window_controls:
            maximize = self.window_controls.maximize_button
            if message.message == 0x00A1 and message.wParam == HTMAXBUTTON:
                self._native_max_pressed = True
                maximize.setDown(True)
                return True, 0
            if message.message in (0x00A2, 0x0202) and self._native_max_pressed:
                self._native_max_pressed = False
                maximize.setDown(False)
                if message.message == 0x00A2 and message.wParam == HTMAXBUTTON:
                    QTimer.singleShot(0, maximize.click)
                return True, 0
            if message.message == 0x02A2:  # WM_NCMOUSELEAVE
                maximize.set_native_hover(False)
            if message.message == 0x001F:  # WM_CANCELMODE
                maximize.setDown(False)
                self._native_max_pressed = False
        return super().nativeEvent(event_type, pointer)
