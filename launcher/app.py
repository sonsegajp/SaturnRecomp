"""Native SaturnRecomp desktop entry point."""
import argparse
import sys
from PySide6.QtGui import QFont, QIcon
from PySide6.QtWidgets import QApplication, QMessageBox
from library import Library, ROOT
from native_ui import MainWindow, STYLE
from theme import native_palette

def main():
    parser = argparse.ArgumentParser(description='SaturnRecomp game library')
    parser.add_argument('--library', help='Use an existing library folder')
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName('SaturnRecomp')
    app.setOrganizationName('SaturnRecomp')
    app.setStyle('Fusion')
    app.setPalette(native_palette())
    app.setFont(QFont('Segoe UI', 10))
    app.setStyleSheet(STYLE)
    app.setWindowIcon(QIcon(str(ROOT / 'assets/saturnrecomp-logo.png')))
    try:
        window = MainWindow(Library(args.library))
        window.show()
    except Exception as error:
        QMessageBox.critical(None, 'SaturnRecomp could not start', str(error))
        return 1
    return app.exec()

if __name__ == '__main__':
    sys.exit(main())
