PS5 HD Camera для macOS
=======================
(English version: README.txt)

Камера PlayStation 5 (CFI-ZEY1) без прошивки не работает как веб-камера: при каждом подключении
в неё нужно загрузить прошивку. Этот пакет ставит фоновую службу, которая делает это сама.

Установка
---------
    sudo bash install.sh

Скрипт спросит режим. Боке (размытие фона по глубине) пока есть только в версии для Windows:
на macOS для него нужна системная камера-расширение, а её macOS запускает только с подписью
разработчика Apple. Поэтому на macOS ставится обычная камера: родные 1920x1080, 30 и 60 к/с.

Нужен Python 3.9 или новее из Command Line Tools (xcode-select --install). Homebrew не нужен:
полный набор зависимостей закреплён и проверяется по SHA-256: PyUSB 1.3.1, libusb-package
1.0.30.0, importlib-resources 6.5.2 и zipp 3.23.1 для Python 3.9.

После установки подключите камеру в порт USB 3 (в USB 2.0 она отдаёт только 640x400) и выберите "USB Camera-OV580" в приложении.

Журнал:   /Library/Logs/PS5Camera.log
Удаление: sudo bash "/Library/Application Support/PS5Camera/uninstall.sh"
